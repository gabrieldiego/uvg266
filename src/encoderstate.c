/*****************************************************************************
 * This file is part of uvg266 VVC encoder.
 *
 * Copyright (c) 2021, Tampere University, ITU/ISO/IEC, project contributors
 * All rights reserved.
 * 
 * Redistribution and use in source and binary forms, with or without modification,
 * are permitted provided that the following conditions are met:
 * 
 * * Redistributions of source code must retain the above copyright notice, this
 *   list of conditions and the following disclaimer.
 * 
 * * Redistributions in binary form must reproduce the above copyright notice, this
 *   list of conditions and the following disclaimer in the documentation and/or
 *   other materials provided with the distribution.
 * 
 * * Neither the name of the Tampere University or ITU/ISO/IEC nor the names of its
 *   contributors may be used to endorse or promote products derived from
 *   this software without specific prior written permission.
 * 
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR
 * ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 * INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 * LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION HOWEVER CAUSED AND ON
 * ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 * INCLUDING NEGLIGENCE OR OTHERWISE ARISING IN ANY WAY OUT OF THE USE OF THIS
 ****************************************************************************/

#include "encoderstate.h"

 // This define is required for M_PI on Windows.
#define _USE_MATH_DEFINES
#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cabac.h"
#include "context.h"
#include "encode_coding_tree.h"
#include "encoder_state-bitstream.h"
#include "filter.h"
#include "hashmap.h"
#include "image.h"
#include "rate_control.h"
#include "sao.h"
#include "search.h"
#include "tables.h"
#include "threads.h"
#include "threadqueue.h"
#include "alf.h"
#include "reshape.h"

#include "strategies/strategies-picture.h"


/**
 * \brief Strength of QP adjustments when using adaptive QP for 360 video.
 *
 * Determined empirically.
 */
static const double ERP_AQP_STRENGTH = 3.0;

int uvg_encoder_state_match_children_of_previous_frame(encoder_state_t * const state) {
  int i;
  for (i = 0; state->children[i].encoder_control; ++i) {
    //Child should also exist for previous encoder
    assert(state->previous_encoder_state->children[i].encoder_control);
    state->children[i].previous_encoder_state = &state->previous_encoder_state->children[i];
    uvg_encoder_state_match_children_of_previous_frame(&state->children[i]);
  }
  return 1;
}

/**
 * \brief Save edge pixels before SAO to buffers.
 *
 * Copies pixels at the edges of the area that will be filtered with SAO to
 * the given buffers. If deblocking is enabled, the pixels must have been
 * deblocked before this.
 *
 * The saved pixels will be needed later when doing SAO for the neighboring
 * areas.
 */
static void encoder_state_recdata_before_sao_to_bufs(
    encoder_state_t * const state,
    const lcu_order_element_t * const lcu,
    yuv_t * const hor_buf,
    yuv_t * const ver_buf)
{
  videoframe_t* const frame = state->tile->frame;

  if (hor_buf && lcu->below) {
    // Copy the bottommost row that will be filtered with SAO to the
    // horizontal buffer.
    vector2d_t pos = {
      .x = lcu->position_px.x,
      .y = lcu->position_px.y + LCU_WIDTH - SAO_DELAY_PX - 1,
    };
    // Copy all pixels that have been deblocked.
    int length = lcu->size.x - DEBLOCK_DELAY_PX;

    if (!lcu->right) {
      // If there is no LCU to the right, the last pixels will be
      // filtered too.
      length += DEBLOCK_DELAY_PX;
    }

    if (lcu->left) {
      // The rightmost pixels of the CTU to the left will also be filtered.
      pos.x -= DEBLOCK_DELAY_PX;
      length += DEBLOCK_DELAY_PX;
    }

    const unsigned from_index = pos.x + pos.y * frame->rec->stride_luma;
    // NOTE: The horizontal buffer is indexed by
    //    x_px + y_lcu * frame->width
    // where x_px is in pixels and y_lcu in number of LCUs.
    const unsigned to_index = pos.x + lcu->position.y * frame->width;

    uvg_pixels_blit(&frame->rec->y[from_index],
                    &hor_buf->y[to_index],
                    length, 1,
                    frame->rec->stride_luma,
                    frame->width);

    if (state->encoder_control->chroma_format != UVG_CSP_400) {
      const unsigned from_index_c = (pos.x >> frame->rec->chroma_scale_x) + (pos.y >> frame->rec->chroma_scale_y) * frame->rec->stride_chroma;
      const unsigned to_index_c = (pos.x >> frame->rec->chroma_scale_x) + lcu->position.y * (frame->width >> frame->rec->chroma_scale_x);

      uvg_pixels_blit(&frame->rec->u[from_index_c],
                      &hor_buf->u[to_index_c],
                      length >> frame->rec->chroma_scale_x, 1,
                      frame->rec->stride_chroma,
                      frame->width >> frame->rec->chroma_scale_x);

      uvg_pixels_blit(&frame->rec->v[from_index_c],
                      &hor_buf->v[to_index_c],
                      length >> frame->rec->chroma_scale_x, 1,
                      frame->rec->stride_chroma,
                      frame->width >> frame->rec->chroma_scale_x);
    }
  }

  if (ver_buf && lcu->right) {
    // Copy the rightmost column that will be filtered with SAO to the
    // vertical buffer.
    vector2d_t pos = {
      .x = lcu->position_px.x + LCU_WIDTH - SAO_DELAY_PX - 1,
      .y = lcu->position_px.y,
    };
    int length = lcu->size.y - DEBLOCK_DELAY_PX;

    if (!lcu->below) {
      // If there is no LCU below, the last pixels will be filtered too.
      length += DEBLOCK_DELAY_PX;
    }

    if (lcu->above) {
      // The bottommost pixels of the CTU above will also be filtered.
      pos.y -= DEBLOCK_DELAY_PX;
      length += DEBLOCK_DELAY_PX;
    }

    const unsigned from_index = pos.x + pos.y * frame->rec->stride_luma;
    // NOTE: The vertical buffer is indexed by
    //    x_lcu * frame->height + y_px
    // where x_lcu is in number of LCUs and y_px in pixels.
    const unsigned to_index = lcu->position.x * frame->height + pos.y;

    uvg_pixels_blit(&frame->rec->y[from_index],
                    &ver_buf->y[to_index],
                    1, length,
                    frame->rec->stride_luma, 1);

    if (state->encoder_control->chroma_format != UVG_CSP_400) {
      const unsigned from_index_c = (pos.x >> frame->rec->chroma_scale_x) + (pos.y >> frame->rec->chroma_scale_y) * frame->rec->stride_chroma;
      const unsigned to_index_c = lcu->position.x * (frame->height >> frame->rec->chroma_scale_y) + (pos.y >> frame->rec->chroma_scale_y);

      uvg_pixels_blit(&frame->rec->u[from_index_c],
                      &ver_buf->u[to_index_c],
                      1, length >> frame->rec->chroma_scale_x,
                      frame->rec->stride_chroma, 1);
      uvg_pixels_blit(&frame->rec->v[from_index_c],
                      &ver_buf->v[to_index_c],
                      1, length >> frame->rec->chroma_scale_x,
                      frame->rec->stride_chroma, 1);
    }
  }
}

static void encoder_state_recdata_to_bufs(encoder_state_t * const state,
                                          const lcu_order_element_t * const lcu,
                                          yuv_t * const hor_buf,
                                          yuv_t * const ver_buf)
{
  videoframe_t* const frame = state->tile->frame;
  
  if (hor_buf) {
    //Copy the bottom row of this LCU to the horizontal buffer
    vector2d_t bottom = { lcu->position_px.x, lcu->position_px.y + lcu->size.y - 1 };
    const int lcu_row = lcu->position.y;

    unsigned from_index = bottom.y * frame->rec->stride_luma + bottom.x;
    unsigned to_index = lcu->position_px.x + lcu_row * frame->width;
    
    uvg_pixels_blit(&frame->rec->y[from_index],
                    &hor_buf->y[to_index],
                    lcu->size.x, 1,
                    frame->rec->stride_luma, frame->width);

    if (state->encoder_control->chroma_format != UVG_CSP_400) {
      unsigned from_index_c = (bottom.y >> frame->rec->chroma_scale_y) * (frame->rec->stride_chroma) + (bottom.x >> frame->rec->chroma_scale_x);
      unsigned to_index_c = (lcu->position_px.x >> frame->rec->chroma_scale_x) + lcu_row * (frame->width >> frame->rec->chroma_scale_x);

      uvg_pixels_blit(&frame->rec->u[from_index_c],
                      &hor_buf->u[to_index_c],
                      lcu->size.x >> frame->rec->chroma_scale_x, 1, 
                      frame->rec->stride_chroma, frame->width >> frame->rec->chroma_scale_x);

      uvg_pixels_blit(&frame->rec->v[from_index_c],
                      &hor_buf->v[to_index_c],
                      lcu->size.x >> frame->rec->chroma_scale_x, 1,
                      frame->rec->stride_chroma, frame->width >> frame->rec->chroma_scale_x);
    }
  }
  
  if (ver_buf) {
    //Copy the right row of this LCU to the vertical buffer.
    
    const int lcu_col = lcu->position.x;
    vector2d_t left = { lcu->position_px.x + lcu->size.x - 1, lcu->position_px.y };
    
    uvg_pixels_blit(&frame->rec->y[left.y * frame->rec->stride_luma + left.x],
                    &ver_buf->y[lcu->position_px.y + lcu_col * frame->height],
                    1, lcu->size.y,
                    frame->rec->stride_luma, 1);

    if (state->encoder_control->chroma_format != UVG_CSP_400) {
      unsigned from_index = (left.y >> frame->rec->chroma_scale_y) * frame->rec->stride_chroma + (left.x >> frame->rec->chroma_scale_x);
      unsigned to_index = (lcu->position_px.y >> frame->rec->chroma_scale_y) + lcu_col * (frame->height >> frame->rec->chroma_scale_y);

      uvg_pixels_blit(&frame->rec->u[from_index],
                      &ver_buf->u[to_index],
                      1, lcu->size.y >> frame->rec->chroma_scale_y,
                      frame->rec->stride_chroma, 1);

      uvg_pixels_blit(&frame->rec->v[from_index],
                      &ver_buf->v[to_index],
                      1, lcu->size.y >> frame->rec->chroma_scale_y,
                      frame->rec->stride_chroma, 1);
    }
  }

  // Fill IBC buffer
  if (state->encoder_control->cfg.ibc) {

    uint32_t ibc_buffer_pos_x = lcu->position_px.x + LCU_WIDTH >= IBC_BUFFER_WIDTH ? IBC_BUFFER_WIDTH - LCU_WIDTH: lcu->position_px.x;
    uint32_t ibc_buffer_pos_x_c = ibc_buffer_pos_x >> frame->rec->chroma_scale_x;
    uint32_t ibc_buffer_row     = lcu->position_px.y / LCU_WIDTH;

    // If the buffer is full shift all the lines LCU_WIDTH left
    if (lcu->position_px.x + LCU_WIDTH > IBC_BUFFER_WIDTH) {
      for (uint32_t i = 0; i < LCU_WIDTH; i++) {
        memmove(
          &frame->ibc_buffer_y[ibc_buffer_row][i * IBC_BUFFER_WIDTH],
          &frame->ibc_buffer_y[ibc_buffer_row][i * IBC_BUFFER_WIDTH + LCU_WIDTH],
          sizeof(uvg_pixel) * (IBC_BUFFER_WIDTH - LCU_WIDTH));
      }
      if (state->encoder_control->chroma_format != UVG_CSP_400) {
        for (uint32_t i = 0; i < LCU_WIDTH_C; i++) {
          memmove(
            &frame->ibc_buffer_u[ibc_buffer_row][i * IBC_BUFFER_WIDTH_C],
            &frame->ibc_buffer_u[ibc_buffer_row]
                                [i * IBC_BUFFER_WIDTH_C + LCU_WIDTH_C],
            sizeof(uvg_pixel) * (IBC_BUFFER_WIDTH_C - LCU_WIDTH_C));
          memmove(
            &frame->ibc_buffer_v[ibc_buffer_row][i * IBC_BUFFER_WIDTH_C],
            &frame->ibc_buffer_v[ibc_buffer_row]
                                [i * IBC_BUFFER_WIDTH_C + LCU_WIDTH_C],
            sizeof(uvg_pixel) * (IBC_BUFFER_WIDTH_C - LCU_WIDTH_C));
        }
      }
    }

    const uint32_t ibc_block_width = MIN(LCU_WIDTH, (state->tile->frame->width-lcu->position_px.x));
    const uint32_t ibc_block_height = MIN(LCU_WIDTH, (state->tile->frame->height-lcu->position_px.y));

    uvg_pixels_blit(&frame->rec->y[lcu->position_px.y * frame->rec->stride_luma + lcu->position_px.x],
                    &frame->ibc_buffer_y[ibc_buffer_row][ibc_buffer_pos_x],
                    ibc_block_width, ibc_block_height,
                    frame->rec->stride_luma, IBC_BUFFER_WIDTH);

    if (state->encoder_control->chroma_format != UVG_CSP_400) {
       uvg_pixels_blit(&frame->rec->u[(lcu->position_px.y >> frame->rec->chroma_scale_y) * (frame->rec->stride_chroma) + (lcu->position_px.x >> frame->rec->chroma_scale_x)],
                       &frame->ibc_buffer_u[ibc_buffer_row][ibc_buffer_pos_x_c],
                       ibc_block_width >> frame->rec->chroma_scale_x, ibc_block_height >> frame->rec->chroma_scale_y,
                       frame->rec->stride_chroma, IBC_BUFFER_WIDTH_C);

       uvg_pixels_blit(&frame->rec->v[(lcu->position_px.y >> frame->rec->chroma_scale_y) * (frame->rec->stride_chroma) + (lcu->position_px.x >> frame->rec->chroma_scale_x)],
                       &frame->ibc_buffer_v[ibc_buffer_row][ibc_buffer_pos_x_c],
                       ibc_block_width >> frame->rec->chroma_scale_x, ibc_block_height >> frame->rec->chroma_scale_y,
                       frame->rec->stride_chroma, IBC_BUFFER_WIDTH_C);

     }
  }
  
}

