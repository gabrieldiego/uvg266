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

#include "strategies/generic/alf-generic.h"

#include "cu.h"
#include "encoder.h"
#include "encoderstate.h"
#include "uvg266.h"
#include "alf.h"
#include "strategyselector.h"

extern uvg_pixel uvg_fast_clip_32bit_to_pixel(int32_t value);

static int16_t clip_alf(const int16_t clip, const int16_t ref, const int16_t val0, const int16_t val1)
{
  return CLIP(-clip, +clip, val0 - ref) + CLIP(-clip, +clip, val1 - ref);
}

static void alf_derive_classification_blk_generic(encoder_state_t * const state,
  const int shift,
  const int n_height,
  const int n_width,
  const int blk_pos_x,
  const int blk_pos_y,
  const int blk_dst_x,
  const int blk_dst_y,
  const int vb_ctu_height,
  int vb_pos)
{
  videoframe_t* const frame = state->tile->frame;
  //int ***g_laplacian = state->tile->frame->alf_info->g_laplacian;
  //alf_classifier **g_classifier = state->tile->frame->alf_info->g_classifier;
  //CHECK((vb_ctu_height & (vb_ctu_height - 1)) != 0, "vb_ctu_height must be a power of 2");

  static const int th[16] = { 0, 1, 2, 2, 2, 2, 2, 3, 3, 3, 3, 3, 3, 3, 3, 4 };
  int laplacian[NUM_DIRECTIONS][CLASSIFICATION_BLK_SIZE + 5][CLASSIFICATION_BLK_SIZE + 5];
  memset(laplacian, 0, sizeof(laplacian));
  alf_classifier **classifier = state->tile->frame->alf_info->classifier;

  const int stride = frame->rec->stride_luma;
  uvg_pixel *src = state->tile->frame->rec->y;
  const int max_activity = 15;

  int fl = 2;
  int fl_p1 = fl + 1;
  int fl2 = 2 * fl;

  int main_direction, secondary_direction, dir_temp_hv, dir_temp_d;
  int pix_y;

  int height = n_height + fl2;
  int width = n_width + fl2;
  int pos_x = blk_pos_x;
  int pos_y = blk_pos_y;
  int start_height = pos_y - fl_p1;

  for (int i = 0; i < height; i += 2)
  {
    int yoffset = (i + 1 + start_height) * stride - fl_p1;
    const uvg_pixel *src0 = &src[yoffset - stride];
    const uvg_pixel *src1 = &src[yoffset];
    const uvg_pixel *src2 = &src[yoffset + stride];
    const uvg_pixel *src3 = &src[yoffset + stride * 2];

    const int y = blk_dst_y - 2 + i;
    if (y > 0 && (y & (vb_ctu_height - 1)) == vb_pos - 2)
    {
      src3 = &src[yoffset + stride];
    }
    else if (y > 0 && (y & (vb_ctu_height - 1)) == vb_pos)
    {
      src0 = &src[yoffset];
    }

    int *p_y_ver = laplacian[ALF_VER][i];
    int *p_y_hor = laplacian[ALF_HOR][i];
    int *p_y_dig_0 = laplacian[ALF_DIAG0][i];
    int *p_y_dig_1 = laplacian[ALF_DIAG1][i];

    for (int j = 0; j < width; j += 2)
    {
      pix_y = j + 1 + pos_x;
      const uvg_pixel *p_y = src1 + pix_y;
      const uvg_pixel *p_y_down = src0 + pix_y;
      const uvg_pixel *p_y_up = src2 + pix_y;
      const uvg_pixel *p_y_up2 = src3 + pix_y;

      const int16_t y0 = p_y[0] << 1;
      const int16_t y_up1 = p_y_up[1] << 1;

      p_y_ver[j] = abs(y0 - p_y_down[0] - p_y_up[0]) + abs(y_up1 - p_y[1] - p_y_up2[1]);
      p_y_hor[j] = abs(y0 - p_y[1] - p_y[-1]) + abs(y_up1 - p_y_up[2] - p_y_up[0]);
      p_y_dig_0[j] = abs(y0 - p_y_down[-1] - p_y_up[1]) + abs(y_up1 - p_y[0] - p_y_up2[2]);
      p_y_dig_1[j] = abs(y0 - p_y_up[-1] - p_y_down[1]) + abs(y_up1 - p_y_up2[0] - p_y[2]);

      if (j > 4 && (j - 6) % 4 == 0)
      {
        int j_m_6 = j - 6;
        int j_m_4 = j - 4;
        int j_m_2 = j - 2;

        p_y_ver[j_m_6] += p_y_ver[j_m_4] + p_y_ver[j_m_2] + p_y_ver[j];
        p_y_hor[j_m_6] += p_y_hor[j_m_4] + p_y_hor[j_m_2] + p_y_hor[j];
        p_y_dig_0[j_m_6] += p_y_dig_0[j_m_4] + p_y_dig_0[j_m_2] + p_y_dig_0[j];
        p_y_dig_1[j_m_6] += p_y_dig_1[j_m_4] + p_y_dig_1[j_m_2] + p_y_dig_1[j];
      }
    }
  }

  // classification block size
  const int cls_size_y = 4;
  const int cls_size_x = 4;

  //for (int i = 0; i < blk.height; i += cls_size_y)
  for (int i = 0; i < n_height; i += cls_size_y)
  {
    int* p_y_ver = laplacian[ALF_VER][i];
    int* p_y_ver2 = laplacian[ALF_VER][i + 2];
    int* p_y_ver4 = laplacian[ALF_VER][i + 4];
    int* p_y_ver6 = laplacian[ALF_VER][i + 6];

    int* p_y_hor = laplacian[ALF_HOR][i];
    int* p_y_hor2 = laplacian[ALF_HOR][i + 2];
    int* p_y_hor4 = laplacian[ALF_HOR][i + 4];
    int* p_y_hor6 = laplacian[ALF_HOR][i + 6];

    int* p_y_dig0 = laplacian[ALF_DIAG0][i];
    int* p_y_dig02 = laplacian[ALF_DIAG0][i + 2];
    int* p_y_dig04 = laplacian[ALF_DIAG0][i + 4];
    int* p_y_dig06 = laplacian[ALF_DIAG0][i + 6];

    int* p_y_dig1 = laplacian[ALF_DIAG1][i];
    int* p_y_dig12 = laplacian[ALF_DIAG1][i + 2];
    int* p_y_dig14 = laplacian[ALF_DIAG1][i + 4];
    int* p_y_dig16 = laplacian[ALF_DIAG1][i + 6];

    //for (int j = 0; j < blk.width; j += cls_size_x)
    for (int j = 0; j < n_width; j += cls_size_x)
    {
      int sum_v = 0; int sum_h = 0; int sum_d0 = 0; int sum_d1 = 0;

      if (((i + blk_dst_y) % vb_ctu_height) == (vb_pos - 4))
      {
        sum_v = p_y_ver[j] + p_y_ver2[j] + p_y_ver4[j];
        sum_h = p_y_hor[j] + p_y_hor2[j] + p_y_hor4[j];
        sum_d0 = p_y_dig0[j] + p_y_dig02[j] + p_y_dig04[j];
        sum_d1 = p_y_dig1[j] + p_y_dig12[j] + p_y_dig14[j];
      }
      else if (((i + blk_dst_y) % vb_ctu_height) == vb_pos)
      {
        sum_v = p_y_ver2[j] + p_y_ver4[j] + p_y_ver6[j];
        sum_h = p_y_hor2[j] + p_y_hor4[j] + p_y_hor6[j];
        sum_d0 = p_y_dig02[j] + p_y_dig04[j] + p_y_dig06[j];
        sum_d1 = p_y_dig12[j] + p_y_dig14[j] + p_y_dig16[j];
      }
      else
      {
        sum_v = p_y_ver[j] + p_y_ver2[j] + p_y_ver4[j] + p_y_ver6[j];
        sum_h = p_y_hor[j] + p_y_hor2[j] + p_y_hor4[j] + p_y_hor6[j];
        sum_d0 = p_y_dig0[j] + p_y_dig02[j] + p_y_dig04[j] + p_y_dig06[j];
        sum_d1 = p_y_dig1[j] + p_y_dig12[j] + p_y_dig14[j] + p_y_dig16[j];
      }

      int temp_act = sum_v + sum_h;
      int activity = 0;

      const int y = (i + blk_dst_y) & (vb_ctu_height - 1);
      if (y == vb_pos - 4 || y == vb_pos)
      {
        activity = CLIP(0, max_activity, (temp_act * 96) >> shift);
      }
      else
      {
        activity = CLIP(0, max_activity, (temp_act * 64) >> shift);
      }

      int class_idx = th[activity];

      int hv1, hv0, d1, d0, hvd1, hvd0;

      if (sum_v > sum_h)
      {
        hv1 = sum_v;
        hv0 = sum_h;
        dir_temp_hv = 1;
      }
      else
      {
        hv1 = sum_h;
        hv0 = sum_v;
        dir_temp_hv = 3;
      }
      if (sum_d0 > sum_d1)
      {
        d1 = sum_d0;
        d0 = sum_d1;
        dir_temp_d = 0;
      }
      else
      {
        d1 = sum_d1;
        d0 = sum_d0;
        dir_temp_d = 2;
      }
      if ((uint32_t)d1 * (uint32_t)hv0 > (uint32_t)hv1 * (uint32_t)d0)
      {
        hvd1 = d1;
        hvd0 = d0;
        main_direction = dir_temp_d;
        secondary_direction = dir_temp_hv;
      }
      else
      {
        hvd1 = hv1;
        hvd0 = hv0;
        main_direction = dir_temp_hv;
        secondary_direction = dir_temp_d;
      }

      int direction_strength = 0;
      if (hvd1 > 2 * hvd0)
      {
        direction_strength = 1;
      }
      if (hvd1 * 2 > 9 * hvd0)
      {
        direction_strength = 2;
      }

      if (direction_strength)
      {
        class_idx += (((main_direction & 0x1) << 1) + direction_strength) * 5;
      }

      static const int transpose_table[8] = { 0, 1, 0, 2, 2, 3, 1, 3 };
      int transpose_idx = transpose_table[main_direction * 2 + (secondary_direction >> 1)];

      int y_offset = i + blk_dst_y;
      int x_offset = j + blk_dst_x;

      alf_classifier *cl0 = classifier[y_offset] + x_offset;
      alf_classifier *cl1 = classifier[y_offset + 1] + x_offset;
      alf_classifier *cl2 = classifier[y_offset + 2] + x_offset;
      alf_classifier *cl3 = classifier[y_offset + 3] + x_offset;

      cl0[0].class_idx = cl0[1].class_idx = cl0[2].class_idx = cl0[3].class_idx =
        cl1[0].class_idx = cl1[1].class_idx = cl1[2].class_idx = cl1[3].class_idx =
        cl2[0].class_idx = cl2[1].class_idx = cl2[2].class_idx = cl2[3].class_idx =
        cl3[0].class_idx = cl3[1].class_idx = cl3[2].class_idx = cl3[3].class_idx = class_idx;

      cl0[0].transpose_idx = cl0[1].transpose_idx = cl0[2].transpose_idx = cl0[3].transpose_idx =
        cl1[0].transpose_idx = cl1[1].transpose_idx = cl1[2].transpose_idx = cl1[3].transpose_idx =
        cl2[0].transpose_idx = cl2[1].transpose_idx = cl2[2].transpose_idx = cl2[3].transpose_idx =
        cl3[0].transpose_idx = cl3[1].transpose_idx = cl3[2].transpose_idx = cl3[3].transpose_idx = transpose_idx;

    }
  }
}

