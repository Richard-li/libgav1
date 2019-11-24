// Copyright 2019 The libgav1 Authors
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

#include "src/dsp/film_grain.h"
#include "src/utils/cpu.h"

#if LIBGAV1_ENABLE_NEON
#include <arm_neon.h>

#include <algorithm>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <new>

#include "src/dsp/arm/common_neon.h"
#include "src/dsp/arm/film_grain_neon.h"
#include "src/dsp/common.h"
#include "src/dsp/dsp.h"
#include "src/dsp/film_grain_impl.h"
#include "src/utils/common.h"
#include "src/utils/compiler_attributes.h"
#include "src/utils/logging.h"

namespace libgav1 {
namespace dsp {
namespace film_grain {
namespace {

// These functions are overloaded for both possible sizes in order to simplify
// loading and storing to and from intermediate value types from within a
// template function.
inline int16x8_t GetSignedSource8(const int8_t* src) {
  return vmovl_s8(vld1_s8(src));
}

inline int16x8_t GetSignedSource8(const uint8_t* src) {
  return ZeroExtend(vld1_u8(src));
}

inline void StoreUnsigned8(uint8_t* dest, const uint16x8_t data) {
  vst1_u8(dest, vmovn_u16(data));
}

#if LIBGAV1_MAX_BITDEPTH >= 10
inline int16x8_t GetSignedSource8(const int16_t* src) { return vld1q_s16(src); }

inline int16x8_t GetSignedSource8(const uint16_t* src) {
  return vreinterpretq_s16_u16(vld1q_u16(src));
}

inline void StoreUnsigned8(uint16_t* dest, const uint16x8_t data) {
  vst1q_u16(dest, data);
}
#endif  // LIBGAV1_MAX_BITDEPTH >= 10

// Each element in |sum| represents one destination value's running
// autoregression formula. The fixed source values in |grain_lo| and |grain_hi|
// allow for a sliding window in successive calls to this function.
template <int position_offset>
inline int32x4x2_t AccumulateWeightedGrain(const int16x8_t grain_lo,
                                           const int16x8_t grain_hi,
                                           int16_t coeff, int32x4x2_t sum) {
  const int16x8_t grain = vextq_s16(grain_lo, grain_hi, position_offset);
  sum.val[0] = vmlal_n_s16(sum.val[0], vget_low_s16(grain), coeff);
  sum.val[1] = vmlal_n_s16(sum.val[1], vget_high_s16(grain), coeff);
  return sum;
}

// Because the autoregressive filter requires the output of each pixel to
// compute pixels that come after in the row, we have to finish the calculations
// one at a time.
template <int bitdepth, int auto_regression_coeff_lag, int lane>
inline void WriteFinalAutoRegression(int8_t* grain_cursor, int32x4x2_t sum,
                                     const int8_t* coeffs, int pos, int shift) {
  int32_t result = vgetq_lane_s32(sum.val[lane >> 2], lane & 3);

  for (int delta_col = -auto_regression_coeff_lag; delta_col < 0; ++delta_col) {
    result += grain_cursor[lane + delta_col] * coeffs[pos];
    ++pos;
  }
  grain_cursor[lane] =
      Clip3(grain_cursor[lane] + RightShiftWithRounding(result, shift),
            GetGrainMin<bitdepth>(), GetGrainMax<bitdepth>());
}

#if LIBGAV1_MAX_BITDEPTH >= 10
template <int bitdepth, int auto_regression_coeff_lag, int lane>
inline void WriteFinalAutoRegression(int16_t* grain_cursor, int32x4x2_t sum,
                                     const int8_t* coeffs, int pos, int shift) {
  int32_t result = vgetq_lane_s32(sum.val[lane >> 2], lane & 3);

  for (int delta_col = -auto_regression_coeff_lag; delta_col < 0; ++delta_col) {
    result += grain_cursor[lane + delta_col] * coeffs[pos];
    ++pos;
  }
  grain_cursor[lane] =
      Clip3(grain_cursor[lane] + RightShiftWithRounding(result, shift),
            GetGrainMin<bitdepth>(), GetGrainMax<bitdepth>());
}
#endif  // LIBGAV1_MAX_BITDEPTH >= 10

// Because the autoregressive filter requires the output of each pixel to
// compute pixels that come after in the row, we have to finish the calculations
// one at a time.
template <int bitdepth, int auto_regression_coeff_lag, int lane>
inline void WriteFinalAutoRegressionChroma(int8_t* u_grain_cursor,
                                           int8_t* v_grain_cursor,
                                           int32x4x2_t sum_u, int32x4x2_t sum_v,
                                           const int8_t* coeffs_u,
                                           const int8_t* coeffs_v, int pos,
                                           int shift) {
  WriteFinalAutoRegression<bitdepth, auto_regression_coeff_lag, lane>(
      u_grain_cursor, sum_u, coeffs_u, pos, shift);
  WriteFinalAutoRegression<bitdepth, auto_regression_coeff_lag, lane>(
      v_grain_cursor, sum_v, coeffs_v, pos, shift);
}

#if LIBGAV1_MAX_BITDEPTH >= 10
template <int bitdepth, int auto_regression_coeff_lag, int lane>
inline void WriteFinalAutoRegressionChroma(int16_t* u_grain_cursor,
                                           int16_t* v_grain_cursor,
                                           int32x4x2_t sum_u, int32x4x2_t sum_v,
                                           const int8_t* coeffs_u,
                                           const int8_t* coeffs_v, int pos,
                                           int shift) {
  WriteFinalAutoRegression<bitdepth, auto_regression_coeff_lag, lane>(
      u_grain_cursor, sum_u, coeffs_u, pos, shift);
  WriteFinalAutoRegression<bitdepth, auto_regression_coeff_lag, lane>(
      v_grain_cursor, sum_v, coeffs_v, pos, shift);
}
#endif  // LIBGAV1_MAX_BITDEPTH >= 10

inline void SetZero(int32x4x2_t* v) {
  v->val[0] = vdupq_n_s32(0);
  v->val[1] = vdupq_n_s32(0);
}

// Computes subsampled luma for use with chroma, by averaging in the x direction
// or y direction when applicable.
int16x8_t GetSubsampledLuma(const int8_t* const luma, int subsampling_x,
                            int subsampling_y, ptrdiff_t stride) {
  if (subsampling_y != 0) {
    assert(subsampling_x != 0);
    const int8x16_t src0 = vld1q_s8(luma);
    const int8x16_t src1 = vld1q_s8(luma + stride);
    const int16x8_t ret0 = vcombine_s16(vpaddl_s8(vget_low_s8(src0)),
                                        vpaddl_s8(vget_high_s8(src0)));
    const int16x8_t ret1 = vcombine_s16(vpaddl_s8(vget_low_s8(src1)),
                                        vpaddl_s8(vget_high_s8(src1)));
    return vrshrq_n_s16(vaddq_s16(ret0, ret1), 2);
  }
  if (subsampling_x != 0) {
    const int8x16_t src = vld1q_s8(luma);
    return vrshrq_n_s16(
        vcombine_s16(vpaddl_s8(vget_low_s8(src)), vpaddl_s8(vget_high_s8(src))),
        1);
  }
  return vmovl_s8(vld1_s8(luma));
}

#if LIBGAV1_MAX_BITDEPTH >= 10
// Computes subsampled luma for use with chroma, by averaging in the x direction
// or y direction when applicable.
int16x8_t GetSubsampledLuma(const int16_t* const luma, int subsampling_x,
                            int subsampling_y, ptrdiff_t stride) {
  if (subsampling_y != 0) {
    assert(subsampling_x != 0);
    int16x8_t src0_lo = vld1q_s16(luma);
    int16x8_t src0_hi = vld1q_s16(luma + 8);
    const int16x8_t src1_lo = vld1q_s16(luma + stride);
    const int16x8_t src1_hi = vld1q_s16(luma + stride + 8);
    const int16x8_t src0 =
        vcombine_s16(vpadd_s16(vget_low_s16(src0_lo), vget_high_s16(src0_lo)),
                     vpadd_s16(vget_low_s16(src0_hi), vget_high_s16(src0_hi)));
    const int16x8_t src1 =
        vcombine_s16(vpadd_s16(vget_low_s16(src1_lo), vget_high_s16(src1_lo)),
                     vpadd_s16(vget_low_s16(src1_hi), vget_high_s16(src1_hi)));
    return vrshrq_n_s16(vaddq_s16(src0, src1), 2);
  }
  if (subsampling_x != 0) {
    const int16x8_t src_lo = vld1q_s16(luma);
    const int16x8_t src_hi = vld1q_s16(luma + 8);
    const int16x8_t ret =
        vcombine_s16(vpadd_s16(vget_low_s16(src_lo), vget_high_s16(src_lo)),
                     vpadd_s16(vget_low_s16(src_hi), vget_high_s16(src_hi)));
    return vrshrq_n_s16(ret, 1);
  }
  return vld1q_s16(luma);
}
#endif  // LIBGAV1_MAX_BITDEPTH >= 10

template <int bitdepth, typename GrainType, int auto_regression_coeff_lag,
          bool use_luma>
void ApplyAutoRegressiveFilterToChromaGrains_NEON(const FilmGrainParams& params,
                                                  const void* luma_grain_buffer,
                                                  int subsampling_x,
                                                  int subsampling_y,
                                                  void* u_grain_buffer,
                                                  void* v_grain_buffer) {
  static_assert(auto_regression_coeff_lag <= 3, "Invalid autoregression lag.");
  const auto* luma_grain = static_cast<const GrainType*>(luma_grain_buffer);
  auto* u_grain = static_cast<GrainType*>(u_grain_buffer);
  auto* v_grain = static_cast<GrainType*>(v_grain_buffer);
  const int auto_regression_shift = params.auto_regression_shift;
  const int chroma_width =
      (subsampling_x == 0) ? kMaxChromaWidth : kMinChromaWidth;
  const int chroma_height =
      (subsampling_y == 0) ? kMaxChromaHeight : kMinChromaHeight;
  // When |chroma_width| == 44, we write 8 at a time from x in [3, 34],
  // leaving [35, 40] to write at the end.
  const int chroma_width_remainder =
      (chroma_width - 2 * kAutoRegressionBorder) & 7;

  int y = kAutoRegressionBorder;
  luma_grain += kLumaWidth * y;
  u_grain += chroma_width * y;
  v_grain += chroma_width * y;
  do {
    // Each row is computed 8 values at a time in the following loop. At the
    // end of the loop, 4 values remain to write. They are given a special
    // reduced iteration at the end.
    int x = kAutoRegressionBorder;
    int luma_x = kAutoRegressionBorder;
    do {
      int pos = 0;
      int32x4x2_t sum_u;
      int32x4x2_t sum_v;
      SetZero(&sum_u);
      SetZero(&sum_v);

      if (auto_regression_coeff_lag > 0) {
        for (int delta_row = -auto_regression_coeff_lag; delta_row < 0;
             ++delta_row) {
          // These loads may overflow to the next row, but they are never called
          // on the final row of a grain block. Therefore, they will never
          // exceed the block boundaries.
          // Note: this could be slightly optimized to a single load in 8bpp,
          // but requires making a special first iteration and accumulate
          // function that takes an int8x16_t.
          const int16x8_t u_grain_lo =
              GetSignedSource8(u_grain + x + delta_row * chroma_width -
                               auto_regression_coeff_lag);
          const int16x8_t u_grain_hi =
              GetSignedSource8(u_grain + x + delta_row * chroma_width -
                               auto_regression_coeff_lag + 8);
          const int16x8_t v_grain_lo =
              GetSignedSource8(v_grain + x + delta_row * chroma_width -
                               auto_regression_coeff_lag);
          const int16x8_t v_grain_hi =
              GetSignedSource8(v_grain + x + delta_row * chroma_width -
                               auto_regression_coeff_lag + 8);
#define ACCUMULATE_WEIGHTED_GRAIN(offset)                                  \
  sum_u = AccumulateWeightedGrain<offset>(                                 \
      u_grain_lo, u_grain_hi, params.auto_regression_coeff_u[pos], sum_u); \
  sum_v = AccumulateWeightedGrain<offset>(                                 \
      v_grain_lo, v_grain_hi, params.auto_regression_coeff_v[pos++], sum_v)

          ACCUMULATE_WEIGHTED_GRAIN(0);
          ACCUMULATE_WEIGHTED_GRAIN(1);
          ACCUMULATE_WEIGHTED_GRAIN(2);
          // The horizontal |auto_regression_coeff_lag| loop is replaced with
          // if-statements to give vextq_s16 an immediate param.
          if (auto_regression_coeff_lag > 1) {
            ACCUMULATE_WEIGHTED_GRAIN(3);
            ACCUMULATE_WEIGHTED_GRAIN(4);
          }
          if (auto_regression_coeff_lag > 2) {
            assert(auto_regression_coeff_lag == 3);
            ACCUMULATE_WEIGHTED_GRAIN(5);
            ACCUMULATE_WEIGHTED_GRAIN(6);
          }
        }
      }

      if (use_luma) {
        const int16x8_t luma = GetSubsampledLuma(
            luma_grain + luma_x, subsampling_x, subsampling_y, kLumaWidth);

        // Luma samples get the final coefficient in the formula, but are best
        // computed all at once before the final row.
        const int coeff_u =
            params.auto_regression_coeff_u[pos + auto_regression_coeff_lag];
        const int coeff_v =
            params.auto_regression_coeff_v[pos + auto_regression_coeff_lag];

        sum_u.val[0] = vmlal_n_s16(sum_u.val[0], vget_low_s16(luma), coeff_u);
        sum_u.val[1] = vmlal_n_s16(sum_u.val[1], vget_high_s16(luma), coeff_u);
        sum_v.val[0] = vmlal_n_s16(sum_v.val[0], vget_low_s16(luma), coeff_v);
        sum_v.val[1] = vmlal_n_s16(sum_v.val[1], vget_high_s16(luma), coeff_v);
      }
      // At this point in the filter, the source addresses and destination
      // addresses overlap. Because this is an auto-regressive filter, the
      // higher lanes cannot be computed without the results of the lower lanes.
      // Each call to WriteFinalAutoRegression incorporates preceding values
      // on the final row, and writes a single sample. This allows the next
      // pixel's value to be computed in the next call.
#define WRITE_AUTO_REGRESSION_RESULT(lane)                                    \
  WriteFinalAutoRegressionChroma<bitdepth, auto_regression_coeff_lag, lane>(  \
      u_grain + x, v_grain + x, sum_u, sum_v, params.auto_regression_coeff_u, \
      params.auto_regression_coeff_v, pos, auto_regression_shift)

      WRITE_AUTO_REGRESSION_RESULT(0);
      WRITE_AUTO_REGRESSION_RESULT(1);
      WRITE_AUTO_REGRESSION_RESULT(2);
      WRITE_AUTO_REGRESSION_RESULT(3);
      WRITE_AUTO_REGRESSION_RESULT(4);
      WRITE_AUTO_REGRESSION_RESULT(5);
      WRITE_AUTO_REGRESSION_RESULT(6);
      WRITE_AUTO_REGRESSION_RESULT(7);

      x += 8;
      luma_x += 8 << subsampling_x;
    } while (x < chroma_width - kAutoRegressionBorder - chroma_width_remainder);

    // This is the "final iteration" of the above loop over width. We fill in
    // the remainder of the width, which is less than 8.
    int pos = 0;
    int32x4x2_t sum_u;
    int32x4x2_t sum_v;
    SetZero(&sum_u);
    SetZero(&sum_v);

    for (int delta_row = -auto_regression_coeff_lag; delta_row < 0;
         ++delta_row) {
      // These loads may overflow to the next row, but they are never called on
      // the final row of a grain block. Therefore, they will never exceed the
      // block boundaries.
      const int16x8_t u_grain_lo = GetSignedSource8(
          u_grain + x + delta_row * chroma_width - auto_regression_coeff_lag);
      const int16x8_t u_grain_hi =
          GetSignedSource8(u_grain + x + delta_row * chroma_width -
                           auto_regression_coeff_lag + 8);
      const int16x8_t v_grain_lo = GetSignedSource8(
          v_grain + x + delta_row * chroma_width - auto_regression_coeff_lag);
      const int16x8_t v_grain_hi =
          GetSignedSource8(v_grain + x + delta_row * chroma_width -
                           auto_regression_coeff_lag + 8);

      ACCUMULATE_WEIGHTED_GRAIN(0);
      ACCUMULATE_WEIGHTED_GRAIN(1);
      ACCUMULATE_WEIGHTED_GRAIN(2);
      // The horizontal |auto_regression_coeff_lag| loop is replaced with
      // if-statements to give vextq_s16 an immediate param.
      if (auto_regression_coeff_lag > 1) {
        ACCUMULATE_WEIGHTED_GRAIN(3);
        ACCUMULATE_WEIGHTED_GRAIN(4);
      }
      if (auto_regression_coeff_lag > 2) {
        assert(auto_regression_coeff_lag == 3);
        ACCUMULATE_WEIGHTED_GRAIN(5);
        ACCUMULATE_WEIGHTED_GRAIN(6);
      }
    }

    if (use_luma) {
      const int16x8_t luma = GetSubsampledLuma(
          luma_grain + luma_x, subsampling_x, subsampling_y, kLumaWidth);

      // Luma samples get the final coefficient in the formula, but are best
      // computed all at once before the final row.
      const int coeff_u =
          params.auto_regression_coeff_u[pos + auto_regression_coeff_lag];
      const int coeff_v =
          params.auto_regression_coeff_v[pos + auto_regression_coeff_lag];

      sum_u.val[0] = vmlal_n_s16(sum_u.val[0], vget_low_s16(luma), coeff_u);
      sum_u.val[1] = vmlal_n_s16(sum_u.val[1], vget_high_s16(luma), coeff_u);
      sum_v.val[0] = vmlal_n_s16(sum_v.val[0], vget_low_s16(luma), coeff_v);
      sum_v.val[1] = vmlal_n_s16(sum_v.val[1], vget_high_s16(luma), coeff_v);
    }

    WRITE_AUTO_REGRESSION_RESULT(0);
    WRITE_AUTO_REGRESSION_RESULT(1);
    WRITE_AUTO_REGRESSION_RESULT(2);
    WRITE_AUTO_REGRESSION_RESULT(3);
    if (chroma_width_remainder == 6) {
      WRITE_AUTO_REGRESSION_RESULT(4);
      WRITE_AUTO_REGRESSION_RESULT(5);
    }

    luma_grain += kLumaWidth << subsampling_y;
    u_grain += chroma_width;
    v_grain += chroma_width;
  } while (++y < chroma_height);
#undef ACCUMULATE_WEIGHTED_GRAIN
#undef WRITE_AUTO_REGRESSION_RESULT
}

// Applies an auto-regressive filter to the white noise in luma_grain.
template <int bitdepth, typename GrainType, int auto_regression_coeff_lag>
void ApplyAutoRegressiveFilterToLumaGrain_NEON(const FilmGrainParams& params,
                                               void* luma_grain_buffer) {
  static_assert(auto_regression_coeff_lag > 0, "");
  const int8_t* const auto_regression_coeff_y = params.auto_regression_coeff_y;
  const uint8_t auto_regression_shift = params.auto_regression_shift;

  int y = kAutoRegressionBorder;
  auto* luma_grain =
      static_cast<GrainType*>(luma_grain_buffer) + kLumaWidth * y;
  do {
    // Each row is computed 8 values at a time in the following loop. At the
    // end of the loop, 4 values remain to write. They are given a special
    // reduced iteration at the end.
    int x = kAutoRegressionBorder;
    do {
      int pos = 0;
      int32x4x2_t sum;
      SetZero(&sum);
      for (int delta_row = -auto_regression_coeff_lag; delta_row < 0;
           ++delta_row) {
        // These loads may overflow to the next row, but they are never called
        // on the final row of a grain block. Therefore, they will never exceed
        // the block boundaries.
        const int16x8_t src_grain_lo =
            GetSignedSource8(luma_grain + x + delta_row * kLumaWidth -
                             auto_regression_coeff_lag);
        const int16x8_t src_grain_hi =
            GetSignedSource8(luma_grain + x + delta_row * kLumaWidth -
                             auto_regression_coeff_lag + 8);

        // A pictorial representation of the auto-regressive filter for
        // various values of params.auto_regression_coeff_lag. The letter 'O'
        // represents the current sample. (The filter always operates on the
        // current sample with filter coefficient 1.) The letters 'X'
        // represent the neighboring samples that the filter operates on, below
        // their corresponding "offset" number.
        //
        // params.auto_regression_coeff_lag == 3:
        //   0 1 2 3 4 5 6
        //   X X X X X X X
        //   X X X X X X X
        //   X X X X X X X
        //   X X X O
        // params.auto_regression_coeff_lag == 2:
        //     0 1 2 3 4
        //     X X X X X
        //     X X X X X
        //     X X O
        // params.auto_regression_coeff_lag == 1:
        //       0 1 2
        //       X X X
        //       X O
        // params.auto_regression_coeff_lag == 0:
        //         O
        // The function relies on the caller to skip the call in the 0 lag
        // case.

#define ACCUMULATE_WEIGHTED_GRAIN(offset)                           \
  sum = AccumulateWeightedGrain<offset>(src_grain_lo, src_grain_hi, \
                                        auto_regression_coeff_y[pos++], sum)
        ACCUMULATE_WEIGHTED_GRAIN(0);
        ACCUMULATE_WEIGHTED_GRAIN(1);
        ACCUMULATE_WEIGHTED_GRAIN(2);
        // The horizontal |auto_regression_coeff_lag| loop is replaced with
        // if-statements to give vextq_s16 an immediate param.
        if (auto_regression_coeff_lag > 1) {
          ACCUMULATE_WEIGHTED_GRAIN(3);
          ACCUMULATE_WEIGHTED_GRAIN(4);
        }
        if (auto_regression_coeff_lag > 2) {
          assert(auto_regression_coeff_lag == 3);
          ACCUMULATE_WEIGHTED_GRAIN(5);
          ACCUMULATE_WEIGHTED_GRAIN(6);
        }
      }
      // At this point in the filter, the source addresses and destination
      // addresses overlap. Because this is an auto-regressive filter, the
      // higher lanes cannot be computed without the results of the lower lanes.
      // Each call to WriteFinalAutoRegression incorporates preceding values
      // on the final row, and writes a single sample. This allows the next
      // pixel's value to be computed in the next call.
#define WRITE_AUTO_REGRESSION_RESULT(lane)                             \
  WriteFinalAutoRegression<bitdepth, auto_regression_coeff_lag, lane>( \
      luma_grain + x, sum, auto_regression_coeff_y, pos,               \
      auto_regression_shift)

      WRITE_AUTO_REGRESSION_RESULT(0);
      WRITE_AUTO_REGRESSION_RESULT(1);
      WRITE_AUTO_REGRESSION_RESULT(2);
      WRITE_AUTO_REGRESSION_RESULT(3);
      WRITE_AUTO_REGRESSION_RESULT(4);
      WRITE_AUTO_REGRESSION_RESULT(5);
      WRITE_AUTO_REGRESSION_RESULT(6);
      WRITE_AUTO_REGRESSION_RESULT(7);
      x += 8;
      // Leave the final four pixels for the special iteration below.
    } while (x < kLumaWidth - kAutoRegressionBorder - 4);

    // Final 4 pixels in the row.
    int pos = 0;
    int32x4x2_t sum;
    SetZero(&sum);
    for (int delta_row = -auto_regression_coeff_lag; delta_row < 0;
         ++delta_row) {
      const int16x8_t src_grain_lo = GetSignedSource8(
          luma_grain + x + delta_row * kLumaWidth - auto_regression_coeff_lag);
      const int16x8_t src_grain_hi =
          GetSignedSource8(luma_grain + x + delta_row * kLumaWidth -
                           auto_regression_coeff_lag + 8);

      ACCUMULATE_WEIGHTED_GRAIN(0);
      ACCUMULATE_WEIGHTED_GRAIN(1);
      ACCUMULATE_WEIGHTED_GRAIN(2);
      // The horizontal |auto_regression_coeff_lag| loop is replaced with
      // if-statements to give vextq_s16 an immediate param.
      if (auto_regression_coeff_lag > 1) {
        ACCUMULATE_WEIGHTED_GRAIN(3);
        ACCUMULATE_WEIGHTED_GRAIN(4);
      }
      if (auto_regression_coeff_lag > 2) {
        assert(auto_regression_coeff_lag == 3);
        ACCUMULATE_WEIGHTED_GRAIN(5);
        ACCUMULATE_WEIGHTED_GRAIN(6);
      }
    }
    // delta_row == 0
    WRITE_AUTO_REGRESSION_RESULT(0);
    WRITE_AUTO_REGRESSION_RESULT(1);
    WRITE_AUTO_REGRESSION_RESULT(2);
    WRITE_AUTO_REGRESSION_RESULT(3);
    luma_grain += kLumaWidth;
  } while (++y < kLumaHeight);

#undef WRITE_AUTO_REGRESSION_RESULT
#undef ACCUMULATE_WEIGHTED_GRAIN
}

void InitializeScalingLookupTable_NEON(
    int num_points, const uint8_t point_value[], const uint8_t point_scaling[],
    uint8_t scaling_lut[kScalingLookupTableSize]) {
  if (num_points == 0) {
    memset(scaling_lut, 0, sizeof(scaling_lut[0]) * kScalingLookupTableSize);
    return;
  }
  static_assert(sizeof(scaling_lut[0]) == 1, "");
  memset(scaling_lut, point_scaling[0], point_value[0]);
  const uint32x4_t steps = vmovl_u16(vcreate_u16(0x0003000200010000));
  const uint32x4_t offset = vdupq_n_u32(32768);
  for (int i = 0; i < num_points - 1; ++i) {
    const int delta_y = point_scaling[i + 1] - point_scaling[i];
    const int delta_x = point_value[i + 1] - point_value[i];
    const int delta = delta_y * ((65536 + (delta_x >> 1)) / delta_x);
    const int delta4 = delta << 2;
    const uint8x8_t base_point = vdup_n_u8(point_scaling[i]);
    uint32x4_t upscaled_points0 = vmlaq_n_u32(offset, steps, delta);
    const uint32x4_t line_increment4 = vdupq_n_u32(delta4);
    // Get the second set of 4 points by adding 4 steps to the first set.
    uint32x4_t upscaled_points1 = vaddq_u32(upscaled_points0, line_increment4);
    // We obtain the next set of 8 points by adding 8 steps to each of the
    // current 8 points.
    const uint32x4_t line_increment8 = vshlq_n_u32(line_increment4, 1);
    int x = 0;
    do {
      const uint16x4_t interp_points0 = vshrn_n_u32(upscaled_points0, 16);
      const uint16x4_t interp_points1 = vshrn_n_u32(upscaled_points1, 16);
      const uint8x8_t interp_points =
          vmovn_u16(vcombine_u16(interp_points0, interp_points1));
      // The spec guarantees that the max value of |point_value[i]| + x is 255.
      // Writing 8 bytes starting at the final table byte, leaves 7 bytes of
      // required padding.
      vst1_u8(&scaling_lut[point_value[i] + x],
              vadd_u8(interp_points, base_point));
      upscaled_points0 = vaddq_u32(upscaled_points0, line_increment8);
      upscaled_points1 = vaddq_u32(upscaled_points1, line_increment8);
      x += 8;
    } while (x < delta_x);
  }
  const uint8_t last_point_value = point_value[num_points - 1];
  memset(&scaling_lut[last_point_value], point_scaling[num_points - 1],
         kScalingLookupTableSize - last_point_value);
}

inline int16x8_t Clip3(const int16x8_t value, const int16x8_t low,
                       const int16x8_t high) {
  const int16x8_t clipped_to_ceiling = vminq_s16(high, value);
  return vmaxq_s16(low, clipped_to_ceiling);
}

template <int bitdepth, typename Pixel>
inline int16x8_t GetScalingFactors(
    const uint8_t scaling_lut[kScalingLookupTableSize], const Pixel* source) {
  int16_t start_vals[8];
  if (bitdepth == 8) {
    start_vals[0] = scaling_lut[source[0]];
    start_vals[1] = scaling_lut[source[1]];
    start_vals[2] = scaling_lut[source[2]];
    start_vals[3] = scaling_lut[source[3]];
    start_vals[4] = scaling_lut[source[4]];
    start_vals[5] = scaling_lut[source[5]];
    start_vals[6] = scaling_lut[source[6]];
    start_vals[7] = scaling_lut[source[7]];
    return vld1q_s16(start_vals);
  }
  int16_t end_vals[8];
  int index = source[0] >> 2;
  start_vals[0] = scaling_lut[index];
  end_vals[0] = scaling_lut[index + 1];
  index = source[1] >> 2;
  start_vals[1] = scaling_lut[index];
  end_vals[1] = scaling_lut[index + 1];
  index = source[2] >> 2;
  start_vals[2] = scaling_lut[index];
  end_vals[2] = scaling_lut[index + 1];
  index = source[3] >> 2;
  start_vals[3] = scaling_lut[index];
  end_vals[3] = scaling_lut[index + 1];
  index = source[4] >> 2;
  start_vals[4] = scaling_lut[index];
  end_vals[4] = scaling_lut[index + 1];
  index = source[5] >> 2;
  start_vals[5] = scaling_lut[index];
  end_vals[5] = scaling_lut[index + 1];
  index = source[6] >> 2;
  start_vals[6] = scaling_lut[index];
  end_vals[6] = scaling_lut[index + 1];
  index = source[7] >> 2;
  start_vals[7] = scaling_lut[index];
  end_vals[7] = scaling_lut[index + 1];
  const int16x8_t start = vld1q_s16(start_vals);
  const int16x8_t end = vld1q_s16(end_vals);
  int16x8_t remainder = GetSignedSource8(source);
  remainder = vandq_s16(remainder, vdupq_n_s16(3));
  const int16x8_t delta = vmulq_s16(vsubq_s16(end, start), remainder);
  return vaddq_s16(start, vrshrq_n_s16(delta, 2));
}

inline int16x8_t ScaleNoise(const int16x8_t noise, const int16x8_t scaling,
                            const int16x8_t scaling_shift_vect) {
  const int16x8_t upscaled_noise = vmulq_s16(noise, scaling);
  return vrshlq_s16(upscaled_noise, scaling_shift_vect);
}

#if LIBGAV1_MAX_BITDEPTH >= 10
inline int16x8_t ScaleNoise(const int16x8_t noise, const int16x8_t scaling,
                            const int32x4_t scaling_shift_vect) {
  // TODO(petersonab): Try refactoring scaling lookup table to int16_t and
  // upscaling by 7 bits to permit high half multiply. This would eliminate
  // the intermediate 32x4 registers. Also write the averaged values directly
  // into the table so it doesn't have to be done for every pixel in
  // the frame.
  const int32x4_t upscaled_noise_lo =
      vmull_s16(vget_low_s16(noise), vget_low_s16(scaling));
  const int32x4_t upscaled_noise_hi =
      vmull_s16(vget_high_s16(noise), vget_high_s16(scaling));
  const int16x4_t noise_lo =
      vmovn_s32(vrshlq_s32(upscaled_noise_lo, scaling_shift_vect));
  const int16x4_t noise_hi =
      vmovn_s32(vrshlq_s32(upscaled_noise_hi, scaling_shift_vect));
  return vcombine_s16(noise_lo, noise_hi);
}
#endif  // LIBGAV1_MAX_BITDEPTH >= 10

template <int bitdepth, typename GrainType, typename Pixel>
void BlendNoiseWithImageLuma_NEON(const void* noise_image_ptr, int min_value,
                                  int max_luma, int scaling_shift, int width,
                                  int height, const uint8_t scaling_lut_y[256],
                                  const void* source_plane_y,
                                  ptrdiff_t source_stride_y, void* dest_plane_y,
                                  ptrdiff_t dest_stride_y) {
  const auto* noise_image =
      static_cast<const Array2D<GrainType>*>(noise_image_ptr);
  const auto* in_y_row = static_cast<const Pixel*>(source_plane_y);
  source_stride_y /= sizeof(Pixel);
  auto* out_y_row = static_cast<Pixel*>(dest_plane_y);
  dest_stride_y /= sizeof(Pixel);
  const int16x8_t floor = vdupq_n_s16(min_value);
  const int16x8_t ceiling = vdupq_n_s16(max_luma);
  // In 8bpp, the maximum upscaled noise is 127*255 = 0x7E81, which is safe
  // for 16 bit signed integers. In higher bitdepths, however, we have to
  // expand to 32 to protect the sign bit.
  const int16x8_t scaling_shift_vect16 = vdupq_n_s16(-scaling_shift);
#if LIBGAV1_MAX_BITDEPTH >= 10
  const int32x4_t scaling_shift_vect32 = vdupq_n_s32(-scaling_shift);
#endif  // LIBGAV1_MAX_BITDEPTH >= 10

  int y = 0;
  do {
    int x = 0;
    do {
      // This operation on the unsigned input is safe in 8bpp because the vector
      // is widened before it is reinterpreted.
      const int16x8_t orig = GetSignedSource8(&in_y_row[x]);
      const int16x8_t scaling =
          GetScalingFactors<bitdepth, Pixel>(scaling_lut_y, &in_y_row[x]);
      int16x8_t noise = GetSignedSource8(&(noise_image[kPlaneY][y][x]));

      if (bitdepth == 8) {
        noise = ScaleNoise(noise, scaling, scaling_shift_vect16);
      } else {
#if LIBGAV1_MAX_BITDEPTH >= 10
        noise = ScaleNoise(noise, scaling, scaling_shift_vect32);
#endif  // LIBGAV1_MAX_BITDEPTH >= 10
      }
      const int16x8_t combined = vaddq_s16(orig, noise);
      // In 8bpp, when params_.clip_to_restricted_range == false, we can replace
      // clipping with vqmovun_s16.
      StoreUnsigned8(&out_y_row[x],
                     vreinterpretq_u16_s16(Clip3(combined, floor, ceiling)));
      x += 8;
    } while (x < width);
    in_y_row += source_stride_y;
    out_y_row += dest_stride_y;
  } while (++y < height);
}

void Init8bpp() {
  Dsp* const dsp = dsp_internal::GetWritableDspTable(8);
  assert(dsp != nullptr);

  // LumaAutoRegressionFunc[auto_regression_coeff_lag]
  // Luma autoregression should never be called when lag is 0.
  dsp->film_grain.luma_auto_regression[0] = nullptr;
  dsp->film_grain.luma_auto_regression[1] =
      ApplyAutoRegressiveFilterToLumaGrain_NEON<8, int8_t, 1>;
  dsp->film_grain.luma_auto_regression[2] =
      ApplyAutoRegressiveFilterToLumaGrain_NEON<8, int8_t, 2>;
  dsp->film_grain.luma_auto_regression[3] =
      ApplyAutoRegressiveFilterToLumaGrain_NEON<8, int8_t, 3>;

  // ChromaAutoRegressionFunc[use_luma][auto_regression_coeff_lag]
  // Chroma autoregression should never be called when lag is 0 and use_luma is
  // false.
  dsp->film_grain.chroma_auto_regression[0][0] = nullptr;
  dsp->film_grain.chroma_auto_regression[0][1] =
      ApplyAutoRegressiveFilterToChromaGrains_NEON<8, int8_t, 1, false>;
  dsp->film_grain.chroma_auto_regression[0][2] =
      ApplyAutoRegressiveFilterToChromaGrains_NEON<8, int8_t, 2, false>;
  dsp->film_grain.chroma_auto_regression[0][3] =
      ApplyAutoRegressiveFilterToChromaGrains_NEON<8, int8_t, 3, false>;
  dsp->film_grain.chroma_auto_regression[1][0] =
      ApplyAutoRegressiveFilterToChromaGrains_NEON<8, int8_t, 0, true>;
  dsp->film_grain.chroma_auto_regression[1][1] =
      ApplyAutoRegressiveFilterToChromaGrains_NEON<8, int8_t, 1, true>;
  dsp->film_grain.chroma_auto_regression[1][2] =
      ApplyAutoRegressiveFilterToChromaGrains_NEON<8, int8_t, 2, true>;
  dsp->film_grain.chroma_auto_regression[1][3] =
      ApplyAutoRegressiveFilterToChromaGrains_NEON<8, int8_t, 3, true>;

  dsp->film_grain.initialize_scaling_lut = InitializeScalingLookupTable_NEON;

  dsp->film_grain.blend_noise_luma =
      BlendNoiseWithImageLuma_NEON<8, int8_t, uint8_t>;
}

#if LIBGAV1_MAX_BITDEPTH >= 10
void Init10bpp() {
  Dsp* const dsp = dsp_internal::GetWritableDspTable(10);
  assert(dsp != nullptr);

  // LumaAutoRegressionFunc[auto_regression_coeff_lag]
  // Luma autoregression should never be called when lag is 0.
  dsp->film_grain.luma_auto_regression[0] = nullptr;
  dsp->film_grain.luma_auto_regression[1] =
      ApplyAutoRegressiveFilterToLumaGrain_NEON<10, int16_t, 1>;
  dsp->film_grain.luma_auto_regression[2] =
      ApplyAutoRegressiveFilterToLumaGrain_NEON<10, int16_t, 2>;
  dsp->film_grain.luma_auto_regression[3] =
      ApplyAutoRegressiveFilterToLumaGrain_NEON<10, int16_t, 3>;

  // ChromaAutoRegressionFunc[use_luma][auto_regression_coeff_lag][subsampling]
  // Chroma autoregression should never be called when lag is 0 and use_luma is
  // false.
  dsp->film_grain.chroma_auto_regression[0][0] = nullptr;
  dsp->film_grain.chroma_auto_regression[0][1] =
      ApplyAutoRegressiveFilterToChromaGrains_NEON<10, int16_t, 1, false>;
  dsp->film_grain.chroma_auto_regression[0][2] =
      ApplyAutoRegressiveFilterToChromaGrains_NEON<10, int16_t, 2, false>;
  dsp->film_grain.chroma_auto_regression[0][3] =
      ApplyAutoRegressiveFilterToChromaGrains_NEON<10, int16_t, 3, false>;
  dsp->film_grain.chroma_auto_regression[1][0] =
      ApplyAutoRegressiveFilterToChromaGrains_NEON<10, int16_t, 0, true>;
  dsp->film_grain.chroma_auto_regression[1][1] =
      ApplyAutoRegressiveFilterToChromaGrains_NEON<10, int16_t, 1, true>;
  dsp->film_grain.chroma_auto_regression[1][2] =
      ApplyAutoRegressiveFilterToChromaGrains_NEON<10, int16_t, 2, true>;
  dsp->film_grain.chroma_auto_regression[1][3] =
      ApplyAutoRegressiveFilterToChromaGrains_NEON<10, int16_t, 3, true>;

  dsp->film_grain.initialize_scaling_lut = InitializeScalingLookupTable_NEON;

  dsp->film_grain.blend_noise_luma =
      BlendNoiseWithImageLuma_NEON<10, int16_t, uint16_t>;
}
#endif  // LIBGAV1_MAX_BITDEPTH >= 10

}  // namespace
}  // namespace film_grain

void FilmGrainInit_NEON() {
  film_grain::Init8bpp();
#if LIBGAV1_MAX_BITDEPTH >= 10
  film_grain::Init10bpp();
#endif  // LIBGAV1_MAX_BITDEPTH >= 10
}

}  // namespace dsp
}  // namespace libgav1

#else   // !LIBGAV1_ENABLE_NEON

namespace libgav1 {
namespace dsp {

void FilmGrainInit_NEON() {}

}  // namespace dsp
}  // namespace libgav1
#endif  // LIBGAV1_ENABLE_NEON