/**
 * \brief Do SAO reconstuction for all available pixels.
 *
 * Does SAO reconstruction for all pixels that are available after the
 * given LCU has been deblocked. This means the following pixels:
 *  - bottom-right block of SAO_DELAY_PX times SAO_DELAY_PX in the lcu to
 *    the left and up
 *  - the rightmost SAO_DELAY_PX pixels of the LCU to the left (excluding
 *    the bottommost pixel)
 *  - the bottommost SAO_DELAY_PX pixels of the LCU above (excluding the
 *    rightmost pixels)
 *  - all pixels inside the LCU, excluding the rightmost SAO_DELAY_PX and
 *    bottommost SAO_DELAY_PX
 */
static void encoder_sao_reconstruct(const encoder_state_t *const state,
                                    const lcu_order_element_t *const lcu)
{
  videoframe_t *const frame = state->tile->frame;


  // Temporary buffers for SAO input pixels. The buffers cover the pixels
  // inside the LCU (LCU_WIDTH x LCU_WIDTH), SAO_DELAY_PX wide bands to the
  // left and above the LCU, and one pixel border on the left and top
  // sides. We add two extra pixels to the buffers because the AVX2 SAO
  // reconstruction reads up to two extra bytes when using edge SAO in the
  // horizontal direction.
#define SAO_BUF_WIDTH   (1 + SAO_DELAY_PX   + LCU_WIDTH)
#define SAO_BUF_WIDTH_C (1 + SAO_DELAY_PX/2 + LCU_WIDTH_C)
  uvg_pixel sao_buf_y_array[SAO_BUF_WIDTH   * SAO_BUF_WIDTH   + 2];
  uvg_pixel sao_buf_u_array[SAO_BUF_WIDTH_C * SAO_BUF_WIDTH_C + 2];
  uvg_pixel sao_buf_v_array[SAO_BUF_WIDTH_C * SAO_BUF_WIDTH_C + 2];

  // Pointers to the top-left pixel of the LCU in the buffers.
  uvg_pixel *const sao_buf_y = &sao_buf_y_array[(SAO_DELAY_PX + 1) * (SAO_BUF_WIDTH + 1)];
  uvg_pixel *const sao_buf_u = &sao_buf_u_array[(SAO_DELAY_PX/2 + 1) * (SAO_BUF_WIDTH_C + 1)];
  uvg_pixel *const sao_buf_v = &sao_buf_v_array[(SAO_DELAY_PX/2 + 1) * (SAO_BUF_WIDTH_C + 1)];

  const int x_offsets[3] = {
    // If there is an lcu to the left, we need to filter its rightmost
    // pixels.
    lcu->left ? -SAO_DELAY_PX : 0,
    0,
    // If there is an lcu to the right, the rightmost pixels of this LCU
    // are filtered when filtering that LCU. Otherwise we filter them now.
    lcu->size.x - (lcu->right ? SAO_DELAY_PX : 0),
  };

  const int y_offsets[3] = {
    // If there is an lcu above, we need to filter its bottommost pixels.
    lcu->above ? -SAO_DELAY_PX : 0,
    0,
    // If there is an lcu below, the bottommost pixels of this LCU are
    // filtered when filtering that LCU. Otherwise we filter them now.
    lcu->size.y - (lcu->below ? SAO_DELAY_PX : 0),
  };

  // Number of pixels around the block that need to be copied to the
  // buffers.
  const int border_left  = lcu->left  ? 1 : 0;
  const int border_right = lcu->right ? 1 : 0;
  const int border_above = lcu->above ? 1 : 0;
  const int border_below = lcu->below ? 1 : 0;

  // Index of the pixel at the intersection of the top and left borders.
  const int border_index = (x_offsets[0] - border_left) +
                           (y_offsets[0] - border_above) * SAO_BUF_WIDTH;
  const int border_index_c = (x_offsets[0]/2 - border_left) +
                             (y_offsets[0]/2 - border_above) * SAO_BUF_WIDTH_C;
  // Width and height of the whole area to filter.
  const int width  = x_offsets[2] - x_offsets[0];
  const int height = y_offsets[2] - y_offsets[0];

  // Copy bordering pixels from above and left to buffers.
  if (lcu->above) {
    const int from_index = (lcu->position_px.x + x_offsets[0] - border_left) +
                           (lcu->position.y - 1) * frame->width;
    uvg_pixels_blit(&state->tile->hor_buf_before_sao->y[from_index],
                    &sao_buf_y[border_index],
                    width + border_left + border_right,
                    1,
                    frame->width,
                    SAO_BUF_WIDTH);
    if (state->encoder_control->chroma_format != UVG_CSP_400) {
      const int from_index_c = ((lcu->position_px.x + x_offsets[0]) >> frame->rec->chroma_scale_x) - border_left +
                               (lcu->position.y - 1) * (frame->width >> frame->rec->chroma_scale_x);
      uvg_pixels_blit(&state->tile->hor_buf_before_sao->u[from_index_c],
                      &sao_buf_u[border_index_c],
                      (width >> frame->rec->chroma_scale_x) + border_left + border_right,
                      1,
                      frame->width >> frame->rec->chroma_scale_x,
                      SAO_BUF_WIDTH_C);
      uvg_pixels_blit(&state->tile->hor_buf_before_sao->v[from_index_c],
                      &sao_buf_v[border_index_c],
                      (width >> frame->rec->chroma_scale_x) + border_left + border_right,
                      1,
                      frame->width >> frame->rec->chroma_scale_x,
                      SAO_BUF_WIDTH_C);
    }
  }
  if (lcu->left) {
    const int from_index = (lcu->position.x - 1) * frame->height +
                           (lcu->position_px.y + y_offsets[0] - border_above);
    uvg_pixels_blit(&state->tile->ver_buf_before_sao->y[from_index],
                    &sao_buf_y[border_index],
                    1,
                    height + border_above + border_below,
                    1,
                    SAO_BUF_WIDTH);
    if (state->encoder_control->chroma_format != UVG_CSP_400) {
      const int from_index_c = ((lcu->position.x - 1) * frame->height >> frame->rec->chroma_scale_y) +
                               ((lcu->position_px.y + y_offsets[0]) >> frame->rec->chroma_scale_y) - border_above;
      uvg_pixels_blit(&state->tile->ver_buf_before_sao->u[from_index_c],
                      &sao_buf_u[border_index_c],
                      1,
                      (height >> frame->rec->chroma_scale_y) + border_above + border_below,
                      1,
                      SAO_BUF_WIDTH_C);
      uvg_pixels_blit(&state->tile->ver_buf_before_sao->v[from_index_c],
                      &sao_buf_v[border_index_c],
                      1,
                      (height >> frame->rec->chroma_scale_y) + border_above + border_below,
                      1,
                      SAO_BUF_WIDTH_C);
    }
  }
  // Copy pixels that will be filtered and bordering pixels from right and
  // below.
  const int from_index = (lcu->position_px.x + x_offsets[0]) +
                         (lcu->position_px.y + y_offsets[0]) * frame->rec->stride_luma;
  const int to_index = x_offsets[0] + y_offsets[0] * SAO_BUF_WIDTH;
  uvg_pixels_blit(&frame->rec->y[from_index],
                  &sao_buf_y[to_index],
                  width + border_right,
                  height + border_below,
                  frame->rec->stride_luma,
                  SAO_BUF_WIDTH);
  if (state->encoder_control->chroma_format != UVG_CSP_400) {
    const int from_index_c = ((lcu->position_px.x + x_offsets[0]) >> frame->rec->chroma_scale_x) +
                             ((lcu->position_px.y + y_offsets[0]) >> frame->rec->chroma_scale_y) * frame->rec->stride_chroma;
    const int to_index_c = (x_offsets[0] >> frame->rec->chroma_scale_x) + (y_offsets[0] >> frame->rec->chroma_scale_y) * SAO_BUF_WIDTH_C;

    uvg_pixels_blit(&frame->rec->u[from_index_c],
                    &sao_buf_u[to_index_c],
                    (width >> frame->rec->chroma_scale_x) + border_right,
                    (height >> frame->rec->chroma_scale_y) + border_below,
                    frame->rec->stride_chroma,
                    SAO_BUF_WIDTH_C);

    uvg_pixels_blit(&frame->rec->v[from_index_c],
                    &sao_buf_v[to_index_c],
                    (width >> frame->rec->chroma_scale_x) + border_right,
                    (height >> frame->rec->chroma_scale_y) + border_below,
                    frame->rec->stride_chroma,
                    SAO_BUF_WIDTH_C);
  }

  // We filter the pixels in four parts:
  //  1. Pixels that belong to the LCU above and to the left
  //  2. Pixels that belong to the LCU above
  //  3. Pixels that belong to the LCU to the left
  //  4. Pixels that belong to the current LCU
  for (int y_offset_index = 0; y_offset_index < 2; y_offset_index++) {
    for (int x_offset_index = 0; x_offset_index < 2; x_offset_index++) {
      const int x = x_offsets[x_offset_index];
      const int y = y_offsets[y_offset_index];
      const int width = x_offsets[x_offset_index + 1] - x;
      const int height = y_offsets[y_offset_index + 1] - y;

      if (width == 0 || height == 0) continue;

      const int lcu_x = (lcu->position_px.x + x) >> LOG2_LCU_WIDTH;
      const int lcu_y = (lcu->position_px.y + y) >> LOG2_LCU_WIDTH;
      const int lcu_index = lcu_x + lcu_y * frame->width_in_lcu;
      const sao_info_t *sao_luma   = &frame->sao_luma[lcu_index];
      const sao_info_t *sao_chroma = &frame->sao_chroma[lcu_index];

      uvg_sao_reconstruct(state,
                          &sao_buf_y[x + y * SAO_BUF_WIDTH],
                          SAO_BUF_WIDTH,
                          lcu->position_px.x + x,
                          lcu->position_px.y + y,
                          width,
                          height,
                          sao_luma,
                          COLOR_Y);

      if (state->encoder_control->chroma_format != UVG_CSP_400) {
        // Coordinates in chroma pixels.
        int x_c = x >> frame->rec->chroma_scale_x;
        int y_c = y >> frame->rec->chroma_scale_y;

        uvg_sao_reconstruct(state,
                            &sao_buf_u[x_c + y_c * SAO_BUF_WIDTH_C],
                            SAO_BUF_WIDTH_C,
                            (lcu->position_px.x >> frame->rec->chroma_scale_x) + x_c,
                            (lcu->position_px.y >> frame->rec->chroma_scale_y) + y_c,
                            width >> frame->rec->chroma_scale_x,
                            height >> frame->rec->chroma_scale_y,
                            sao_chroma,
                            COLOR_U);
        uvg_sao_reconstruct(state,
                            &sao_buf_v[x_c + y_c * SAO_BUF_WIDTH_C],
                            SAO_BUF_WIDTH_C,
                            (lcu->position_px.x >> frame->rec->chroma_scale_x) + x_c,
                            (lcu->position_px.y >> frame->rec->chroma_scale_y) + y_c,
                            width >> frame->rec->chroma_scale_x,
                            height >> frame->rec->chroma_scale_y,
                            sao_chroma,
                            COLOR_V);
      }
    }
  }
}