static void alf_filter_block_generic(encoder_state_t* const state,
  const uvg_pixel* src_pixels,
  uvg_pixel* dst_pixels,
  const int src_stride,
  const int dst_stride,
  const short* filter_set,
  const int16_t* fClipSet,
  clp_rng clp_rng,
  alf_component_id component_id,
  const int width,
  const int height,
  int x_pos,
  int y_pos,
  int blk_dst_x,
  int blk_dst_y,
  int vb_pos,
  const int vb_ctu_height)
{
  alf_filter_type const filter_type = component_id == COMPONENT_Y ? ALF_FILTER_7X7 : ALF_FILTER_5X5;
  const bool chroma = component_id == COMPONENT_Y ? 0 : 1;
  const int8_t bit_depth = state->encoder_control->bitdepth;

  if (chroma)
  {
    assert((int)filter_type == 0); //Chroma needs to have filtType == 0
  }

  const int start_height = y_pos;
  const int end_height = start_height + height;
  const int start_width = x_pos;
  const int end_width = start_width + width;

  const uvg_pixel* src = src_pixels;
  uvg_pixel* dst = dst_pixels + blk_dst_y * dst_stride;

  const uvg_pixel* p_img_y_pad_0, * p_img_y_pad_1, * p_img_y_pad_2, * p_img_y_pad_3, * p_img_y_pad_4, * p_img_y_pad_5, * p_img_y_pad_6;
  const uvg_pixel* p_img_0, * p_img_1, * p_img_2, * p_img_3, * p_img_4, * p_img_5, * p_img_6;

  const short* coef = filter_set;
  const int16_t* clip = fClipSet;

  const int shift = bit_depth - 1;

  const int offset = 1 << (shift - 1);

  int transpose_idx = 0;
  const int cls_size_y = 4;
  const int cls_size_x = 4;

  assert((start_height % cls_size_y) == 0); //Wrong startHeight in filtering
  assert((start_width % cls_size_x) == 0); //Wrong startWidth in filtering
  assert(((end_height - start_height) % cls_size_y) == 0); //Wrong endHeight in filtering
  assert(((end_width - start_width) % cls_size_x) == 0); //Wrong endWidth in filtering

  alf_classifier* p_class = NULL;

  int dst_stride2 = dst_stride * cls_size_y;
  int src_stride2 = src_stride * cls_size_y;

  //std::vector<Pel> filter_coeff(MAX_NUM_ALF_LUMA_COEFF);
  int filter_coeff[MAX_NUM_ALF_LUMA_COEFF];
  memset(filter_coeff, 0, MAX_NUM_ALF_LUMA_COEFF * sizeof(int));
  //std::array<int, MAX_NUM_ALF_LUMA_COEFF> filterClipp;
  int filter_clipp[MAX_NUM_ALF_LUMA_COEFF];
  memset(filter_clipp, 0, MAX_NUM_ALF_LUMA_COEFF * sizeof(int));

  p_img_y_pad_0 = src + start_height * src_stride + start_width;
  p_img_y_pad_1 = p_img_y_pad_0 + src_stride;
  p_img_y_pad_2 = p_img_y_pad_0 - src_stride;
  p_img_y_pad_3 = p_img_y_pad_1 + src_stride;
  p_img_y_pad_4 = p_img_y_pad_2 - src_stride;
  p_img_y_pad_5 = p_img_y_pad_3 + src_stride;
  p_img_y_pad_6 = p_img_y_pad_4 - src_stride;

  uvg_pixel* p_rec_0 = dst + blk_dst_x;//start_width;
  uvg_pixel* p_rec_1 = p_rec_0 + dst_stride;

  for (int i = 0; i < end_height - start_height; i += cls_size_y)
  {
    if (!chroma)
    {
      p_class = state->tile->frame->alf_info->classifier[blk_dst_y + i] + blk_dst_x;
    }

    for (int j = 0; j < end_width - start_width; j += cls_size_x)
    {
      if (!chroma)
      {
        alf_classifier cl = p_class[j];
        transpose_idx = cl.transpose_idx;
        coef = filter_set + cl.class_idx * MAX_NUM_ALF_LUMA_COEFF;
        clip = fClipSet + cl.class_idx * MAX_NUM_ALF_LUMA_COEFF;
      }

      if (filter_type == ALF_FILTER_7X7)
      {
        if (transpose_idx == 1)
        {
          filter_coeff[0] = coef[9];
          filter_coeff[1] = coef[4];
          filter_coeff[2] = coef[10];
          filter_coeff[3] = coef[8];
          filter_coeff[4] = coef[1];
          filter_coeff[5] = coef[5];
          filter_coeff[6] = coef[11];
          filter_coeff[7] = coef[7];
          filter_coeff[8] = coef[3];
          filter_coeff[9] = coef[0];
          filter_coeff[10] = coef[2];
          filter_coeff[11] = coef[6];
          filter_coeff[12] = coef[12];

          filter_clipp[0] = clip[9];
          filter_clipp[1] = clip[4];
          filter_clipp[2] = clip[10];
          filter_clipp[3] = clip[8];
          filter_clipp[4] = clip[1];
          filter_clipp[5] = clip[5];
          filter_clipp[6] = clip[11];
          filter_clipp[7] = clip[7];
          filter_clipp[8] = clip[3];
          filter_clipp[9] = clip[0];
          filter_clipp[10] = clip[2];
          filter_clipp[11] = clip[6];
          filter_clipp[12] = clip[12];
        }
        else if (transpose_idx == 2)
        {
          filter_coeff[0] = coef[0];
          filter_coeff[1] = coef[3];
          filter_coeff[2] = coef[2];
          filter_coeff[3] = coef[1];
          filter_coeff[4] = coef[8];
          filter_coeff[5] = coef[7];
          filter_coeff[6] = coef[6];
          filter_coeff[7] = coef[5];
          filter_coeff[8] = coef[4];
          filter_coeff[9] = coef[9];
          filter_coeff[10] = coef[10];
          filter_coeff[11] = coef[11];
          filter_coeff[12] = coef[12];

          filter_clipp[0] = clip[0];
          filter_clipp[1] = clip[3];
          filter_clipp[2] = clip[2];
          filter_clipp[3] = clip[1];
          filter_clipp[4] = clip[8];
          filter_clipp[5] = clip[7];
          filter_clipp[6] = clip[6];
          filter_clipp[7] = clip[5];
          filter_clipp[8] = clip[4];
          filter_clipp[9] = clip[9];
          filter_clipp[10] = clip[10];
          filter_clipp[11] = clip[11];
          filter_clipp[12] = clip[12];

        }
        else if (transpose_idx == 3)
        {
          filter_coeff[0] = coef[9];
          filter_coeff[1] = coef[8];
          filter_coeff[2] = coef[10];
          filter_coeff[3] = coef[4];
          filter_coeff[4] = coef[3];
          filter_coeff[5] = coef[7];
          filter_coeff[6] = coef[11];
          filter_coeff[7] = coef[5];
          filter_coeff[8] = coef[1];
          filter_coeff[9] = coef[0];
          filter_coeff[10] = coef[2];
          filter_coeff[11] = coef[6];
          filter_coeff[12] = coef[12];

          filter_clipp[0] = clip[9];
          filter_clipp[1] = clip[8];
          filter_clipp[2] = clip[10];
          filter_clipp[3] = clip[4];
          filter_clipp[4] = clip[3];
          filter_clipp[5] = clip[7];
          filter_clipp[6] = clip[11];
          filter_clipp[7] = clip[5];
          filter_clipp[8] = clip[1];
          filter_clipp[9] = clip[0];
          filter_clipp[10] = clip[2];
          filter_clipp[11] = clip[6];
          filter_clipp[12] = clip[12];
        }
        else
        {
          filter_coeff[0] = coef[0];
          filter_coeff[1] = coef[1];
          filter_coeff[2] = coef[2];
          filter_coeff[3] = coef[3];
          filter_coeff[4] = coef[4];
          filter_coeff[5] = coef[5];
          filter_coeff[6] = coef[6];
          filter_coeff[7] = coef[7];
          filter_coeff[8] = coef[8];
          filter_coeff[9] = coef[9];
          filter_coeff[10] = coef[10];
          filter_coeff[11] = coef[11];
          filter_coeff[12] = coef[12];

          filter_clipp[0] = clip[0];
          filter_clipp[1] = clip[1];
          filter_clipp[2] = clip[2];
          filter_clipp[3] = clip[3];
          filter_clipp[4] = clip[4];
          filter_clipp[5] = clip[5];
          filter_clipp[6] = clip[6];
          filter_clipp[7] = clip[7];
          filter_clipp[8] = clip[8];
          filter_clipp[9] = clip[9];
          filter_clipp[10] = clip[10];
          filter_clipp[11] = clip[11];
          filter_clipp[12] = clip[12];
        }
      }
      else
      {
        if (transpose_idx == 1)
        {
          filter_coeff[0] = coef[4];
          filter_coeff[1] = coef[1];
          filter_coeff[2] = coef[5];
          filter_coeff[3] = coef[3];
          filter_coeff[4] = coef[0];
          filter_coeff[5] = coef[2];
          filter_coeff[6] = coef[6];

          filter_clipp[0] = clip[4];
          filter_clipp[1] = clip[1];
          filter_clipp[2] = clip[5];
          filter_clipp[3] = clip[3];
          filter_clipp[4] = clip[0];
          filter_clipp[5] = clip[2];
          filter_clipp[6] = clip[6];

        }
        else if (transpose_idx == 2)
        {
          filter_coeff[0] = coef[0];
          filter_coeff[1] = coef[3];
          filter_coeff[2] = coef[2];
          filter_coeff[3] = coef[1];
          filter_coeff[4] = coef[4];
          filter_coeff[5] = coef[5];
          filter_coeff[6] = coef[6];

          filter_clipp[0] = clip[0];
          filter_clipp[1] = clip[3];
          filter_clipp[2] = clip[2];
          filter_clipp[3] = clip[1];
          filter_clipp[4] = clip[4];
          filter_clipp[5] = clip[5];
          filter_clipp[6] = clip[6];

        }
        else if (transpose_idx == 3)
        {
          filter_coeff[0] = coef[4];
          filter_coeff[1] = coef[3];
          filter_coeff[2] = coef[5];
          filter_coeff[3] = coef[1];
          filter_coeff[4] = coef[0];
          filter_coeff[5] = coef[2];
          filter_coeff[6] = coef[6];

          filter_clipp[0] = clip[4];
          filter_clipp[1] = clip[3];
          filter_clipp[2] = clip[5];
          filter_clipp[3] = clip[1];
          filter_clipp[4] = clip[0];
          filter_clipp[5] = clip[2];
          filter_clipp[6] = clip[6];

        }
        else
        {
          filter_coeff[0] = coef[0];
          filter_coeff[1] = coef[1];
          filter_coeff[2] = coef[2];
          filter_coeff[3] = coef[3];
          filter_coeff[4] = coef[4];
          filter_coeff[5] = coef[5];
          filter_coeff[6] = coef[6];

          filter_clipp[0] = clip[0];
          filter_clipp[1] = clip[1];
          filter_clipp[2] = clip[2];
          filter_clipp[3] = clip[3];
          filter_clipp[4] = clip[4];
          filter_clipp[5] = clip[5];
          filter_clipp[6] = clip[6];

        }
      }

      for (int ii = 0; ii < cls_size_y; ii++)
      {
        p_img_0 = p_img_y_pad_0 + j + ii * src_stride;
        p_img_1 = p_img_y_pad_1 + j + ii * src_stride;
        p_img_2 = p_img_y_pad_2 + j + ii * src_stride;
        p_img_3 = p_img_y_pad_3 + j + ii * src_stride;
        p_img_4 = p_img_y_pad_4 + j + ii * src_stride;
        p_img_5 = p_img_y_pad_5 + j + ii * src_stride;
        p_img_6 = p_img_y_pad_6 + j + ii * src_stride;

        p_rec_1 = p_rec_0 + j + ii * dst_stride;

        const int y_vb = (blk_dst_y + i + ii) & (vb_ctu_height - 1);

        if (y_vb < vb_pos && (y_vb >= vb_pos - (chroma ? 2 : 4)))   // above
        {
          p_img_1 = (y_vb == vb_pos - 1) ? p_img_0 : p_img_1;
          p_img_3 = (y_vb >= vb_pos - 2) ? p_img_1 : p_img_3;
          p_img_5 = (y_vb >= vb_pos - 3) ? p_img_3 : p_img_5;

          p_img_2 = (y_vb == vb_pos - 1) ? p_img_0 : p_img_2;
          p_img_4 = (y_vb >= vb_pos - 2) ? p_img_2 : p_img_4;
          p_img_6 = (y_vb >= vb_pos - 3) ? p_img_4 : p_img_6;
        }

        else if (y_vb >= vb_pos && (y_vb <= vb_pos + (chroma ? 1 : 3)))   // bottom
        {
          p_img_2 = (y_vb == vb_pos) ? p_img_0 : p_img_2;
          p_img_4 = (y_vb <= vb_pos + 1) ? p_img_2 : p_img_4;
          p_img_6 = (y_vb <= vb_pos + 2) ? p_img_4 : p_img_6;

          p_img_1 = (y_vb == vb_pos) ? p_img_0 : p_img_1;
          p_img_3 = (y_vb <= vb_pos + 1) ? p_img_1 : p_img_3;
          p_img_5 = (y_vb <= vb_pos + 2) ? p_img_3 : p_img_5;
        }

        bool is_near_vb_above = y_vb < vb_pos && (y_vb >= vb_pos - 1);
        bool is_near_vb_below = y_vb >= vb_pos && (y_vb <= vb_pos);
        for (int jj = 0; jj < cls_size_x; jj++)
        {
          int sum = 0;
          const uvg_pixel curr = p_img_0[+0];

          if (filter_type == ALF_FILTER_7X7)
          {
            sum += filter_coeff[0] * (clip_alf(filter_clipp[0], curr, p_img_5[+0], p_img_6[+0]));

            sum += filter_coeff[1] * (clip_alf(filter_clipp[1], curr, p_img_3[+1], p_img_4[-1]));
            sum += filter_coeff[2] * (clip_alf(filter_clipp[2], curr, p_img_3[+0], p_img_4[+0]));
            sum += filter_coeff[3] * (clip_alf(filter_clipp[3], curr, p_img_3[-1], p_img_4[+1]));

            sum += filter_coeff[4] * (clip_alf(filter_clipp[4], curr, p_img_1[+2], p_img_2[-2]));
            sum += filter_coeff[5] * (clip_alf(filter_clipp[5], curr, p_img_1[+1], p_img_2[-1]));
            sum += filter_coeff[6] * (clip_alf(filter_clipp[6], curr, p_img_1[+0], p_img_2[+0]));
            sum += filter_coeff[7] * (clip_alf(filter_clipp[7], curr, p_img_1[-1], p_img_2[+1]));
            sum += filter_coeff[8] * (clip_alf(filter_clipp[8], curr, p_img_1[-2], p_img_2[+2]));

            sum += filter_coeff[9] * (clip_alf(filter_clipp[9], curr, p_img_0[+3], p_img_0[-3]));
            sum += filter_coeff[10] * (clip_alf(filter_clipp[10], curr, p_img_0[+2], p_img_0[-2]));
            sum += filter_coeff[11] * (clip_alf(filter_clipp[11], curr, p_img_0[+1], p_img_0[-1]));
          }
          else
          {
            sum += filter_coeff[0] * (clip_alf(filter_clipp[0], curr, p_img_3[+0], p_img_4[+0]));

            sum += filter_coeff[1] * (clip_alf(filter_clipp[1], curr, p_img_1[+1], p_img_2[-1]));
            sum += filter_coeff[2] * (clip_alf(filter_clipp[2], curr, p_img_1[+0], p_img_2[+0]));
            sum += filter_coeff[3] * (clip_alf(filter_clipp[3], curr, p_img_1[-1], p_img_2[+1]));

            sum += filter_coeff[4] * (clip_alf(filter_clipp[4], curr, p_img_0[+2], p_img_0[-2]));
            sum += filter_coeff[5] * (clip_alf(filter_clipp[5], curr, p_img_0[+1], p_img_0[-1]));
          }

          if (!(is_near_vb_above || is_near_vb_below))
          {
            sum = (sum + offset) >> shift;
          }
          else
          {
            sum = (sum + (1 << ((shift + 3) - 1))) >> (shift + 3);
          }
          sum += curr;

          p_rec_1[jj] = uvg_fast_clip_32bit_to_pixel(sum);

          p_img_0++;
          p_img_1++;
          p_img_2++;
          p_img_3++;
          p_img_4++;
          p_img_5++;
          p_img_6++;
        }
      }
    }

    p_rec_0 += dst_stride2;
    p_rec_1 += dst_stride2;

    p_img_y_pad_0 += src_stride2;
    p_img_y_pad_1 += src_stride2;
    p_img_y_pad_2 += src_stride2;
    p_img_y_pad_3 += src_stride2;
    p_img_y_pad_4 += src_stride2;
    p_img_y_pad_5 += src_stride2;
    p_img_y_pad_6 += src_stride2;
  }
}


