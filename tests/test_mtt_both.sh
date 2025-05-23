#!/bin/sh

# Test combined intra/inter mtt coding.

set -eu

. "${0%/*}/util.sh"

common_args='264x130 10 yuv420p --preset=ultrafast --threads=0 --no-cpuid --no-wpp --fast-residual-cost 0'
valgrind_test $common_args --rd=0 --mtt-depth-intra 1 --pu-depth-intra 2-3 --mtt-depth-inter 1 --pu-depth-inter 2-3
valgrind_test $common_args --rd=3 --mtt-depth-intra 1 --pu-depth-intra 0-5 --mtt-depth-inter 1 --pu-depth-inter 0-5
valgrind_test $common_args --rd=3 --mtt-depth-intra 3 --pu-depth-intra 0-8 --mtt-depth-inter 3 --pu-depth-inter 0-8
valgrind_test $common_args --rd=3 --mtt-depth-intra 3 --mtt-depth-intra-chroma 3 --dual-tree --pu-depth-intra 0-8 --mtt-depth-inter 3 --pu-depth-inter 0-8
valgrind_test $common_args --rd=3 --rdoq --jccr --isp --lfnst --mip --mrl --mts both --cclm --mtt-depth-intra 3 --mtt-depth-intra-chroma 3 --dual-tree --pu-depth-intra 0-8 --mtt-depth-inter 3 --pu-depth-inter 0-8