static void encode_sao_color(encoder_state_t * const state, sao_info_t *sao,
                             color_t color_i)
{
  cabac_data_t * const cabac = &state->cabac;
  sao_eo_cat i;
  int offset_index = (color_i == COLOR_V) ? 5 : 0;

  // Skip colors with no SAO.
  //FIXME: for now, we always have SAO for all channels
  if (color_i == COLOR_Y && 0) return;
  if (color_i != COLOR_Y && 0) return;

  /// sao_type_idx_luma:   TR, cMax = 2, cRiceParam = 0, bins = {0, bypass}
  /// sao_type_idx_chroma: TR, cMax = 2, cRiceParam = 0, bins = {0, bypass}
  // Encode sao_type_idx for Y and U+V.
  if (color_i != COLOR_V) {
    cabac->cur_ctx = &(cabac->ctx.sao_type_idx_model);
    CABAC_BIN(cabac, sao->type != SAO_TYPE_NONE, "sao_type_idx");
    if (sao->type == SAO_TYPE_BAND) {
      CABAC_BIN_EP(cabac, 0, "sao_type_idx_ep");
    } else if (sao->type == SAO_TYPE_EDGE) {
      CABAC_BIN_EP(cabac, 1, "sao_type_idx_ep");
    }
  }

  if (sao->type == SAO_TYPE_NONE) return;

  /// sao_offset_abs[][][][]: TR, cMax = (1 << (Min(bitDepth, 10) - 5)) - 1,
  ///                         cRiceParam = 0, bins = {bypass x N}
  for (i = SAO_EO_CAT1; i <= SAO_EO_CAT4; ++i) {
    uvg_cabac_write_unary_max_symbol_ep(cabac, abs(sao->offsets[i + offset_index]), SAO_ABS_OFFSET_MAX);
  }

  /// sao_offset_sign[][][][]: FL, cMax = 1, bins = {bypass}
  /// sao_band_position[][][]: FL, cMax = 31, bins = {bypass x N}
  /// sao_eo_class_luma:       FL, cMax = 3, bins = {bypass x 3}
  /// sao_eo_class_chroma:     FL, cMax = 3, bins = {bypass x 3}
  if (sao->type == SAO_TYPE_BAND) {
    for (i = SAO_EO_CAT1; i <= SAO_EO_CAT4; ++i) {
      // Positive sign is coded as 0.
      if (sao->offsets[i + offset_index] != 0) {
        CABAC_BIN_EP(cabac, sao->offsets[i + offset_index] < 0 ? 1 : 0, "sao_offset_sign");
      }
    }
    // TODO: sao_band_position
    // FL cMax=31 (5 bits)
    CABAC_BINS_EP(cabac, sao->band_position[color_i == COLOR_V ? 1:0], 5, "sao_band_position");
  } else if (color_i != COLOR_V) {
    CABAC_BINS_EP(cabac, sao->eo_class, 2, "sao_eo_class");
  }
}

static void encode_sao_merge_flags(encoder_state_t * const state, sao_info_t *sao, unsigned x_ctb, unsigned y_ctb)
{
  cabac_data_t * const cabac = &state->cabac;
  // SAO merge flags are not present for the first row and column.
  if (x_ctb > 0) {
    cabac->cur_ctx = &(cabac->ctx.sao_merge_flag_model);
    CABAC_BIN(cabac, sao->merge_left_flag, "sao_merge_left_flag");
  }
  if (y_ctb > 0 && !sao->merge_left_flag) {
    cabac->cur_ctx = &(cabac->ctx.sao_merge_flag_model);
    CABAC_BIN(cabac, sao->merge_up_flag, "sao_merge_up_flag");
  }
}


/**
 * \brief Encode SAO information.
 */
static void encode_sao(encoder_state_t * const state,
                       unsigned x_lcu, uint16_t y_lcu,
                       sao_info_t *sao_luma, sao_info_t *sao_chroma)
{
  // TODO: transmit merge flags outside sao_info
  encode_sao_merge_flags(state, sao_luma, x_lcu, y_lcu);

  // If SAO is merged, nothing else needs to be coded.
  if (!sao_luma->merge_left_flag && !sao_luma->merge_up_flag) {
    encode_sao_color(state, sao_luma, COLOR_Y);
    if (state->encoder_control->chroma_format != UVG_CSP_400) {
      encode_sao_color(state, sao_chroma, COLOR_U);
      encode_sao_color(state, sao_chroma, COLOR_V);
    }
  }
}


/**
 * \brief Sets the QP for each CU in state->tile->frame->cu_array.
 *
 * The QPs are used in deblocking and QP prediction.
 *
 * The QP delta for a quantization group is coded when the first CU with
 * coded block flag set is encountered. Hence, for the purposes of
 * deblocking and QP prediction, all CUs in before the first one that has
 * cbf set use the QP predictor and all CUs after that use (QP predictor
 * + QP delta).
 *
 * \param state           encoder state
 * \param x               x-coordinate of the left edge of the root CU
 * \param y               y-coordinate of the top edge of the root CU
 * \param depth           depth in the CU quadtree
 * \param last_qp         QP of the last CU in the last quantization group
 * \param prev_qp         -1 if QP delta has not been coded in current QG,
 *                        otherwise the QP of the current QG
 */
static void set_cu_qps(encoder_state_t *state, const cu_loc_t* const cu_loc, int *last_qp, int *prev_qp, const
                       int depth)
{

  // Stop recursion if the CU is completely outside the frame.
  if (cu_loc->x >= state->tile->frame->width || cu_loc->y >= state->tile->frame->height) return;

  cu_info_t *cu = uvg_cu_array_at(state->tile->frame->cu_array, cu_loc->x, cu_loc->y);
  const int width = 1 << cu->log2_width;

  if (depth <= state->frame->max_qp_delta_depth) {
    *prev_qp = -1;
  }

  if (cu_loc->width > width) {
    // Recursively process sub-CUs.
    const int half_width = cu_loc->width >> state->tile->frame->source->chroma_scale_x;
    const int half_height = cu_loc->height >> state->tile->frame->source->chroma_scale_y;
    cu_loc_t split_cu_loc;
    uvg_cu_loc_ctor(&split_cu_loc, cu_loc->x, cu_loc->y, half_width, half_height);
    set_cu_qps(state, &split_cu_loc,     last_qp,     prev_qp, depth + 1);
    uvg_cu_loc_ctor(&split_cu_loc, cu_loc->x + half_width, cu_loc->y, half_width, half_height);
    set_cu_qps(state, &split_cu_loc, last_qp,     prev_qp, depth + 1);
    uvg_cu_loc_ctor(&split_cu_loc, cu_loc->x, cu_loc->y + half_height, half_width, half_height);
    set_cu_qps(state, &split_cu_loc,     last_qp, prev_qp, depth + 1);
    uvg_cu_loc_ctor(&split_cu_loc, cu_loc->x + half_width, cu_loc->y + half_height, half_width, half_height);
    set_cu_qps(state, &split_cu_loc, last_qp, prev_qp, depth + 1);

  } else {
    bool cbf_found = *prev_qp >= 0;

    int y_limit = cu_loc->y + cu_loc->height;
    int x_limit = cu_loc->x + cu_loc->width;
    if (cu_loc->width > TR_MAX_WIDTH || cu_loc->height > TR_MAX_WIDTH) {
      // The CU is split into smaller transform units. Check whether coded
      // block flag is set for any of the TUs.
      const int tu_width = MIN(TR_MAX_WIDTH, 1 << cu->log2_width);
      for (int y_scu = cu_loc->y; !cbf_found && y_scu < y_limit; y_scu += tu_width) {
        for (int x_scu = cu_loc->x; !cbf_found && x_scu < x_limit; x_scu += tu_width) {
          cu_info_t *tu = uvg_cu_array_at(state->tile->frame->cu_array, x_scu, y_scu);
          if (cbf_is_set_any(tu->cbf)) {
            cbf_found = true;
          }
        }
      }
    } else if (cbf_is_set_any(cu->cbf)) {
      cbf_found = true;
    }

    int8_t qp;
    if (cbf_found) {
      *prev_qp = qp = cu->qp;
    } else {
      qp = uvg_get_cu_ref_qp(state, cu_loc->x, cu_loc->y, *last_qp);
    }

    // Set the correct QP for all state->tile->frame->cu_array elements in
    // the area covered by the CU.
    for (int y_scu = cu_loc->y; y_scu < y_limit; y_scu += SCU_WIDTH) {
      for (int x_scu = cu_loc->x; x_scu < x_limit; x_scu += SCU_WIDTH) {
        uvg_cu_array_at(state->tile->frame->cu_array, x_scu, y_scu)->qp = qp;
      }
    }

    if (is_last_cu_in_qg(state, cu_loc)) {
      *last_qp = cu->qp;
    }
  }
}


static void set_joint_cb_cr_modes(encoder_state_t* state, uvg_picture* pic)
{
  bool              sgnFlag = true;

  if (state->encoder_control->chroma_format != UVG_CSP_400)
  {
    const int32_t   x1 = (pic->width_chroma) - 1;
    const int32_t   y1 = (pic->height_chroma) - 1;
    const int32_t   cbs = pic->stride_chroma;
    const int32_t   crs = pic->stride_chroma;
    const uvg_pixel* p_cb = pic->u + 1 * cbs;
    const uvg_pixel* p_cr = pic->v + 1 * crs;
    int64_t         sum_cb_cr = 0;

    // determine inter-chroma transform sign from correlation between high-pass filtered (i.e., zero-mean) Cb and Cr planes
    for (int y = 1; y < y1; y++, p_cb += cbs, p_cr += crs)
    {
      for (int x = 1; x < x1; x++)
      {
        int cb = (12 * (int)p_cb[x] - 2 * ((int)p_cb[x - 1] + (int)p_cb[x + 1] + (int)p_cb[x - cbs] + (int)p_cb[x + cbs]) - ((int)p_cb[x - 1 - cbs] + (int)p_cb[x + 1 - cbs] + (int)p_cb[x - 1 + cbs] + (int)p_cb[x + 1 + cbs]));
        int cr = (12 * (int)p_cr[x] - 2 * ((int)p_cr[x - 1] + (int)p_cr[x + 1] + (int)p_cr[x - crs] + (int)p_cr[x + crs]) - ((int)p_cr[x - 1 - crs] + (int)p_cr[x + 1 - crs] + (int)p_cr[x - 1 + crs] + (int)p_cr[x + 1 + crs]));
        sum_cb_cr += cb * cr;
      }
    }

    sgnFlag = (sum_cb_cr < 0);
  }

  state->frame->jccr_sign = sgnFlag;
}

static void encoder_state_worker_encode_lcu_bitstream(void* opaque);