static void alf_filter_5x5_block_generic(encoder_state_t* const state,
  const uvg_pixel* src_pixels,
  uvg_pixel* dst_pixels,
  const int src_stride,
  const int dst_stride,
  const short* filter_set,
  const int16_t* fClipSet,
  clp_rng clp_rng,
  const int width,
  const int height,
  int x_pos,
  int y_pos,
  int blk_dst_x,
  int blk_dst_y,
  int vb_pos,
  const int vb_ctu_height)
{
  alf_filter_block_generic(state, src_pixels, dst_pixels, src_stride, dst_stride, 
    filter_set, fClipSet, clp_rng, COMPONENT_Cb, width, height, x_pos, y_pos, blk_dst_x, blk_dst_y, vb_pos, vb_ctu_height);
}

static void alf_filter_7x7_block_generic(encoder_state_t* const state,
  const uvg_pixel* src_pixels,
  uvg_pixel* dst_pixels,
  const int src_stride,
  const int dst_stride,
  const short* filter_set,
  const int16_t* fClipSet,
  clp_rng clp_rng,
  const int width,
  const int height,
  int x_pos,
  int y_pos,
  int blk_dst_x,
  int blk_dst_y,
  int vb_pos,
  const int vb_ctu_height)
{
  alf_filter_block_generic(state, src_pixels, dst_pixels, src_stride, dst_stride, filter_set, fClipSet, clp_rng, COMPONENT_Y, width, height, x_pos, y_pos, blk_dst_x, blk_dst_y, vb_pos, vb_ctu_height);
}




