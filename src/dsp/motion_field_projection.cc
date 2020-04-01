// Copyright 2020 The libgav1 Authors
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "src/dsp/motion_field_projection.h"

#include <algorithm>
#include <cassert>
#include <cstddef>
#include <cstdint>

#include "src/dsp/dsp.h"
#include "src/utils/common.h"
#include "src/utils/constants.h"
#include "src/utils/types.h"

namespace libgav1 {
namespace dsp {
namespace {

// Silence unused function warnings when MotionFieldProjectionKernel_C is
// not used.
#if LIBGAV1_ENABLE_ALL_DSP_FUNCTIONS ||                      \
    !defined(LIBGAV1_Dsp8bpp_MotionFieldProjectionKernel) || \
    (LIBGAV1_MAX_BITDEPTH >= 10 &&                           \
     !defined(LIBGAV1_Dsp10bpp_MotionFieldProjectionKernel))

void CalculateReferenceFramesInfo(
    const int8_t reference_offsets[kNumReferenceFrameTypes],
    bool skip_references[kNumReferenceFrameTypes],
    int16_t projection_mv_divisions[kNumReferenceFrameTypes]) {
  // Initialize skip_references[kReferenceFrameIntra] to simplify branch
  // conditions in projection.
  skip_references[kReferenceFrameIntra] = true;
  for (int reference_type = kReferenceFrameLast;
       reference_type <= kNumInterReferenceFrameTypes; ++reference_type) {
    const int reference_offset = reference_offsets[reference_type];
    skip_references[reference_type] =
        reference_offset > kMaxFrameDistance || reference_offset <= 0;
    projection_mv_divisions[reference_type] =
        skip_references[reference_type]
            ? 0
            : kProjectionMvDivisionLookup[reference_offset];
  }
}

// 7.9.2.
void MotionFieldProjectionKernel_C(
    const ReferenceFrameType* source_reference_types, const MotionVector* mv,
    const int8_t reference_offsets[kNumReferenceFrameTypes],
    int reference_to_current_with_sign, int dst_sign, int y8_start, int y8_end,
    int x8_start, int x8_end, TemporalMotionField* motion_field) {
  const ptrdiff_t stride = motion_field->mv.columns();
  // The column range has to be offset by kProjectionMvMaxHorizontalOffset since
  // coordinates in that range could end up being position_x8 because of
  // projection.
  const int adjusted_x8_start =
      std::max(x8_start - kProjectionMvMaxHorizontalOffset, 0);
  const int adjusted_x8_end = std::min(
      x8_end + kProjectionMvMaxHorizontalOffset, static_cast<int>(stride));
  int8_t* dst_reference_offset = motion_field->reference_offset[y8_start];
  MotionVector* dst_mv = motion_field->mv[y8_start];
  bool skip_references[kNumReferenceFrameTypes];
  int16_t projection_mv_divisions[kNumReferenceFrameTypes];
  assert(stride == motion_field->reference_offset.columns());
  assert((y8_start & 7) == 0);
  CalculateReferenceFramesInfo(reference_offsets, skip_references,
                               projection_mv_divisions);

  int y8 = y8_start;
  do {
    const int y8_floor = (y8 & ~7) - y8;
    const int y8_ceiling = std::min(y8_end - y8, y8_floor + 8);
    int x8 = adjusted_x8_start;
    do {
      const int source_reference_type = source_reference_types[x8];
      if (skip_references[source_reference_type]) continue;
      MotionVector projection_mv;
      // reference_to_current_with_sign could be 0.
      GetMvProjection(mv[x8], reference_to_current_with_sign,
                      projection_mv_divisions[source_reference_type],
                      &projection_mv);
      // Do not update the motion vector if the block position is not valid or
      // if position_x8 is outside the current range of x8_start and x8_end.
      // Note that position_y8 will always be within the range of y8_start and
      // y8_end.
      const int position_y8 = Project(0, projection_mv.mv[0], dst_sign);
      if (position_y8 < y8_floor || position_y8 >= y8_ceiling) continue;
      const int x8_base = x8 & ~7;
      const int x8_floor =
          std::max(x8_start, x8_base - kProjectionMvMaxHorizontalOffset);
      const int x8_ceiling =
          std::min(x8_end, x8_base + 8 + kProjectionMvMaxHorizontalOffset);
      const int position_x8 = Project(x8, projection_mv.mv[1], dst_sign);
      if (position_x8 < x8_floor || position_x8 >= x8_ceiling) continue;
      dst_mv[position_y8 * stride + position_x8] = mv[x8];
      dst_reference_offset[position_y8 * stride + position_x8] =
          reference_offsets[source_reference_type];
    } while (++x8 < adjusted_x8_end);
    source_reference_types += stride;
    mv += stride;
    dst_reference_offset += stride;
    dst_mv += stride;
  } while (++y8 < y8_end);
}

#endif  // LIBGAV1_ENABLE_ALL_DSP_FUNCTIONS ||
        // !defined(LIBGAV1_Dsp8bpp_MotionFieldProjectionKernel) ||
        // (LIBGAV1_MAX_BITDEPTH >= 10 &&
        //  !defined(LIBGAV1_Dsp10bpp_MotionFieldProjectionKernel))

void Init8bpp() {
#if LIBGAV1_ENABLE_ALL_DSP_FUNCTIONS || \
    !defined(LIBGAV1_Dsp8bpp_MotionFieldProjectionKernel)
  Dsp* const dsp = dsp_internal::GetWritableDspTable(8);
  assert(dsp != nullptr);
  dsp->motion_field_projection_kernel = MotionFieldProjectionKernel_C;
#endif
}

#if LIBGAV1_MAX_BITDEPTH >= 10
void Init10bpp() {
#if LIBGAV1_ENABLE_ALL_DSP_FUNCTIONS || \
    !defined(LIBGAV1_Dsp10bpp_MotionFieldProjectionKernel)
  Dsp* const dsp = dsp_internal::GetWritableDspTable(10);
  assert(dsp != nullptr);
  dsp->motion_field_projection_kernel = MotionFieldProjectionKernel_C;
#endif
}
#endif

}  // namespace

void MotionFieldProjectionInit_C() {
  Init8bpp();
#if LIBGAV1_MAX_BITDEPTH >= 10
  Init10bpp();
#endif
}

}  // namespace dsp
}  // namespace libgav1