static void encoder_state_worker_encode_lcu_search(void * opaque)
{
  lcu_order_element_t * const lcu = opaque;
  encoder_state_t *state = lcu->encoder_state;
  const encoder_control_t * const encoder = state->encoder_control;

  switch (encoder->cfg.rc_algorithm) {
  case UVG_NO_RC:
  case UVG_LAMBDA:
    uvg_set_lcu_lambda_and_qp(state, lcu->position);
    break;
  case UVG_OBA:
    uvg_set_ctu_qp_lambda(state, lcu->position);
    break;
  default:
    assert(0);
  }

  lcu->coeff = calloc(1, sizeof(lcu_coeff_t));

  const uint32_t ctu_row = (lcu->position_px.y >> LOG2_LCU_WIDTH);
  const uint32_t ctu_row_mul_five = ctu_row * MAX_NUM_HMVP_CANDS;

  cu_info_t original_lut[MAX_NUM_HMVP_CANDS];
  uint8_t original_lut_size = state->tile->frame->hmvp_size[ctu_row];
  cu_info_t original_lut_ibc[MAX_NUM_HMVP_CANDS];
  uint8_t original_lut_size_ibc = state->tile->frame->hmvp_size_ibc[ctu_row];

  // Store original HMVP lut before search and restore after, since it's modified
  if(state->frame->slicetype != UVG_SLICE_I) memcpy(original_lut, &state->tile->frame->hmvp_lut[ctu_row_mul_five], sizeof(cu_info_t) * MAX_NUM_HMVP_CANDS);
  if(state->encoder_control->cfg.ibc) memcpy(original_lut_ibc, &state->tile->frame->hmvp_lut_ibc[ctu_row_mul_five], sizeof(cu_info_t) * MAX_NUM_HMVP_CANDS);


  if (state->encoder_control->cfg.ibc & 2) {
    videoframe_t * const frame      = state->tile->frame;
    const uint32_t ibc_block_width  = MIN(LCU_WIDTH, (state->tile->frame->width-lcu->position_px.x));
    const uint32_t ibc_block_height = MIN(LCU_WIDTH, (state->tile->frame->height-lcu->position_px.y));
    int items = 0;
    // Hash the current LCU to the IBC hashmap
    for (int32_t xx = 0; xx < (int32_t)(ibc_block_width)-7; xx+=UVG_HASHMAP_BLOCKSIZE >> frame->rec->chroma_scale_x) {
      for (int32_t yy = 0; yy < (int32_t)(ibc_block_height)-7; yy+=UVG_HASHMAP_BLOCKSIZE >> frame->rec->chroma_scale_y) {
        int cur_x = lcu->position_px.x + xx;
        int cur_y = lcu->position_px.y + yy;
        
        // Skip blocks that seem to be the same value for the whole block
        uint64_t first_line =
          *(uint64_t *)&frame->source->y[cur_y * frame->source->stride_luma + cur_x];
        bool same_data = true;
        for (int y_temp = 1; y_temp < 8; y_temp++) {
          if (*(uint64_t *)&frame->source->y[(cur_y+y_temp) * frame->source->stride_luma + cur_x] != first_line) {
            same_data = false;
            break;
          }
        }
        
        if (!same_data || (xx % UVG_HASHMAP_BLOCKSIZE == 0 && yy % UVG_HASHMAP_BLOCKSIZE == 0)) {
          uint32_t crc = uvg_crc32c_8x8(&frame->source->y[cur_y * frame->source->stride_luma + cur_x],frame->source->stride_luma);
          if (state->encoder_control->chroma_format != UVG_CSP_400) {
            crc += uvg_crc32c_4x4(&frame->source->u[(cur_y >> frame->rec->chroma_scale_y) * (frame->source->stride_chroma) + (cur_x >> frame->rec->chroma_scale_x)],frame->source->stride_chroma);
            crc += uvg_crc32c_4x4(&frame->source->v[(cur_y >> frame->rec->chroma_scale_y) * (frame->source->stride_chroma) + (cur_x >> frame->rec->chroma_scale_x)],frame->source->stride_chroma);
          }
          if (xx % UVG_HASHMAP_BLOCKSIZE == 0 && yy % UVG_HASHMAP_BLOCKSIZE == 0) {
            state->tile->frame->ibc_hashmap_pos_to_hash[(cur_y / UVG_HASHMAP_BLOCKSIZE)*state->tile->frame->ibc_hashmap_pos_to_hash_stride + cur_x / UVG_HASHMAP_BLOCKSIZE] = crc;
          }
          uvg_hashmap_insert(frame->ibc_hashmap_row[ctu_row], crc, ((cur_x&0xffff)<<16) | (cur_y&0xffff));
          items++;
        }
      }
    }
  }
  //fprintf(stderr, "Inserted %d items to %dx%d at %dx%d\r\n", items, ibc_block_width, ibc_block_height, lcu->position_px.x, lcu->position_px.y);


  //This part doesn't write to bitstream, it's only search, deblock and sao
  uvg_search_lcu(state, lcu->position_px.x, lcu->position_px.y, state->tile->hor_buf_search, state->tile->ver_buf_search, lcu->coeff);

  if(state->frame->slicetype != UVG_SLICE_I) {
    memcpy(&state->tile->frame->hmvp_lut[ctu_row_mul_five], original_lut, sizeof(cu_info_t) * MAX_NUM_HMVP_CANDS);
    state->tile->frame->hmvp_size[ctu_row] = original_lut_size;
  }
  if (state->encoder_control->cfg.ibc) {
    memcpy(&state->tile->frame->hmvp_lut_ibc[ctu_row_mul_five], original_lut_ibc, sizeof(cu_info_t) * MAX_NUM_HMVP_CANDS);
    state->tile->frame->hmvp_size_ibc[ctu_row] = original_lut_size_ibc;
  }

  encoder_state_recdata_to_bufs(state, lcu, state->tile->hor_buf_search, state->tile->ver_buf_search);

  if (state->frame->max_qp_delta_depth >= 0) {
    int last_qp = state->last_qp;
    int prev_qp = -1;
    cu_loc_t cu_loc;
    uvg_cu_loc_ctor(&cu_loc, lcu->position_px.x, lcu->position_px.y, LCU_WIDTH, LCU_WIDTH);
    set_cu_qps(state, &cu_loc, &last_qp, &prev_qp, 0);
  }

  if (state->tile->frame->lmcs_aps->m_sliceReshapeInfo.sliceReshaperEnableFlag) {
    uvg_pixel* luma = &state->tile->frame->rec->y[lcu->position_px.x + lcu->position_px.y * state->tile->frame->rec->stride_luma];
    for (int y = 0; y < LCU_WIDTH; y++) {
      if (lcu->position_px.y+y < state->tile->frame->rec->height_luma) {
        for (int x = 0; x < LCU_WIDTH; x++) {
          if (lcu->position_px.x+x < state->tile->frame->rec->width_luma) luma[x] = state->tile->frame->lmcs_aps->m_invLUT[luma[x]];
        }
      }
      luma += state->tile->frame->rec->stride_luma;
    }
  }

  if (encoder->cfg.deblock_enable) {
    uvg_filter_deblock_lcu(state, lcu->position_px.x, lcu->position_px.y);
  }

  if (encoder->cfg.sao_type) {
    // Save the post-deblocking but pre-SAO pixels of the LCU to a buffer
    // so that they can be used in SAO reconstruction later.
    encoder_state_recdata_before_sao_to_bufs(state,
      lcu,
      state->tile->hor_buf_before_sao,
      state->tile->ver_buf_before_sao);
    uvg_sao_search_lcu(state, lcu->position.x, lcu->position.y);
    encoder_sao_reconstruct(state, lcu);
  }

  // Do simulated bitstream writing to update the cabac contexts
  if (encoder->cfg.alf_type) {
    state->cabac.only_count = 1;
    encoder_state_worker_encode_lcu_bitstream(opaque);
  }
}

static void encoder_state_worker_encode_lcu_bitstream(void * opaque)
{
  lcu_order_element_t * const lcu = opaque;
  encoder_state_t *state = lcu->encoder_state;
  const encoder_control_t * const encoder = state->encoder_control;
  videoframe_t* const frame = state->tile->frame;

  //Now write data to bitstream (required to have a correct CABAC state)
  const uint64_t existing_bits = uvg_bitstream_tell(&state->stream);

  //Encode SAO
  state->cabac.update = 1;
  if (encoder->cfg.sao_type) {
    encode_sao(state, lcu->position.x, lcu->position.y, &frame->sao_luma[lcu->position.y * frame->width_in_lcu + lcu->position.x], &frame->sao_chroma[lcu->position.y * frame->width_in_lcu + lcu->position.x]);
  }

  //Encode ALF
  uvg_encode_alf_bits(state, lcu->position.y * frame->width_in_lcu + lcu->position.x);

  enum uvg_tree_type tree_type = state->frame->slicetype == UVG_SLICE_I && state->encoder_control->cfg.dual_tree ? UVG_LUMA_T : UVG_BOTH_T;
  //Encode coding tree
  cu_loc_t start;
  uvg_cu_loc_ctor(&start, lcu->position.x * LCU_WIDTH, lcu->position.y * LCU_WIDTH, LCU_WIDTH, LCU_WIDTH);
  split_tree_t split_tree = { 0, MODE_TYPE_ALL, 0, 0, 0, 0 };

  uvg_encode_coding_tree(state, lcu->coeff, tree_type,&start, &start, split_tree, true);

  if(tree_type == UVG_LUMA_T && state->encoder_control->chroma_format != UVG_CSP_400) {
    uvg_cu_loc_ctor(&start, lcu->position.x * LCU_WIDTH, lcu->position.y * LCU_WIDTH, LCU_WIDTH, LCU_WIDTH);
    cu_loc_t chroma_tree_loc = start;
    uvg_encode_coding_tree(state, lcu->coeff, UVG_CHROMA_T, &start, &chroma_tree_loc, split_tree, true);
  }

  if (!state->cabac.only_count) {
    // Coeffs are not needed anymore.
    free(lcu->coeff);
    lcu->coeff = NULL;
  }

  /*
  bool end_of_slice_segment_flag;
  if (state->encoder_control->cfg.slices & UVG_SLICES_WPP) {
    // Slice segments end after each WPP row.
    end_of_slice_segment_flag = lcu->last_column;
  }
  else if (state->encoder_control->cfg.slices & UVG_SLICES_TILES) {
    // Slices end after each tile.
    end_of_slice_segment_flag = lcu->last_column && lcu->last_row;
  }
  else {
    // Slice ends after the last row of the last tile.
    int last_tile_id = -1 + encoder->cfg.tiles_width_count * encoder->cfg.tiles_height_count;
    bool is_last_tile = state->tile->id == last_tile_id;
    end_of_slice_segment_flag = is_last_tile && lcu->last_column && lcu->last_row;
  }
  //uvg_cabac_encode_bin_trm(&state->cabac, end_of_slice_segment_flag);
  */

  {
    const bool end_of_tile = lcu->last_column && lcu->last_row;
    const bool end_of_wpp_row = encoder->cfg.wpp && lcu->last_column;

    if (end_of_tile || end_of_wpp_row) {
      // end_of_sub_stream_one_bit
      uvg_cabac_encode_bin_trm(&state->cabac, 1);

      // Finish the substream by writing out remaining state.
      uvg_cabac_finish(&state->cabac);

      // Write a rbsp_trailing_bits or a byte_alignment. The first one is used
      // for ending a slice_segment_layer_rbsp and the second one for ending
      // a substream. They are identical and align the byte stream.
      uvg_bitstream_put(state->cabac.stream, 1, 1);
      uvg_bitstream_align_zero(state->cabac.stream);

      uvg_cabac_start(&state->cabac);
    }
  }
  state->cabac.update = 0;


  pthread_mutex_lock(&state->frame->rc_lock);
  const uint32_t bits = (const uint32_t)(uvg_bitstream_tell(&state->stream) - existing_bits);
  state->frame->cur_frame_bits_coded += bits;
  // This variable is used differently by intra and inter frames and shouldn't
  // be touched in intra frames here
  state->frame->remaining_weight -= !state->frame->is_irap ?
    uvg_get_lcu_stats(state, lcu->position.x, lcu->position.y)->original_weight :
    0;
  pthread_mutex_unlock(&state->frame->rc_lock);
  uvg_get_lcu_stats(state, lcu->position.x, lcu->position.y)->bits = bits;

  uint8_t not_skip = false;
  for (int y = 0; y < 64 && !not_skip; y += 8) {
    for (int x = 0; x < 64 && !not_skip; x += 8) {
      not_skip |= !uvg_cu_array_at_const(state->tile->frame->cu_array,
        lcu->position_px.x + x,
        lcu->position_px.y + y)->skipped;
    }
  }
  uvg_get_lcu_stats(state, lcu->position.x, lcu->position.y)->skipped = !not_skip;

  //Wavefronts need the context to be copied to the next row
  if (state->type == ENCODER_STATE_TYPE_WAVEFRONT_ROW && lcu->index == 0) {
    int j;
    //Find next encoder (next row)
    for (j = 0; state->parent->children[j].encoder_control; ++j) {
      if (state->parent->children[j].wfrow->lcu_offset_y == state->wfrow->lcu_offset_y + 1) {
        //And copy context
        uvg_context_copy(&state->parent->children[j], state);
      }
    }
  }
}

static void encoder_state_init_children_after_simulation(encoder_state_t* const state) {
  uvg_bitstream_clear(&state->stream);

  if (state->is_leaf) {
    //Leaf states have cabac and context
    uvg_cabac_start(&state->cabac);
    uvg_init_contexts(state, state->encoder_control->cfg.set_qp_in_cu ? 26 : state->frame->QP, state->frame->slicetype);
  }

  for (int i = 0; state->children[i].encoder_control; ++i) {
    encoder_state_init_children_after_simulation(&state->children[i]);
  }
}

void uvg_alf_enc_process_job(void* opaque) {
  encoder_state_t* const state = (encoder_state_t* const)opaque;
  
  uvg_alf_enc_process(state);

  encoder_state_t* parent = state;
  while (parent->parent) parent = parent->parent;

  // If ALF was used the bitstream coding was simulated in search, reset the cabac/stream
  encoder_state_init_children_after_simulation(parent);
}