static void alf_calc_covariance_generic(int16_t e_local[MAX_NUM_ALF_LUMA_COEFF][MAX_ALF_NUM_CLIPPING_VALUES],
  const uvg_pixel* rec,
  const int stride,
  const channel_type channel,
  const int transpose_idx,
  int vb_distance,
  short alf_clipping_values[MAX_NUM_CHANNEL_TYPE][MAX_ALF_NUM_CLIPPING_VALUES])
{
  static const int alf_pattern_5[13] = {
              0,
          1,  2,  3,
      4,  5,  6,  5,  4,
          3,  2,  1,
              0
  };

  static const int alf_pattern_7[25] = {
                0,
            1,  2,  3,
        4,  5,  6,  7,  8,
    9, 10, 11, 12, 11, 10, 9,
        8,  7,  6,  5,  4,
            3,  2,  1,
                0
  };

  int clip_top_row = -4;
  int clip_bot_row = 4;
  if (vb_distance >= -3 && vb_distance < 0)
  {
    clip_bot_row = -vb_distance - 1;
    clip_top_row = -clip_bot_row; // symmetric
  }
  else if (vb_distance >= 0 && vb_distance < 3)
  {
    clip_top_row = -vb_distance;
    clip_bot_row = -clip_top_row; // symmetric
  }

  const bool is_luma = channel == CHANNEL_TYPE_LUMA;
  const int* filter_pattern = is_luma ? alf_pattern_7 : alf_pattern_5;
  const int half_filter_length = (is_luma ? 7 : 5) >> 1;
  const short* clip = alf_clipping_values[channel];
  const int num_bins = MAX_ALF_NUM_CLIPPING_VALUES;

  int k = 0;

  const int16_t curr = rec[0];

  if (transpose_idx == 0)
  {
    for (int i = -half_filter_length; i < 0; i++)
    {
      const uvg_pixel* rec0 = rec + MAX(i, clip_top_row) * stride;
      const uvg_pixel* rec1 = rec - MAX(i, -clip_bot_row) * stride;
      for (int j = -half_filter_length - i; j <= half_filter_length + i; j++, k++)
      {
        for (int b = 0; b < num_bins; b++)
        {
          e_local[filter_pattern[k]][b] += clip_alf(clip[b], curr, rec0[j], rec1[-j]);
        }
      }
    }
    for (int j = -half_filter_length; j < 0; j++, k++)
    {
      for (int b = 0; b < num_bins; b++)
      {
        e_local[filter_pattern[k]][b] += clip_alf(clip[b], curr, rec[j], rec[-j]);
      }
    }
  }
  else if (transpose_idx == 1)
  {
    for (int j = -half_filter_length; j < 0; j++)
    {
      const uvg_pixel* rec0 = rec + j;
      const uvg_pixel* rec1 = rec - j;

      for (int i = -half_filter_length - j; i <= half_filter_length + j; i++, k++)
      {
        for (int b = 0; b < num_bins; b++)
        {
          e_local[filter_pattern[k]][b] += clip_alf(clip[b], curr, rec0[MAX(i, clip_top_row) * stride], rec1[-MAX(i, -clip_bot_row) * stride]);
        }
      }
    }
    for (int i = -half_filter_length; i < 0; i++, k++)
    {
      for (int b = 0; b < num_bins; b++)
      {
        e_local[filter_pattern[k]][b] += clip_alf(clip[b], curr, rec[MAX(i, clip_top_row) * stride], rec[-MAX(i, -clip_bot_row) * stride]);
      }
    }
  }
  else if (transpose_idx == 2)
  {
    for (int i = -half_filter_length; i < 0; i++)
    {
      const uvg_pixel* rec0 = rec + MAX(i, clip_top_row) * stride;
      const uvg_pixel* rec1 = rec - MAX(i, -clip_bot_row) * stride;

      for (int j = half_filter_length + i; j >= -half_filter_length - i; j--, k++)
      {
        for (int b = 0; b < num_bins; b++)
        {
          e_local[filter_pattern[k]][b] += clip_alf(clip[b], curr, rec0[j], rec1[-j]);
        }
      }
    }
    for (int j = -half_filter_length; j < 0; j++, k++)
    {
      for (int b = 0; b < num_bins; b++)
      {
        e_local[filter_pattern[k]][b] += clip_alf(clip[b], curr, rec[j], rec[-j]);
      }
    }
  }
  else
  {
    for (int j = -half_filter_length; j < 0; j++)
    {
      const uvg_pixel* rec0 = rec + j;
      const uvg_pixel* rec1 = rec - j;

      for (int i = half_filter_length + j; i >= -half_filter_length - j; i--, k++)
      {
        for (int b = 0; b < num_bins; b++)
        {
          e_local[filter_pattern[k]][b] += clip_alf(clip[b], curr, rec0[MAX(i, clip_top_row) * stride], rec1[-MAX(i, -clip_bot_row) * stride]);
        }
      }
    }
    for (int i = -half_filter_length; i < 0; i++, k++)
    {
      for (int b = 0; b < num_bins; b++)
      {
        e_local[filter_pattern[k]][b] += clip_alf(clip[b], curr, rec[MAX(i, clip_top_row) * stride], rec[-MAX(i, -clip_bot_row) * stride]);
      }
    }
  }
  for (int b = 0; b < num_bins; b++)
  {
    e_local[filter_pattern[k]][b] += curr;
  }
}