static void encoder_state_encode_leaf(encoder_state_t * const state)
{
  const encoder_control_t * const encoder = state->encoder_control;

  assert(state->is_leaf);
  assert(state->lcu_order_count > 0);

  const encoder_control_t *ctrl = state->encoder_control;
  const uvg_config *cfg = &ctrl->cfg;

  // Signaled slice QP may be different to frame QP with set-qp-in-cu enabled.
  state->last_qp = ctrl->cfg.set_qp_in_cu ? 26 : state->frame->QP;

  // Select whether to encode the frame/tile in current thread or to define
  // wavefront jobs for other threads to handle.
  bool wavefront = state->type == ENCODER_STATE_TYPE_WAVEFRONT_ROW;

  // Clear hmvp lut size before each leaf
  if (!wavefront) {
    memset(state->tile->frame->hmvp_size, 0, sizeof(uint8_t) * state->tile->frame->height_in_lcu);
    if(cfg->ibc) memset(state->tile->frame->hmvp_size_ibc, 0, sizeof(uint8_t) * state->tile->frame->height_in_lcu);
  } else {
    state->tile->frame->hmvp_size[state->wfrow->lcu_offset_y] = 0;
    state->tile->frame->hmvp_size_ibc[state->wfrow->lcu_offset_y] = 0;
  }

  bool use_parallel_encoding = (wavefront && state->parent->children[1].encoder_control);
  if (!use_parallel_encoding) {
    
    // Encode every LCU in order and perform SAO reconstruction after every
    // frame is encoded. Deblocking and SAO search is done during LCU encoding.
    for (uint32_t i = 0; i < state->lcu_order_count; ++i) {
      encoder_state_worker_encode_lcu_search(&state->lcu_order[i]);
      // Without alf we can code the bitstream right after each LCU to update cabac contexts
      if (encoder->cfg.alf_type == 0) {
        encoder_state_worker_encode_lcu_bitstream(&state->lcu_order[i]);
      }
    }

    //Encode ALF
    if (encoder->cfg.alf_type) {
      uvg_alf_enc_process(state);
      // If ALF was used the bitstream coding was simulated in search, reset the cabac/stream
      // And write the actual bitstream
      encoder_state_init_children_after_simulation(state);
      for (uint32_t i = 0; i < state->lcu_order_count; ++i) {
        encoder_state_worker_encode_lcu_bitstream(&state->lcu_order[i]);
      }
    }

    
  } else {
    // Add each LCU in the wavefront row as it's own job to the queue.

    // Select which frame dependancies should be set to.
    const encoder_state_t * ref_state = NULL;

    if (state->frame->slicetype == UVG_SLICE_I) {
      // I-frames have no references.
      ref_state = NULL;
    } else if (cfg->gop_lowdelay &&
               cfg->gop_len > 0 &&
               state->previous_encoder_state != state)
    {
      // For LP-gop, depend on the state of the first reference.
      int ref_neg = cfg->gop[state->frame->gop_offset].ref_neg[0];
      if (ref_neg > cfg->owf) {
        // If frame is not within OWF range, it's already done.
        ref_state = NULL;
      } else {
        ref_state = state->previous_encoder_state;
        while (ref_neg > 1) {
          ref_neg -= 1;
          ref_state = ref_state->previous_encoder_state;
        }
      }
    } else {
      // Otherwise, depend on the previous frame.
      ref_state = state->previous_encoder_state;
    }

    for (uint32_t i = 0; i < state->lcu_order_count; ++i) {
      const lcu_order_element_t * const lcu = &state->lcu_order[i];

      uvg_threadqueue_free_job(&state->tile->wf_jobs[lcu->id]);
      uvg_threadqueue_free_job(&state->tile->wf_recon_jobs[lcu->id]);
      state->tile->wf_jobs[lcu->id] = uvg_threadqueue_job_create(encoder_state_worker_encode_lcu_bitstream, (void*)lcu);
      threadqueue_job_t **bitstream_job = &state->tile->wf_jobs[lcu->id];

      // Use a separate job for bitstream writing, first process search and recon
      state->tile->wf_recon_jobs[lcu->id] = uvg_threadqueue_job_create(encoder_state_worker_encode_lcu_search, (void*)lcu);
      threadqueue_job_t **job = &state->tile->wf_recon_jobs[lcu->id];

      // If job object was returned, add dependancies and allow it to run.
      if (job[0]) {
        // Add inter frame dependancies when ecoding more than one frame at
        // once. The added dependancy is for the first LCU of each wavefront
        // row to depend on the reconstruction status of the row below in the
        // previous frame.
        if (ref_state != NULL &&
            state->previous_encoder_state->tqj_recon_done &&
            state->frame->slicetype != UVG_SLICE_I)
        {
          // We need to wait until the CTUs whose pixels we refer to are
          // done before we can start this CTU.
          const lcu_order_element_t *dep_lcu = lcu;
          for (int i = 0; dep_lcu->below && i < ctrl->max_inter_ref_lcu.down; i++) {
            dep_lcu = dep_lcu->below;
          }
          for (int i = 0; dep_lcu->right && i < ctrl->max_inter_ref_lcu.right + 1; i++) {
            dep_lcu = dep_lcu->right;
          }
          uvg_threadqueue_job_dep_add(job[0], ref_state->tile->wf_recon_jobs[dep_lcu->id]);

          //TODO: Preparation for the lock free implementation of the new rc
          if (ref_state->frame->slicetype == UVG_SLICE_I && ref_state->frame->num != 0 && state->encoder_control->cfg.owf > 1 && true) {
            uvg_threadqueue_job_dep_add(job[0], ref_state->previous_encoder_state->tile->wf_recon_jobs[dep_lcu->id]);
          }

          // Very spesific bug that happens when owf length is longer than the
          // gop length. Takes care of that.
          if(!state->encoder_control->cfg.gop_lowdelay &&
             state->encoder_control->cfg.open_gop &&
             state->encoder_control->cfg.gop_len != 0 &&
             state->encoder_control->cfg.owf > state->encoder_control->cfg.gop_len &&
             ref_state->frame->slicetype == UVG_SLICE_I &&
             ref_state->frame->num != 0){

            while (ref_state->frame->poc != state->frame->poc - state->encoder_control->cfg.gop_len){
              ref_state = ref_state->previous_encoder_state;
            }
            uvg_threadqueue_job_dep_add(job[0], ref_state->tile->wf_recon_jobs[dep_lcu->id]);
          }
        }
        
        if (state->encoder_control->cfg.alf_type) {
          encoder_state_t* parent = state;
          while (parent->parent) parent = parent->parent;

          // Add local WPP dependancy to the LCU on the left.
          if (lcu->left) {
            uvg_threadqueue_job_dep_add(job[0], job[-1]);
            uvg_threadqueue_job_dep_add(bitstream_job[0], bitstream_job[-1]);
          }
          // Add local WPP dependancy to the LCU on the top.
          if (lcu->above) {
            uvg_threadqueue_job_dep_add(job[0], job[-state->tile->frame->width_in_lcu]);
            uvg_threadqueue_job_dep_add(bitstream_job[0], bitstream_job[-state->tile->frame->width_in_lcu]);
          }

          uvg_threadqueue_submit(state->encoder_control->threadqueue, job[0]);

          uvg_threadqueue_job_dep_add(state->tile->wf_jobs[lcu->id], parent->tqj_alf_process);
          uvg_threadqueue_job_dep_add(parent->tqj_alf_process, state->tile->wf_recon_jobs[lcu->id]);
        } else {

          // Add local WPP dependancy to the LCU on the left.
          if (lcu->left) {
            uvg_threadqueue_job_dep_add(job[0], bitstream_job[-1]);
          }
          // Add local WPP dependancy to the LCU on the top.
          if (lcu->above) {
            uvg_threadqueue_job_dep_add(job[0], bitstream_job[-state->tile->frame->width_in_lcu]);
          }

          uvg_threadqueue_submit(state->encoder_control->threadqueue, job[0]);

          uvg_threadqueue_job_dep_add(state->tile->wf_jobs[lcu->id], state->tile->wf_recon_jobs[lcu->id]);
#ifdef UVG_DEBUG_PRINT_CABAC
          // Ensures that the ctus are encoded in raster scan order
          if(i >= state->tile->frame->width_in_lcu) {
            uvg_threadqueue_job_dep_add(state->tile->wf_jobs[lcu->id], state->tile->wf_recon_jobs[(lcu->id / state->tile->frame->width_in_lcu - 1) * state->tile->frame->width_in_lcu]);
          }
#endif
        }

        uvg_threadqueue_submit(state->encoder_control->threadqueue, state->tile->wf_jobs[lcu->id]);

        // The wavefront row is done when the last LCU in the row is done.
        if (i + 1 == state->lcu_order_count) {
          assert(!state->tqj_recon_done);
          state->tqj_recon_done =
            uvg_threadqueue_copy_ref(state->tile->wf_jobs[lcu->id]);
        }
      }
    }
  }
}

static void encoder_state_encode(encoder_state_t * const main_state);

static void encoder_state_worker_encode_children(void * opaque)
{
  encoder_state_t *sub_state = opaque;
  encoder_state_encode(sub_state);

  if (sub_state->is_leaf && sub_state->type == ENCODER_STATE_TYPE_WAVEFRONT_ROW) {
    // Set the last wavefront job of this row as the job that completes
    // the bitstream for this wavefront row state.

    int wpp_row = sub_state->wfrow->lcu_offset_y;
    int tile_width = sub_state->tile->frame->width_in_lcu;
    int end_of_row = (wpp_row + 1) * tile_width - 1;
    assert(!sub_state->tqj_bitstream_written);
    if (sub_state->tile->wf_jobs[end_of_row]) {
      sub_state->tqj_bitstream_written =
        uvg_threadqueue_copy_ref(sub_state->tile->wf_jobs[end_of_row]);
    }
  }
}

static int encoder_state_tree_is_a_chain(const encoder_state_t * const state) {
  if (!state->children[0].encoder_control) return 1;
  if (state->children[1].encoder_control) return 0;
  return encoder_state_tree_is_a_chain(&state->children[0]);
}

static void encoder_state_encode(encoder_state_t * const main_state) {
  //If we have children, encode at child level
  if (main_state->children[0].encoder_control) {
    //If we have only one child, than it cannot be the last split in tree
    int node_is_the_last_split_in_tree = (main_state->children[1].encoder_control != 0);

    for (int i = 0; main_state->children[i].encoder_control; ++i) {
      encoder_state_t *sub_state = &(main_state->children[i]);

      if (sub_state->tile != main_state->tile) {
        const int offset_x = sub_state->tile->offset_x;
        const int offset_y = sub_state->tile->offset_y;
        const int width = MIN(sub_state->tile->frame->width_in_lcu * LCU_WIDTH, main_state->tile->frame->width - offset_x);
        const int height = MIN(sub_state->tile->frame->height_in_lcu * LCU_WIDTH, main_state->tile->frame->height - offset_y);

        sub_state->tile->frame->lmcs_aps = main_state->tile->frame->lmcs_aps;
        sub_state->tile->frame->lmcs_avg_processed = main_state->tile->frame->lmcs_avg_processed;
        sub_state->tile->frame->lmcs_avg = main_state->tile->frame->lmcs_avg;

        if (sub_state->encoder_control->cfg.alf_type) {
          main_state->slice->alf = sub_state->slice->alf = main_state->parent->slice->alf;
          sub_state->tile->frame->alf_param_set_map = main_state->tile->frame->alf_param_set_map;
          sub_state->tile->frame->alf_info = main_state->tile->frame->alf_info;
        }
        uvg_image_free(sub_state->tile->frame->source);
        sub_state->tile->frame->source = NULL;

        uvg_image_free(sub_state->tile->frame->rec);
        sub_state->tile->frame->rec = NULL;

        uvg_cu_array_free(&sub_state->tile->frame->cu_array);
        if(sub_state->tile->frame->chroma_cu_array) {
          uvg_cu_array_free(&sub_state->tile->frame->chroma_cu_array);
        }

        sub_state->tile->frame->source = uvg_image_make_subimage(
            main_state->tile->frame->source,
            offset_x,
            offset_y,
            width,
            height
        );
        sub_state->tile->frame->rec = uvg_image_make_subimage(
            main_state->tile->frame->rec,
            offset_x,
            offset_y,
            width,
            height
        );
        

        if (sub_state->encoder_control->cfg.lmcs_enable) {
          uvg_image_free(sub_state->tile->frame->source_lmcs);
          sub_state->tile->frame->source_lmcs = NULL;

          uvg_image_free(sub_state->tile->frame->rec_lmcs);
          sub_state->tile->frame->rec_lmcs = NULL;

          sub_state->tile->frame->source_lmcs = uvg_image_make_subimage(
            main_state->tile->frame->source_lmcs,
            offset_x,
            offset_y,
            width,
            height
          );
          sub_state->tile->frame->rec_lmcs = uvg_image_make_subimage(
            main_state->tile->frame->rec_lmcs,
            offset_x,
            offset_y,
            width,
            height
          );

          sub_state->tile->frame->source_lmcs_mapped = true;
        } else {
          sub_state->tile->frame->source_lmcs = sub_state->tile->frame->source;
          sub_state->tile->frame->rec_lmcs = sub_state->tile->frame->rec;
        }

        sub_state->tile->frame->cu_array = uvg_cu_subarray(
            main_state->tile->frame->cu_array,
            offset_x,
            offset_y,
            sub_state->tile->frame->width_in_lcu * LCU_WIDTH,
            sub_state->tile->frame->height_in_lcu * LCU_WIDTH
        );
        if(main_state->encoder_control->cfg.dual_tree && main_state->frame->is_irap){
          sub_state->tile->frame->chroma_cu_array = uvg_cu_subarray(
              main_state->tile->frame->chroma_cu_array,
              offset_x,
              offset_y,
              sub_state->tile->frame->width_in_lcu * LCU_WIDTH,
              sub_state->tile->frame->height_in_lcu * LCU_WIDTH
          );
        }
      }

      //To be the last split, we require that every child is a chain
      node_is_the_last_split_in_tree =
        node_is_the_last_split_in_tree &&
        encoder_state_tree_is_a_chain(&main_state->children[i]);
    }
    //If it's the latest split point
    if (node_is_the_last_split_in_tree) {
      for (int i = 0; main_state->children[i].encoder_control; ++i) {
        //If we don't have wavefronts, parallelize encoding of children.
        if (main_state->children[i].type != ENCODER_STATE_TYPE_WAVEFRONT_ROW) {
          uvg_threadqueue_free_job(&main_state->children[i].tqj_recon_done);
          main_state->children[i].tqj_recon_done =
            uvg_threadqueue_job_create(encoder_state_worker_encode_children, &main_state->children[i]);
          if (main_state->children[i].previous_encoder_state != &main_state->children[i] &&
              main_state->children[i].previous_encoder_state->tqj_recon_done &&
              !main_state->children[i].frame->is_irap)
          {
#if 0
            // Disabled due to non-determinism.
            if (main_state->encoder_control->cfg->mv_constraint == UVG_MV_CONSTRAIN_FRAME_AND_TILE_MARGIN)
            {
              // When MV's don't cross tile boundaries, add dependancy only to the same tile.
              uvg_threadqueue_job_dep_add(main_state->children[i].tqj_recon_done, main_state->children[i].previous_encoder_state->tqj_recon_done);
            } else 
#endif      
            {
              // Add dependancy to each child in the previous frame.
              for (int child_id = 0; main_state->children[child_id].encoder_control; ++child_id) {
                uvg_threadqueue_job_dep_add(main_state->children[i].tqj_recon_done, main_state->children[child_id].previous_encoder_state->tqj_recon_done);
              }
            }
          }
          uvg_threadqueue_submit(main_state->encoder_control->threadqueue, main_state->children[i].tqj_recon_done);
        } else {
          //Wavefront rows have parallelism at LCU level, so we should not launch multiple threads here!
          //FIXME: add an assert: we can only have wavefront children
          encoder_state_worker_encode_children(&(main_state->children[i]));
        }
      }
    } else {
      for (int i = 0; main_state->children[i].encoder_control; ++i) {
        encoder_state_worker_encode_children(&(main_state->children[i]));
      }
    }
  } else {
    switch (main_state->type) {
      case ENCODER_STATE_TYPE_TILE:
      case ENCODER_STATE_TYPE_SLICE:
      case ENCODER_STATE_TYPE_WAVEFRONT_ROW:
        encoder_state_encode_leaf(main_state);
        break;
      default:
        fprintf(stderr, "Unsupported leaf type %c!\n", main_state->type);
        assert(0);
    }
  }
}


static void encoder_ref_insertion_sort(const encoder_state_t *const state,
                                       uint8_t reflist[16],
                                       uint8_t length,
                                       bool reverse)
{

  for (uint8_t i = 1; i < length; ++i) {
    const uint8_t cur_idx = reflist[i];
    const int32_t cur_poc = state->frame->ref->pocs[cur_idx];
    int8_t j = i;
    while ((j > 0 && !reverse && cur_poc > state->frame->ref->pocs[reflist[j - 1]]) ||
           (j > 0 &&  reverse && cur_poc < state->frame->ref->pocs[reflist[j - 1]]))
    {
      reflist[j] = reflist[j - 1];
      --j;
    }
    reflist[j] = cur_idx;
  }
}

/**
 * \brief Generate reference picture lists.
 *
 * \param state             main encoder state
 */
void uvg_encoder_create_ref_lists(const encoder_state_t *const state)
{
  const uvg_config *cfg = &state->encoder_control->cfg;

  FILL_ARRAY(state->frame->ref_LX_size, 0, 2);

  int num_negative = 0;
  int num_positive = 0;

  // Add positive references to L1 list
  for (uint32_t i = 0; i < state->frame->ref->used_size; i++) {
    if (state->frame->ref->pocs[i] > state->frame->poc) {
      state->frame->ref_LX[1][state->frame->ref_LX_size[1]] = i;
      state->frame->ref_LX_size[1] += 1;
      num_positive++;
    }
  }

  // Add negative references to L1 list when bipred is enabled and GOP is
  // either disabled or does not use picture reordering.
  bool l1_negative_refs =
    (cfg->bipred && (cfg->gop_len == 0 || cfg->gop_lowdelay));

  // Add negative references to L0 and L1 lists.
  for (uint32_t i = 0; i < state->frame->ref->used_size; i++) {
    if (state->frame->ref->pocs[i] < state->frame->poc) {
      state->frame->ref_LX[0][state->frame->ref_LX_size[0]] = i;
      state->frame->ref_LX_size[0] += 1;
      if (l1_negative_refs) {
        state->frame->ref_LX[1][state->frame->ref_LX_size[1]] = i;
        state->frame->ref_LX_size[1] += 1;
      }
      num_negative++;
    }
  }

  // Fill the rest with -1.
  for (int i = state->frame->ref_LX_size[0]; i < 16; i++) {
    state->frame->ref_LX[0][i] = 0xff;
  }
  for (int i = state->frame->ref_LX_size[1]; i < 16; i++) {
    state->frame->ref_LX[1][i] = 0xff;
  }

  // Sort reference lists.
  encoder_ref_insertion_sort(state, state->frame->ref_LX[0], num_negative, false);
  encoder_ref_insertion_sort(state, state->frame->ref_LX[1], num_positive, true);
  if (l1_negative_refs) {
    encoder_ref_insertion_sort(state, state->frame->ref_LX[1] + num_positive, num_negative, false);
  }
}

/**
 * \brief Remove any references that should no longer be used.
 */
static void encoder_state_remove_refs(encoder_state_t *state) {
  const encoder_control_t * const encoder = state->encoder_control;

  int neg_refs = encoder->cfg.gop[state->frame->gop_offset].ref_neg_count;
  int pos_refs = encoder->cfg.gop[state->frame->gop_offset].ref_pos_count;

  unsigned target_ref_num;
  if (encoder->cfg.gop_len) {
    target_ref_num = neg_refs + pos_refs;
  } else {
    target_ref_num = encoder->cfg.ref_frames;
  }

  if (state->frame->pictype == UVG_NAL_IDR_W_RADL ||
      state->frame->pictype == UVG_NAL_IDR_N_LP)
  {
    target_ref_num = 0;
  }

  if (encoder->cfg.gop_len && target_ref_num > 0) {
    // With GOP in use, go through all the existing reference pictures and
    // remove any picture that is not referenced by the current picture.

    for (int ref = state->frame->ref->used_size - 1; ref >= 0; --ref) {
      bool is_referenced = false;

      int ref_poc = state->frame->ref->pocs[ref];

      for (int i = 0; i < neg_refs; i++) {
        int ref_relative_poc = -encoder->cfg.gop[state->frame->gop_offset].ref_neg[i];
        if (ref_poc == state->frame->poc + ref_relative_poc) {
          is_referenced = true;
          break;
        }
      }

      for (int i = 0; i < pos_refs; i++) {
        int ref_relative_poc = encoder->cfg.gop[state->frame->gop_offset].ref_pos[i];
        if (ref_poc == state->frame->poc + ref_relative_poc) {
          is_referenced = true;
          break;
        }
      }

      if (ref_poc < state->frame->irap_poc &&
          state->frame->irap_poc < state->frame->poc)
      {
        // Trailing frames cannot refer to leading frames.
        is_referenced = false;
      }

      if (encoder->cfg.intra_period > 0 &&
          ref_poc < state->frame->irap_poc - encoder->cfg.intra_period)
      {
        // No frame can refer past the two preceding IRAP frames.
        is_referenced = false;
      }

      if (!is_referenced) {
        // This reference is not referred to by this frame, it must be removed.
        uvg_image_list_rem(state->frame->ref, ref);
      }
    }
  } else {
    // Without GOP, remove the oldest picture.
    while (state->frame->ref->used_size > target_ref_num) {
      int8_t oldest_ref = state->frame->ref->used_size - 1;
      uvg_image_list_rem(state->frame->ref, oldest_ref);
    }
  }

  assert(state->frame->ref->used_size <= target_ref_num);
}

static void encoder_set_source_picture(encoder_state_t * const state, uvg_picture* frame)
{
  assert(!state->tile->frame->source);
  assert(!state->tile->frame->rec);

  state->tile->frame->source_lmcs_mapped = false;
  state->tile->frame->rec_lmcs_mapped = false;
  state->tile->frame->lmcs_top_level = false;

  state->tile->frame->source = frame;
  state->tile->frame->source_lmcs = state->tile->frame->source;

  if (state->encoder_control->cfg.lossless) {
    // In lossless mode, the reconstruction is equal to the source frame.
    state->tile->frame->rec = uvg_image_copy_ref(frame);
  } else {
    state->tile->frame->rec = uvg_image_alloc(state->encoder_control->chroma_format, frame->width_luma, frame->height_luma);
    state->tile->frame->rec->dts = frame->dts;
    state->tile->frame->rec->pts = frame->pts;
  }
  state->tile->frame->rec_lmcs = state->tile->frame->rec;

  if (state->encoder_control->cfg.lmcs_enable) {
    state->tile->frame->rec_lmcs = uvg_image_alloc(state->encoder_control->chroma_format, frame->width_luma, frame->height_luma);
    state->tile->frame->source_lmcs = uvg_image_alloc(state->encoder_control->chroma_format, frame->width_luma, frame->height_luma);
  }
  uvg_videoframe_set_poc(state->tile->frame, state->frame->poc);
}

static void encoder_state_init_children(encoder_state_t * const state) {
  uvg_bitstream_clear(&state->stream);

  if (state->is_leaf) {
    //Leaf states have cabac and context
    uvg_cabac_start(&state->cabac);
    uvg_init_contexts(state, state->encoder_control->cfg.set_qp_in_cu ? 26 : state->frame->QP, state->frame->slicetype);
  }

  //Clear the jobs
  uvg_threadqueue_free_job(&state->tqj_bitstream_written);
  uvg_threadqueue_free_job(&state->tqj_recon_done);

  //Copy the constraint pointer
  // TODO: Try to do it in the if (state->is_leaf)
  //if (state->parent != NULL) {
    // state->constraint = state->parent->constraint;
  //}

  for (int i = 0; state->children[i].encoder_control; ++i) {
    encoder_state_init_children(&state->children[i]);
  }
}

static void normalize_lcu_weights(encoder_state_t * const state)
{
  if (state->frame->num == 0) return;

  const uint32_t num_lcus = state->encoder_control->in.width_in_lcu *
                            state->encoder_control->in.height_in_lcu;
  double sum = 0.0;
  for (uint32_t i = 0; i < num_lcus; i++) {
    sum += state->frame->lcu_stats[i].weight;
  }

  for (uint32_t i = 0; i < num_lcus; i++) {
    state->frame->lcu_stats[i].weight /= sum;
  }
}

// Check if lcu is edge lcu. Return false if frame dimensions are 64 divisible
static bool edge_lcu(int id, int lcus_x, int lcus_y, bool xdiv64, bool ydiv64)
{
  if (xdiv64 && ydiv64) {
    return false;
  }
  int last_row_first_id = (lcus_y - 1) * lcus_x;
  if ((id % lcus_x == lcus_x - 1 && !xdiv64) || (id >= last_row_first_id && !ydiv64)) {
    return true;
  }
  else {
    return false;
  }
}


/**
 * \brief Return weight for 360 degree ERP video
 *
 * Returns the scaling factor of area from equirectangular projection to
 * spherical surface.
 *
 * \param y   y-coordinate of the pixel
 * \param h   height of the picture
 */
static double ws_weight(int y, int h)
{
  return cos((y - 0.5 * h + 0.5) * (M_PI / h));
}


/**
 * \brief Update ROI QPs for 360 video with equirectangular projection.
 *
 * Updates the ROI parameters in frame->roi.
 *
 * \param encoder       encoder control
 * \param frame         frame that will have the ROI map
 */