static void alf_get_blk_stats_generic(encoder_state_t* const state,
  channel_type channel,
  alf_covariance* alf_covariance,
  alf_classifier** g_classifier,
  uvg_pixel* org,
  int32_t org_stride,
  uvg_pixel* rec,
  int32_t rec_stride,
  const int x_pos,
  const int y_pos,
  const int x_dst,
  const int y_dst,
  const int width,
  const int height,
  int vb_ctu_height,
  int vb_pos,
  short alf_clipping_values[MAX_NUM_CHANNEL_TYPE][MAX_ALF_NUM_CLIPPING_VALUES])
{
  int16_t e_local[MAX_NUM_ALF_LUMA_COEFF][MAX_ALF_NUM_CLIPPING_VALUES];

  const int num_bins = MAX_ALF_NUM_CLIPPING_VALUES;

  int num_coeff = channel == CHANNEL_TYPE_LUMA ? 13 : 7;
  int transpose_idx = 0;
  int class_idx = 0;

  for (int i = 0; i < height; i++)
  {
    int vb_distance = ((y_dst + i) % vb_ctu_height) - vb_pos;
    for (int j = 0; j < width; j++)
    {
      if (g_classifier && g_classifier[y_dst + i][x_dst + j].class_idx == ALF_UNUSED_CLASS_IDX && g_classifier[y_dst + i][x_dst + j].transpose_idx == ALF_UNUSED_TRANSPOSE_IDX)
      {
        continue;
      }
      memset(e_local, 0, sizeof(e_local));
      if (g_classifier)
      {
        alf_classifier* cl = &g_classifier[y_dst + i][x_dst + j];
        transpose_idx = cl->transpose_idx;
        class_idx = cl->class_idx;
      }

      double weight = 1.0;
      if (0/*m_alfWSSD*/)
      {
        //weight = g_luma_level_to_weight_plut[org[j]];
      }
      int16_t y_local = org[j] - rec[j];
      alf_calc_covariance_generic(e_local, rec + j, rec_stride, channel, transpose_idx, vb_distance, alf_clipping_values);
      for (int k = 0; k < num_coeff; k++)
      {
        for (int l = k; l < num_coeff; l++)
        {
          for (int b0 = 0; b0 < num_bins; b0++)
          {
            for (int b1 = 0; b1 < num_bins; b1++)
            {
              if (0/*m_alfWSSD*/)
              {
                alf_covariance[class_idx].ee[k][l][b0][b1] += (int64_t)(weight * (e_local[k][b0] * (double)e_local[l][b1]));
              }
              else
              {
                alf_covariance[class_idx].ee[k][l][b0][b1] += (int32_t)(e_local[k][b0] * (double)e_local[l][b1]);
              }
            }
          }
        }
        for (int b = 0; b < num_bins; b++)
        {
          if (0/*m_alfWSSD*/)
          {
            alf_covariance[class_idx].y[k][b] += (int32_t)(weight * (e_local[k][b] * (double)y_local));
          }
          else
          {
            alf_covariance[class_idx].y[k][b] += (int32_t)(e_local[k][b] * (double)y_local);
          }
        }
      }
      if (0/*m_alfWSSD*/)
      {
        alf_covariance[class_idx].pix_acc += weight * (y_local * (double)y_local);
      }
      else
      {
        alf_covariance[class_idx].pix_acc += y_local * (double)y_local;
      }
    }
    org += org_stride;
    rec += rec_stride;
  }

  int num_classes = g_classifier ? MAX_NUM_ALF_CLASSES : 1;
  for (class_idx = 0; class_idx < num_classes; class_idx++)
  {
    for (int k = 1; k < num_coeff; k++)
    {
      for (int l = 0; l < k; l++)
      {
        for (int b0 = 0; b0 < num_bins; b0++)
        {
          for (int b1 = 0; b1 < num_bins; b1++)
          {
            alf_covariance[class_idx].ee[k][l][b0][b1] = alf_covariance[class_idx].ee[l][k][b1][b0];
          }
        }
      }
    }
  }
}


int uvg_strategy_register_alf_generic(void* opaque, uint8_t bitdepth)
{
  bool success = true;

  success &= uvg_strategyselector_register(opaque, "alf_derive_classification_blk", "generic", 0, &alf_derive_classification_blk_generic);
  success &= uvg_strategyselector_register(opaque, "alf_filter_5x5_blk", "generic", 0, &alf_filter_5x5_block_generic);
  success &= uvg_strategyselector_register(opaque, "alf_filter_7x7_blk", "generic", 0, &alf_filter_7x7_block_generic);
  success &= uvg_strategyselector_register(opaque, "alf_get_blk_stats", "generic", 0, &alf_get_blk_stats_generic);
  

  return success;
}