static void init_erp_aqp_roi(const encoder_control_t *encoder, uvg_picture *frame)
{
  int8_t *orig_roi    = frame->roi.roi_array;
  int32_t orig_width  = frame->roi.width;
  int32_t orig_height = frame->roi.height;

  // Update ROI with WS-PSNR delta QPs.
  int new_height = encoder->in.height_in_lcu;
  int new_width = orig_roi ? orig_width : 1;
  int8_t *new_array = calloc(new_width * new_height, sizeof(orig_roi[0]));

  int frame_height = encoder->in.real_height;

  double total_weight = 0.0;
  for (int y = 0; y < frame_height; y++) {
    total_weight += ws_weight(y, frame_height);
  }

  for (int y_lcu = 0; y_lcu < new_height; y_lcu++) {
    int y_orig = LCU_WIDTH * y_lcu;
    int lcu_height = MIN(LCU_WIDTH, frame_height - y_orig);

    double lcu_weight = 0.0;
    for (int y = y_orig; y < y_orig + lcu_height; y++) {
      lcu_weight += ws_weight(y, frame_height);
    }
    // Normalize.
    lcu_weight = (lcu_weight * frame_height) / (total_weight * lcu_height);

    int8_t qp_delta = (int8_t)(round(-ERP_AQP_STRENGTH * log2(lcu_weight)));

    if (orig_roi) {
      // If a ROI array already exists, we copy the existing values to the
      // new array while adding qp_delta to each.
      int y_roi = y_lcu * orig_height / new_height;
      for (int x = 0; x < new_width; x++) {
        new_array[x + y_lcu * new_width] =
          CLIP(-51, 51, orig_roi[x + y_roi * new_width] + qp_delta);
      }

    } else {
      // Otherwise, simply write qp_delta to the ROI array.
      new_array[y_lcu] = qp_delta;
    }
  }

  // Update new values
  frame->roi.width = new_width;
  frame->roi.height = new_height;
  frame->roi.roi_array = new_array;
  FREE_POINTER(orig_roi);
}


static void next_roi_frame_from_file(uvg_picture *frame, FILE *file, enum uvg_roi_format format) {
  // The ROI description is as follows:
  // First number is width, second number is height,
  // then follows width * height number of dqp values.

  // Rewind the (seekable) ROI file when end of file is reached.
  // Allows a single ROI frame to be used for a whole sequence
  // and looping with --loop-input. Skips possible whitespace.
  if (ftell(file) != -1L) {
    int c = fgetc(file);
    while (format == UVG_ROI_TXT && isspace(c)) c = fgetc(file);
    ungetc(c, file);
    if (c == EOF) rewind(file);
  }

  int *width  = &frame->roi.width;
  int *height = &frame->roi.height;

  bool failed = false;

  if (format == UVG_ROI_TXT) failed = !fscanf(file, "%d", width) || !fscanf(file, "%d", height);
  if (format == UVG_ROI_BIN) failed = fread(&frame->roi, 4, 2, file) != 2;
  
  if (failed) {
    fprintf(stderr, "Failed to read ROI size.\n");
    fclose(file);
    assert(0);
  }

  if (*width <= 0 || *height <= 0) {
    fprintf(stderr, "Invalid ROI size: %dx%d.\n", *width, *height);
    fclose(file);
    assert(0);
  }

  if (*width > 10000 || *height > 10000) {
    fprintf(stderr, "ROI dimensions exceed arbitrary value of 10000.\n");
    fclose(file);
    assert(0);
  }

  const unsigned size = (*width) * (*height);
  int8_t *dqp_array = calloc((size_t)size, sizeof(frame->roi.roi_array[0]));
  if (!dqp_array) {
    fprintf(stderr, "Failed to allocate memory for ROI table.\n");
    fclose(file);
    assert(0);
  }

  FREE_POINTER(frame->roi.roi_array);
  frame->roi.roi_array = dqp_array;

  if (format == UVG_ROI_TXT) {
    for (uint32_t i = 0; i < size; ++i) {
      int number; // Need a pointer to int for fscanf
      if (fscanf(file, "%d", &number) != 1) {
        fprintf(stderr, "Reading ROI file failed.\n");
        fclose(file);
        assert(0);
      }
      dqp_array[i] = CLIP(-51, 51, number);
    }
  } else if (format == UVG_ROI_BIN) {
    if (fread(dqp_array, 1, size, file) != size) {
      fprintf(stderr, "Reading ROI file failed.\n");
      assert(0);
    }
  }
}

static void encoder_state_init_new_frame(encoder_state_t * const state, uvg_picture* frame) {
  assert(state->type == ENCODER_STATE_TYPE_MAIN);

  const uvg_config * const cfg = &state->encoder_control->cfg;

  encoder_set_source_picture(state, frame);

  assert(!state->tile->frame->cu_array);
  state->tile->frame->cu_array = uvg_cu_array_alloc(
      state->tile->frame->width,
      state->tile->frame->height
  );

  if (!state->encoder_control->tiles_enable) {
    memset(state->tile->frame->hmvp_size, 0, sizeof(uint8_t) * state->tile->frame->height_in_lcu);
    memset(state->tile->frame->hmvp_size_ibc, 0, sizeof(uint8_t) * state->tile->frame->height_in_lcu);
  }

  // ROI / delta QP maps
  if (frame->roi.roi_array && cfg->roi.file_path) {
    assert(0 && "Conflict: Other ROI data was supplied when a ROI file was specified.");
  }

  // Read frame from the file. If no file is specified,
  // ROI data should be already set by the application.
  if (cfg->roi.file_path) {
    next_roi_frame_from_file(frame, state->encoder_control->roi_file, cfg->roi.format);
  }
  
  if (cfg->erp_aqp) {
    init_erp_aqp_roi(state->encoder_control, state->tile->frame->source);
  }

  // Variance adaptive quantization
  if (cfg->vaq) {
    const bool has_chroma = state->encoder_control->chroma_format != UVG_CSP_400;
    double d = cfg->vaq * 0.1; // Empirically decided constant. Affects delta-QP strength
    
    // Calculate frame pixel variance
    uint32_t len = state->tile->frame->width * state->tile->frame->height;
    uint32_t c_len = len / 4;
    double frame_var = uvg_pixel_var(state->tile->frame->source->y, len);
    if (has_chroma) {
      frame_var += uvg_pixel_var(state->tile->frame->source->u, c_len);
      frame_var += uvg_pixel_var(state->tile->frame->source->v, c_len);
    }

    // Loop through LCUs
    // For each LCU calculate: D * (log(LCU pixel variance) - log(frame pixel variance))
    unsigned x_lim = state->tile->frame->width_in_lcu;
    unsigned y_lim = state->tile->frame->height_in_lcu;
    
    unsigned id = 0;
    for (uint32_t y = 0; y < y_lim; ++y) {
      for (uint32_t x = 0; x < x_lim; ++x) {
        uvg_pixel tmp[LCU_LUMA_SIZE];
        int pxl_x = x * LCU_WIDTH;
        int pxl_y = y * LCU_WIDTH;
        int x_max = MIN(pxl_x + LCU_WIDTH, frame->width_luma) - pxl_x;
        int y_max = MIN(pxl_y + LCU_WIDTH, frame->height_luma) - pxl_y;
        
        bool xdiv64 = false;
        bool ydiv64 = false;
        if (frame->width_luma % 64 == 0) xdiv64 = true;
        if (frame->height_luma % 64 == 0) ydiv64 = true;

        // Luma variance
        if (!edge_lcu(id, x_lim, y_lim, xdiv64, ydiv64)) {
          uvg_pixels_blit(&state->tile->frame->source->y[pxl_x + pxl_y * state->tile->frame->source->stride_luma], tmp,
            x_max, y_max, state->tile->frame->source->stride_luma, LCU_WIDTH);
        } else {
          // Extend edge pixels for edge lcus
          for (int y = 0; y < LCU_WIDTH; y++) {
            for (int x = 0; x < LCU_WIDTH; x++) {
              int src_y = CLIP(0, frame->height_luma - 1, pxl_y + y);
              int src_x = CLIP(0, frame->width_luma - 1, pxl_x + x);
              tmp[y * LCU_WIDTH + x] = state->tile->frame->source->y[src_y * state->tile->frame->source->stride_luma + src_x];
            }
          }
        }
        
        double lcu_var = uvg_pixel_var(tmp, LCU_LUMA_SIZE);

        if (has_chroma) {
          // Add chroma variance if not monochrome
          int32_t c_stride = state->tile->frame->source->stride_chroma;
          uvg_pixel chromau_tmp[LCU_CHROMA_SIZE];
          uvg_pixel chromav_tmp[LCU_CHROMA_SIZE];
          int lcu_chroma_width = LCU_WIDTH >> state->tile->frame->source->chroma_scale_x;
          int c_pxl_x = x * lcu_chroma_width;
          int c_pxl_y = y * lcu_chroma_width;
          int c_x_max = MIN(c_pxl_x + lcu_chroma_width, frame->width_chroma) - c_pxl_x;
          int c_y_max = MIN(c_pxl_y + lcu_chroma_width, frame->height_chroma) - c_pxl_y;

          if (!edge_lcu(id, x_lim, y_lim, xdiv64, ydiv64)) {
            uvg_pixels_blit(&state->tile->frame->source->u[c_pxl_x + c_pxl_y * c_stride], chromau_tmp, c_x_max, c_y_max, c_stride, lcu_chroma_width);
            uvg_pixels_blit(&state->tile->frame->source->v[c_pxl_x + c_pxl_y * c_stride], chromav_tmp, c_x_max, c_y_max, c_stride, lcu_chroma_width);
          }
          else {
            for (int y = 0; y < lcu_chroma_width; y++) {
              for (int x = 0; x < lcu_chroma_width; x++) {
                int src_y = CLIP(0, (frame->height_chroma) - 1, c_pxl_y + y);
                int src_x = CLIP(0, (frame->width_chroma) - 1, c_pxl_x + x);
                chromau_tmp[y * lcu_chroma_width + x] = state->tile->frame->source->u[src_y * c_stride + src_x];
                chromav_tmp[y * lcu_chroma_width + x] = state->tile->frame->source->v[src_y * c_stride + src_x];
              }
            }
          }
          lcu_var += uvg_pixel_var(chromau_tmp, LCU_CHROMA_SIZE);
          lcu_var += uvg_pixel_var(chromav_tmp, LCU_CHROMA_SIZE);
        }
                
        state->frame->aq_offsets[id] = d * (log(lcu_var) - log(frame_var));
        id++; 
      }
    }
  }
  // Variance adaptive quantization - END

  if (cfg->target_bitrate > 0 || frame->roi.roi_array || cfg->set_qp_in_cu || cfg->vaq) {
    state->frame->max_qp_delta_depth = 0;
  } else {
    state->frame->max_qp_delta_depth = -1;
  }

  // Use this flag to handle closed gop irap picture selection.
  // If set to true, irap is already set and we avoid
  // setting it based on the intra period
  bool is_closed_normal_gop = false;

  encoder_state_t *previous = state->previous_encoder_state;
  int owf = MIN(state->encoder_control->cfg.owf, state->frame->num);

  const int layer = state->encoder_control->cfg.gop[state->frame->gop_offset].layer;

  while (--owf > 0 && layer != state->encoder_control->cfg.gop[previous->frame->gop_offset].layer) {
    previous = previous->previous_encoder_state;
  }

  if (owf == 0) previous = state;
  state->frame->previous_layer_state = previous;
  // Set POC.
  if (state->frame->num == 0) {
    state->frame->poc = 0;
  } else if (cfg->gop_len && !cfg->gop_lowdelay) {

    int32_t framenum = state->frame->num - 1;
    // Handle closed GOP
    // Closed GOP structure has an extra IDR between the GOPs
    if (cfg->intra_period > 0 && !cfg->open_gop) {
      is_closed_normal_gop = true;
      if (framenum % (cfg->intra_period + 1) == cfg->intra_period) {
        // Insert IDR before each new GOP after intra period in closed GOP configuration
        state->frame->poc = 0;
      } else {
        // Calculate frame number again and use that for the POC
        framenum = framenum % (cfg->intra_period + 1);
        int32_t poc_offset = cfg->gop[state->frame->gop_offset].poc_offset;
        state->frame->poc = framenum - framenum % cfg->gop_len + poc_offset;
        // This should not be an irap picture in closed GOP
        state->frame->is_irap = false;
      }
    } else { // Open GOP
      // Calculate POC according to the global frame counter and GOP structure
      int32_t poc_offset = cfg->gop[state->frame->gop_offset].poc_offset;
      state->frame->poc = framenum - framenum % cfg->gop_len + poc_offset;
    }
    
    uvg_videoframe_set_poc(state->tile->frame, state->frame->poc);
  } else if (cfg->intra_period > 1) {
    state->frame->poc = state->frame->num % cfg->intra_period;
  } else {
    state->frame->poc = state->frame->num;
  }

  // Check whether the frame is a keyframe or not.
  if (state->frame->num == 0 || state->frame->poc == 0) {
    state->frame->is_irap = true;
  } else if(!is_closed_normal_gop) { // In closed-GOP IDR frames are poc==0 so skip this check
    state->frame->is_irap =
      cfg->intra_period > 0 &&
      (state->frame->poc % cfg->intra_period) == 0;
  }
  if (state->frame->is_irap) {
    state->frame->irap_poc = state->frame->poc;
  }

  if (cfg->dual_tree && state->encoder_control->chroma_format != UVG_CSP_400 && state->frame->is_irap) {
    assert(state->tile->frame->chroma_cu_array == NULL);
    state->tile->frame->chroma_cu_array = uvg_cu_array_alloc(
      state->tile->frame->width,
      state->tile->frame->height
    );
  }
  // Set pictype.
  if (state->frame->is_irap) {
    if (state->frame->num == 0 ||
        cfg->intra_period == 1 ||
        cfg->gop_len == 0 ||
        cfg->gop_lowdelay ||
        !cfg->open_gop) // Closed GOP uses IDR pictures
    {
      state->frame->pictype = UVG_NAL_IDR_N_LP;
      if (cfg->intra_period == 1 && state->frame->num > 0) state->frame->pictype = UVG_NAL_IDR_W_RADL;
    } else {
      state->frame->pictype = UVG_NAL_CRA_NUT;
    }
  } else if (state->frame->poc < state->frame->irap_poc) {
    state->frame->pictype = UVG_NAL_RASL;
  } else {
    state->frame->pictype = UVG_NAL_TRAIL;
  }

  encoder_state_remove_refs(state);
  uvg_encoder_create_ref_lists(state);

  // Set slicetype.
  if (state->frame->is_irap) {
    state->frame->slicetype = UVG_SLICE_I;
  } else if (state->frame->ref_LX_size[1] > 0) {
    state->frame->slicetype = UVG_SLICE_B;
  } else {
    state->frame->slicetype = UVG_SLICE_P;
  }

  if (cfg->target_bitrate > 0 && state->frame->num > cfg->owf) {
    normalize_lcu_weights(state);
  }
  state->frame->cur_frame_bits_coded = 0;

  switch (state->encoder_control->cfg.rc_algorithm) {
    case UVG_NO_RC:
    case UVG_LAMBDA:
      uvg_set_picture_lambda_and_qp(state);
      break;
    case UVG_OBA:
      uvg_estimate_pic_lambda(state);
      break;
    default:
      assert(0);
  }

  if (state->encoder_control->cfg.lmcs_enable) {
    uvg_init_lmcs_aps(state->tile->frame->lmcs_aps, state->encoder_control->cfg.width, state->encoder_control->cfg.height, LCU_CU_WIDTH, LCU_CU_WIDTH, state->encoder_control->bitdepth);

    state->tile->frame->lmcs_aps->m_reshapeCW.rspPicSize = state->tile->frame->width * state->tile->frame->height;
    state->tile->frame->lmcs_aps->m_reshapeCW.rspBaseQP = state->encoder_control->cfg.qp;
    state->tile->frame->lmcs_aps->m_reshapeCW.rspFpsToIp = 16;
    state->tile->frame->lmcs_aps->m_reshapeCW.updateCtrl = 1; //ToDo: change "LMCS model update control: 0:RA, 1:AI, 2:LDB/LDP"
    
    // ToDo: support other signal types in LMCS
    uvg_lmcs_preanalyzer(state, state->tile->frame, state->tile->frame->lmcs_aps, RESHAPE_SIGNAL_SDR);
    if (state->tile->frame->lmcs_aps->m_sliceReshapeInfo.sliceReshaperEnableFlag) {
      uvg_construct_reshaper_lmcs(state->tile->frame->lmcs_aps);

      uvg_pixel* luma = state->tile->frame->source->y;
      uvg_pixel* luma_lmcs = state->tile->frame->source_lmcs->y;
      for (int y = 0; y < state->tile->frame->source->height_luma; y++) {
        for (int x = 0; x < state->tile->frame->source->width_luma; x++) {
          luma_lmcs[x] = state->tile->frame->lmcs_aps->m_fwdLUT[luma[x]];
        }
        luma += state->tile->frame->source->stride_luma;
        luma_lmcs += state->tile->frame->source->stride_luma;
      }
      state->tile->frame->source_lmcs_mapped = true;
      state->tile->frame->lmcs_top_level = true;
    }

    memset(state->tile->frame->lmcs_avg_processed, 0, state->tile->frame->width_in_lcu * state->tile->frame->height_in_lcu);
  }
 
  encoder_state_init_children(state);
}

static void _encode_one_frame_add_bitstream_deps(const encoder_state_t * const state, threadqueue_job_t * const job) {
  int i;
  for (i = 0; state->children[i].encoder_control; ++i) {
    _encode_one_frame_add_bitstream_deps(&state->children[i], job);
  }
  if (state->tqj_bitstream_written) {
    uvg_threadqueue_job_dep_add(job, state->tqj_bitstream_written);
  }
  if (state->tqj_recon_done) {
    uvg_threadqueue_job_dep_add(job, state->tqj_recon_done);
  }
}


void uvg_encode_one_frame(encoder_state_t * const state, uvg_picture* frame)
{
#if UVG_DEBUG_PRINT_CABAC == 1
  // uvg_cabac_bins_count = 0;
  if (state->frame->num == 1) uvg_cabac_bins_verbose = true;
  // else uvg_cabac_bins_verbose = false;
#endif


  encoder_state_init_new_frame(state, frame);
  if(state->encoder_control->cfg.jccr) set_joint_cb_cr_modes(state, frame);
  
  // Create a separate job for ALF done after everything else, and only then do final bitstream writing (for ALF parameters)
  if (state->encoder_control->cfg.alf_type && state->encoder_control->cfg.wpp) {
    uvg_threadqueue_free_job(&state->tqj_alf_process);
    encoder_state_t* child_state = state;
    while (child_state->lcu_order == NULL) child_state = &child_state->children[0];
    state->tqj_alf_process = uvg_threadqueue_job_create(uvg_alf_enc_process_job, child_state);
  }

  encoder_state_encode(state);

  threadqueue_job_t *job =
    uvg_threadqueue_job_create(uvg_encoder_state_worker_write_bitstream, state);


  if (state->encoder_control->cfg.alf_type && state->encoder_control->cfg.wpp) {
    uvg_threadqueue_submit(state->encoder_control->threadqueue, state->tqj_alf_process);    
  }

  _encode_one_frame_add_bitstream_deps(state, job);
  if (state->previous_encoder_state != state && state->previous_encoder_state->tqj_bitstream_written) {
    //We need to depend on previous bitstream generation
    uvg_threadqueue_job_dep_add(job, state->previous_encoder_state->tqj_bitstream_written);
  }  
  assert(!state->tqj_bitstream_written);
  state->tqj_bitstream_written = job;  
  state->frame->done = 0;
  uvg_threadqueue_submit(state->encoder_control->threadqueue, job);
}


/**
 * Prepare the encoder state for encoding the next frame.
 *
 * - Add the previous reconstructed picture as a reference, if needed.
 * - Free the previous reconstructed and source pictures.
 * - Create a new cu array, if needed.
 * - Update frame count and POC.
 */
void uvg_encoder_prepare(encoder_state_t *state)
{
  const encoder_control_t * const encoder = state->encoder_control;

  // The previous frame must be done before the next one is started.
  assert(state->frame->done);

  if (state->frame->num == -1) {
    // We're at the first frame, so don't care about all this stuff.
    state->frame->num = 0;
    state->frame->poc = 0;
    state->frame->irap_poc = 0;
    assert(!state->tile->frame->source);
    assert(!state->tile->frame->rec);
    assert(!state->tile->frame->cu_array);
    state->frame->prepared = 1;

    return;
  }

  // NOTE: prev_state is equal to state when OWF is zero
  encoder_state_t *prev_state = state->previous_encoder_state;

  if (state->previous_encoder_state != state) {
    uvg_cu_array_free(&state->tile->frame->cu_array);
    if (state->tile->frame->chroma_cu_array) {
      uvg_cu_array_free(&state->tile->frame->chroma_cu_array);
    }
    unsigned width  = state->tile->frame->width_in_lcu  * LCU_WIDTH;
    unsigned height = state->tile->frame->height_in_lcu * LCU_WIDTH;
    state->tile->frame->cu_array = uvg_cu_array_alloc(width, height);

    uvg_image_list_copy_contents(state->frame->ref, prev_state->frame->ref);
    uvg_encoder_create_ref_lists(state);
  }

  if (!encoder->cfg.gop_len ||
      !prev_state->frame->poc ||
      encoder->cfg.gop[prev_state->frame->gop_offset].is_ref) {

    // Store current list of POCs for use in TMVP derivation
    memcpy(prev_state->tile->frame->rec->ref_pocs, state->frame->ref->pocs, sizeof(int32_t)*state->frame->ref->used_size);

    // Add previous reconstructed picture as a reference
    uvg_image_list_add(state->frame->ref,
                   prev_state->tile->frame->rec,
                   prev_state->tile->frame->cu_array,
                   prev_state->frame->poc,
                   prev_state->frame->ref_LX);
    uvg_cu_array_free(&state->tile->frame->cu_array);
    if (state->tile->frame->chroma_cu_array) {
      uvg_cu_array_free(&state->tile->frame->chroma_cu_array);
    }
    unsigned height = state->tile->frame->height_in_lcu * LCU_WIDTH;
    unsigned width  = state->tile->frame->width_in_lcu  * LCU_WIDTH;
    state->tile->frame->cu_array = uvg_cu_array_alloc(width, height);
  }

  if (state->encoder_control->cfg.lmcs_enable) {
    uvg_image_free(state->tile->frame->source_lmcs);
    state->tile->frame->source_lmcs = NULL;

    uvg_image_free(state->tile->frame->rec_lmcs);
    state->tile->frame->rec_lmcs = NULL;
  }

  // Remove source and reconstructed picture.
  uvg_image_free(state->tile->frame->source);
  state->tile->frame->source = NULL;

  uvg_image_free(state->tile->frame->rec);
  state->tile->frame->rec = NULL;

  uvg_cu_array_free(&state->tile->frame->cu_array);
  if (state->tile->frame->chroma_cu_array) {
    uvg_cu_array_free(&state->tile->frame->chroma_cu_array);
  }

  // Update POC and frame count.
  state->frame->num = prev_state->frame->num + 1;
  state->frame->poc = prev_state->frame->poc + 1;
  state->frame->irap_poc = prev_state->frame->irap_poc;

  state->frame->prepared = 1;


}

coeff_scan_order_t uvg_get_scan_order(int8_t cu_type, int intra_mode, int depth)
{
  // Scan mode is diagonal, except for 4x4+8x8 luma and 4x4 chroma, where:
  // - angular 6-14 = vertical
  // - angular 22-30 = horizontal
#if HEVC_USE_MDCS
  if (cu_type == CU_INTRA && depth >= 3) {
    if (intra_mode >= 6 && intra_mode <= 14) {
      return SCAN_VER;
    } else if (intra_mode >= 22 && intra_mode <= 30) {
      return SCAN_HOR;
    }
  }
#endif
  return SCAN_DIAG;
}

lcu_stats_t* uvg_get_lcu_stats(encoder_state_t *state, int lcu_x, int lcu_y)
{
  const int index = lcu_x + state->tile->lcu_offset_x +
                    (lcu_y + state->tile->lcu_offset_y) *
                    state->encoder_control->in.width_in_lcu;
  return &state->frame->lcu_stats[index];
}

int uvg_get_cu_ref_qp(const encoder_state_t *state, int x, int y, int last_qp)
{
  const cu_array_t *cua = state->tile->frame->cu_array;
  // Quantization group width
  const int qg_width = 1 << MAX(6 - state->frame->max_qp_delta_depth, uvg_cu_array_at_const(cua, x, y)->log2_width);
  const int qg_height = 1 << MAX(6 - state->frame->max_qp_delta_depth, uvg_cu_array_at_const(cua, x, y)->log2_height);

  // Coordinates of the top-left corner of the quantization group
  const int x_qg = x & ~(qg_width - 1);
  const int y_qg = y & ~(qg_height - 1);
  if(x_qg == 0 && y_qg > 0 && y_qg % LCU_WIDTH == 0) {
    return uvg_cu_array_at_const(cua, x_qg, y_qg - 1)->qp;
  }

  int qp_pred_a = last_qp;
  if (x_qg % LCU_WIDTH > 0) {
    qp_pred_a = uvg_cu_array_at_const(cua, x_qg - 1, y_qg)->qp;
  }

  int qp_pred_b = last_qp;
  if (y_qg % LCU_WIDTH > 0) {
    qp_pred_b = uvg_cu_array_at_const(cua, x_qg, y_qg - 1)->qp;
  }

  return ((qp_pred_a + qp_pred_b + 1) >> 1);
}
