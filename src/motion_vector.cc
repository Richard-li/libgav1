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

#include "src/motion_vector.h"

#include <algorithm>
#include <cassert>
#include <cstdint>
#include <cstdlib>
#include <memory>

#include "src/dsp/dsp.h"
#include "src/utils/bit_mask_set.h"
#include "src/utils/common.h"
#include "src/utils/constants.h"
#include "src/utils/logging.h"

namespace libgav1 {
namespace {

// Entry at index i is computed as:
// Clip3(std::max(kBlockWidthPixels[i], kBlockHeightPixels[i], 16, 112)).
constexpr int kWarpValidThreshold[kMaxBlockSizes] = {
    16, 16, 16, 16, 16, 16, 32, 16, 16,  16,  32,
    64, 32, 32, 32, 64, 64, 64, 64, 112, 112, 112};

// 7.10.2.10.
void LowerMvPrecision(const ObuFrameHeader& frame_header,
                      MotionVector* const mvs) {
  if (frame_header.allow_high_precision_mv) return;
  if (frame_header.force_integer_mv != 0) {
    for (auto& mv : mvs->mv) {
      const int value = (std::abs(static_cast<int>(mv)) + 3) & ~7;
      const int sign = mv >> 15;
      mv = ApplySign(value, sign);
    }
  } else {
    for (auto& mv : mvs->mv) {
      if ((mv & 1) != 0) {
        // The next line is equivalent to:
        // if (mv > 0) { --mv; } else { ++mv; }
        mv -= (mv >> 15) | 1;
      }
    }
  }
}

// 7.9.3.
void GetMvProjection(const MotionVector& mv, int numerator, int denominator,
                     MotionVector* const projection_mv) {
  assert(denominator > 0);
  assert(denominator <= kMaxFrameDistance);
  numerator = Clip3(numerator, -kMaxFrameDistance, kMaxFrameDistance);
  for (int i = 0; i < 2; ++i) {
    projection_mv->mv[i] = Clip3(
        RightShiftWithRoundingSigned(
            mv.mv[i] * numerator * kProjectionMvDivisionLookup[denominator],
            14),
        -kProjectionMvClamp, kProjectionMvClamp);
  }
}

// 7.10.2.1.
void SetupGlobalMv(const Tile::Block& block, int index,
                   MotionVector* const mv) {
  const BlockParameters& bp = *block.bp;
  const ObuFrameHeader& frame_header = block.tile.frame_header();
  ReferenceFrameType reference_type = bp.reference_frame[index];
  const auto& gm = frame_header.global_motion[reference_type];
  GlobalMotionTransformationType global_motion_type =
      (reference_type != kReferenceFrameIntra)
          ? gm.type
          : kNumGlobalMotionTransformationTypes;
  if (reference_type == kReferenceFrameIntra ||
      global_motion_type == kGlobalMotionTransformationTypeIdentity) {
    mv->mv32 = 0;
    return;
  }
  if (global_motion_type == kGlobalMotionTransformationTypeTranslation) {
    for (int i = 0; i < 2; ++i) {
      mv->mv[i] = gm.params[i] >> (kWarpedModelPrecisionBits - 3);
    }
    LowerMvPrecision(frame_header, mv);
    return;
  }
  const int x = MultiplyBy4(block.column4x4) + DivideBy2(block.width) - 1;
  const int y = MultiplyBy4(block.row4x4) + DivideBy2(block.height) - 1;
  const int xc = (gm.params[2] - (1 << kWarpedModelPrecisionBits)) * x +
                 gm.params[3] * y + gm.params[0];
  const int yc = gm.params[4] * x +
                 (gm.params[5] - (1 << kWarpedModelPrecisionBits)) * y +
                 gm.params[1];
  if (frame_header.allow_high_precision_mv) {
    mv->mv[MotionVector::kRow] =
        RightShiftWithRoundingSigned(yc, kWarpedModelPrecisionBits - 3);
    mv->mv[MotionVector::kColumn] =
        RightShiftWithRoundingSigned(xc, kWarpedModelPrecisionBits - 3);
  } else {
    mv->mv[MotionVector::kRow] = MultiplyBy2(
        RightShiftWithRoundingSigned(yc, kWarpedModelPrecisionBits - 2));
    mv->mv[MotionVector::kColumn] = MultiplyBy2(
        RightShiftWithRoundingSigned(xc, kWarpedModelPrecisionBits - 2));
    LowerMvPrecision(frame_header, mv);
  }
}

constexpr BitMaskSet kPredictionModeNewMvMask(kPredictionModeNewMv,
                                              kPredictionModeNewNewMv,
                                              kPredictionModeNearNewMv,
                                              kPredictionModeNewNearMv,
                                              kPredictionModeNearestNewMv,
                                              kPredictionModeNewNearestMv);

// 7.10.2.8.
void SearchStack(const Tile::Block& block, const BlockParameters& mv_bp,
                 int index, int weight, bool* const found_new_mv,
                 bool* const found_match, int* const num_mv_found) {
  const BlockParameters& bp = *block.bp;
  const std::array<GlobalMotion, kNumReferenceFrameTypes>& global_motion =
      block.tile.frame_header().global_motion;
  PredictionParameters& prediction_parameters = *bp.prediction_parameters;
  MotionVector candidate_mv;
  // LowerMvPrecision() is not necessary, since the values in
  // |prediction_parameters.global_mv| and |mv_bp.mv| were generated by it.
  const auto global_motion_type = global_motion[bp.reference_frame[0]].type;
  if (IsGlobalMvBlock(mv_bp.is_global_mv_block, global_motion_type)) {
    candidate_mv = prediction_parameters.global_mv[0];
  } else {
    candidate_mv = mv_bp.mv.mv[index];
  }
  *found_new_mv |= kPredictionModeNewMvMask.Contains(mv_bp.y_mode);
  *found_match = true;
  MotionVector* const ref_mv_stack = prediction_parameters.ref_mv_stack;
  auto result = std::find_if(ref_mv_stack, ref_mv_stack + *num_mv_found,
                             [&candidate_mv](const MotionVector& ref_mv) {
                               return ref_mv == candidate_mv;
                             });
  if (result != ref_mv_stack + *num_mv_found) {
    prediction_parameters.IncreaseWeight(std::distance(ref_mv_stack, result),
                                         weight);
    return;
  }
  if (*num_mv_found >= kMaxRefMvStackSize) return;
  ref_mv_stack[*num_mv_found] = candidate_mv;
  prediction_parameters.SetWeightIndexStackEntry(*num_mv_found, weight);
  ++*num_mv_found;
}

// 7.10.2.9.
void CompoundSearchStack(const Tile::Block& block, const BlockParameters& mv_bp,
                         int weight, bool* const found_new_mv,
                         bool* const found_match, int* const num_mv_found) {
  const BlockParameters& bp = *block.bp;
  const std::array<GlobalMotion, kNumReferenceFrameTypes>& global_motion =
      block.tile.frame_header().global_motion;
  PredictionParameters& prediction_parameters = *bp.prediction_parameters;
  // LowerMvPrecision() is not necessary, since the values in
  // |prediction_parameters.global_mv| and |mv_bp.mv| were generated by it.
  CompoundMotionVector candidate_mv = mv_bp.mv;
  for (int i = 0; i < 2; ++i) {
    const auto global_motion_type = global_motion[bp.reference_frame[i]].type;
    if (IsGlobalMvBlock(mv_bp.is_global_mv_block, global_motion_type)) {
      candidate_mv.mv[i] = prediction_parameters.global_mv[i];
    }
  }
  *found_new_mv |= kPredictionModeNewMvMask.Contains(mv_bp.y_mode);
  *found_match = true;
  CompoundMotionVector* const compound_ref_mv_stack =
      prediction_parameters.compound_ref_mv_stack;
  auto result =
      std::find_if(compound_ref_mv_stack, compound_ref_mv_stack + *num_mv_found,
                   [&candidate_mv](const CompoundMotionVector& ref_mv) {
                     return ref_mv == candidate_mv;
                   });
  if (result != compound_ref_mv_stack + *num_mv_found) {
    prediction_parameters.IncreaseWeight(
        std::distance(compound_ref_mv_stack, result), weight);
    return;
  }
  if (*num_mv_found >= kMaxRefMvStackSize) return;
  compound_ref_mv_stack[*num_mv_found] = candidate_mv;
  prediction_parameters.SetWeightIndexStackEntry(*num_mv_found, weight);
  ++*num_mv_found;
}

// 7.10.2.7.
void AddReferenceMvCandidate(const Tile::Block& block,
                             const BlockParameters& mv_bp, bool is_compound,
                             int weight, bool* const found_new_mv,
                             bool* const found_match, int* const num_mv_found) {
  if (!mv_bp.is_inter) return;
  const BlockParameters& bp = *block.bp;
  if (is_compound) {
    if (mv_bp.reference_frame[0] == bp.reference_frame[0] &&
        mv_bp.reference_frame[1] == bp.reference_frame[1]) {
      CompoundSearchStack(block, mv_bp, weight, found_new_mv, found_match,
                          num_mv_found);
    }
    return;
  }
  for (int i = 0; i < 2; ++i) {
    if (mv_bp.reference_frame[i] == bp.reference_frame[0]) {
      SearchStack(block, mv_bp, i, weight, found_new_mv, found_match,
                  num_mv_found);
    }
  }
}

int GetMinimumStep(int block_width_or_height4x4, int delta_row_or_column) {
  assert(delta_row_or_column < 0);
  if (block_width_or_height4x4 >= 16) return 4;
  if (delta_row_or_column < -1) return 2;
  return 0;
}

// 7.10.2.2.
void ScanRow(const Tile::Block& block, int mv_column, int delta_row,
             bool is_compound, bool* const found_new_mv,
             bool* const found_match, int* const num_mv_found) {
  const int mv_row = block.row4x4 + delta_row;
  const Tile& tile = block.tile;
  if (!tile.IsTopInside(mv_row + 1)) return;
  const int width4x4 = block.width4x4;
  const int min_step = GetMinimumStep(width4x4, delta_row);
  BlockParameters** bps = tile.BlockParametersAddress(mv_row, mv_column);
  BlockParameters** const end_bps =
      bps + std::min({static_cast<int>(width4x4),
                      tile.frame_header().columns4x4 - block.column4x4, 16});
  do {
    const BlockParameters& mv_bp = **bps;
    const int step = std::max(
        std::min(width4x4, static_cast<int>(kNum4x4BlocksWide[mv_bp.size])),
        min_step);
    AddReferenceMvCandidate(block, mv_bp, is_compound, MultiplyBy2(step),
                            found_new_mv, found_match, num_mv_found);
    bps += step;
  } while (bps < end_bps);
}

// 7.10.2.3.
void ScanColumn(const Tile::Block& block, int mv_row, int delta_column,
                bool is_compound, bool* const found_new_mv,
                bool* const found_match, int* const num_mv_found) {
  const int mv_column = block.column4x4 + delta_column;
  const Tile& tile = block.tile;
  if (!tile.IsLeftInside(mv_column + 1)) return;
  const int height4x4 = block.height4x4;
  const int min_step = GetMinimumStep(height4x4, delta_column);
  const ptrdiff_t stride = tile.BlockParametersStride();
  BlockParameters** bps = tile.BlockParametersAddress(mv_row, mv_column);
  BlockParameters** const end_bps =
      bps + stride * std::min({static_cast<int>(height4x4),
                               tile.frame_header().rows4x4 - block.row4x4, 16});
  do {
    const BlockParameters& mv_bp = **bps;
    const int step = std::max(
        std::min(height4x4, static_cast<int>(kNum4x4BlocksHigh[mv_bp.size])),
        min_step);
    AddReferenceMvCandidate(block, mv_bp, is_compound, MultiplyBy2(step),
                            found_new_mv, found_match, num_mv_found);
    bps += step * stride;
  } while (bps < end_bps);
}

// 7.10.2.4.
void ScanPoint(const Tile::Block& block, int delta_row, int delta_column,
               bool is_compound, bool* const found_new_mv,
               bool* const found_match, int* const num_mv_found) {
  const int mv_row = block.row4x4 + delta_row;
  const int mv_column = block.column4x4 + delta_column;
  const Tile& tile = block.tile;
  if (!tile.IsInside(mv_row, mv_column) ||
      !tile.HasParameters(mv_row, mv_column)) {
    return;
  }
  const BlockParameters& mv_bp = tile.Parameters(mv_row, mv_column);
  if (mv_bp.reference_frame[0] == kReferenceFrameNone) return;
  AddReferenceMvCandidate(block, mv_bp, is_compound, 4, found_new_mv,
                          found_match, num_mv_found);
}

// 7.10.2.6.
void AddTemporalReferenceMvCandidate(
    const ObuFrameHeader& frame_header, const int reference_offsets[2],
    const MotionVector* const temporal_mvs,
    const int* const temporal_reference_offsets, int count, bool is_compound,
    int* const zero_mv_context, int* const num_mv_found,
    PredictionParameters* const prediction_parameters) {
  const MotionVector* const global_mv = prediction_parameters->global_mv;
  int index = 0;
  if (is_compound) {
    CompoundMotionVector* const compound_ref_mv_stack =
        prediction_parameters->compound_ref_mv_stack;
    do {
      assert(temporal_reference_offsets[index] > 0);
      assert(temporal_reference_offsets[index] <= kMaxFrameDistance);
      CompoundMotionVector candidate_mv = {};
      for (int i = 0; i < 2; ++i) {
        if (reference_offsets[i] != 0) {
          GetMvProjection(temporal_mvs[index], reference_offsets[i],
                          temporal_reference_offsets[index],
                          &candidate_mv.mv[i]);
          LowerMvPrecision(frame_header, &candidate_mv.mv[i]);
        }
      }
      if (*zero_mv_context == -1) {
        int max_difference =
            std::max(std::abs(candidate_mv.mv[0].mv[0] - global_mv[0].mv[0]),
                     std::abs(candidate_mv.mv[0].mv[1] - global_mv[0].mv[1]));
        max_difference =
            std::max(max_difference,
                     std::abs(candidate_mv.mv[1].mv[0] - global_mv[1].mv[0]));
        max_difference =
            std::max(max_difference,
                     std::abs(candidate_mv.mv[1].mv[1] - global_mv[1].mv[1]));
        *zero_mv_context = static_cast<int>(max_difference >= 16);
      }
      const auto result = std::find_if(
          compound_ref_mv_stack, compound_ref_mv_stack + *num_mv_found,
          [&candidate_mv](const CompoundMotionVector& ref_mv) {
            return ref_mv == candidate_mv;
          });
      if (result != compound_ref_mv_stack + *num_mv_found) {
        prediction_parameters->IncreaseWeight(
            std::distance(compound_ref_mv_stack, result), 2);
        continue;
      }
      if (*num_mv_found >= kMaxRefMvStackSize) continue;
      compound_ref_mv_stack[*num_mv_found] = candidate_mv;
      prediction_parameters->SetWeightIndexStackEntry(*num_mv_found, 2);
      ++*num_mv_found;
    } while (++index < count);
    return;
  }
  MotionVector* const ref_mv_stack = prediction_parameters->ref_mv_stack;
  do {
    assert(temporal_reference_offsets[index] > 0);
    assert(temporal_reference_offsets[index] <= kMaxFrameDistance);
    MotionVector candidate_mv = {};
    if (reference_offsets[0] != 0) {
      GetMvProjection(temporal_mvs[index], reference_offsets[0],
                      temporal_reference_offsets[index], &candidate_mv);
      LowerMvPrecision(frame_header, &candidate_mv);
    }
    if (*zero_mv_context == -1) {
      const int max_difference =
          std::max(std::abs(candidate_mv.mv[0] - global_mv[0].mv[0]),
                   std::abs(candidate_mv.mv[1] - global_mv[0].mv[1]));
      *zero_mv_context = static_cast<int>(max_difference >= 16);
    }
    const auto result =
        std::find_if(ref_mv_stack, ref_mv_stack + *num_mv_found,
                     [&candidate_mv](const MotionVector& ref_mv) {
                       return ref_mv == candidate_mv;
                     });
    if (result != ref_mv_stack + *num_mv_found) {
      prediction_parameters->IncreaseWeight(std::distance(ref_mv_stack, result),
                                            2);
      continue;
    }
    if (*num_mv_found >= kMaxRefMvStackSize) continue;
    ref_mv_stack[*num_mv_found] = candidate_mv;
    prediction_parameters->SetWeightIndexStackEntry(*num_mv_found, 2);
    ++*num_mv_found;
  } while (++index < count);
}

// Part of 7.10.2.5.
bool IsWithinTheSame64x64Block(const Tile::Block& block, int delta_row,
                               int delta_column) {
  const int row = (block.row4x4 & 15) + delta_row;
  const int column = (block.column4x4 & 15) + delta_column;
  // |block.height4x4| is at least 2 for all elements in |kTemporalScanMask|.
  // So |row| are all non-negative.
  assert(row >= 0);
  return row < 16 && column >= 0 && column < 16;
}

constexpr BitMaskSet kTemporalScanMask(kBlock8x8, kBlock8x16, kBlock8x32,
                                       kBlock16x8, kBlock16x16, kBlock16x32,
                                       kBlock32x8, kBlock32x16, kBlock32x32);

// 7.10.2.5.
//
// The |zero_mv_context| output parameter may be null. If |zero_mv_context| is
// not null, the function may set |*zero_mv_context|.
void TemporalScan(const Tile::Block& block, bool is_compound,
                  int* const zero_mv_context, int* const num_mv_found) {
  const int step_w = (block.width4x4 >= 16) ? 4 : 2;
  const int step_h = (block.height4x4 >= 16) ? 4 : 2;
  const int row_start = block.row4x4 | 1;
  const int column_start = block.column4x4 | 1;
  const int row_end =
      row_start + std::min(static_cast<int>(block.height4x4), 16);
  const int column_end =
      column_start + std::min(static_cast<int>(block.width4x4), 16);
  const Tile& tile = block.tile;
  const TemporalMotionField& motion_field = tile.motion_field();
  const int stride = motion_field.mv.columns();
  const MotionVector* motion_field_mv = motion_field.mv[0];
  const int8_t* motion_field_reference_offset =
      motion_field.reference_offset[0];
  constexpr int kMaxTemporalMvCandidates = 19;
  MotionVector temporal_mvs[kMaxTemporalMvCandidates];
  int temporal_reference_offsets[kMaxTemporalMvCandidates];
  int count = 0;
  int offset = stride * (row_start >> 1);
  int mv_row = row_start;
  do {
    int mv_column = column_start;
    do {
      // Both horizontal and vertical offsets are positive. Only bottom and
      // right boundaries need to be checked.
      if (tile.IsBottomRightInside(mv_row, mv_column)) {
        const int x8 = mv_column >> 1;
        const MotionVector temporal_mv = motion_field_mv[offset + x8];
        const int temporal_reference_offset =
            motion_field_reference_offset[offset + x8];
        if (temporal_mv.mv[0] == kInvalidMvValue) {
          if (mv_row == row_start && mv_column == column_start) {
            *zero_mv_context = 1;
          }
        } else {
          temporal_mvs[count] = temporal_mv;
          temporal_reference_offsets[count++] = temporal_reference_offset;
        }
      }
      mv_column += step_w;
    } while (mv_column < column_end);
    offset += stride * step_h >> 1;
    mv_row += step_h;
  } while (mv_row < row_end);
  if (kTemporalScanMask.Contains(block.size)) {
    const int temporal_sample_positions[3][2] = {
        {block.height4x4, -2},
        {block.height4x4, block.width4x4},
        {block.height4x4 - 2, block.width4x4}};
    // Getting the address of an element in Array2D is slow. Precalculate the
    // offsets.
    int temporal_sample_offsets[3];
    temporal_sample_offsets[0] = stride * ((row_start + block.height4x4) >> 1) +
                                 ((column_start - 2) >> 1);
    temporal_sample_offsets[1] =
        temporal_sample_offsets[0] + ((block.width4x4 + 2) >> 1);
    temporal_sample_offsets[2] = temporal_sample_offsets[1] - stride;
    for (int i = 0; i < 3; i++) {
      const int row = temporal_sample_positions[i][0];
      const int column = temporal_sample_positions[i][1];
      if (!IsWithinTheSame64x64Block(block, row, column)) continue;
      const int mv_row = row_start + row;
      const int mv_column = column_start + column;
      // IsWithinTheSame64x64Block() guarantees the reference block is inside
      // the top and left boundary.
      if (!tile.IsBottomRightInside(mv_row, mv_column)) continue;
      const MotionVector temporal_mv =
          motion_field_mv[temporal_sample_offsets[i]];
      const int temporal_reference_offset =
          motion_field_reference_offset[temporal_sample_offsets[i]];
      if (temporal_mv.mv[0] != kInvalidMvValue) {
        temporal_mvs[count] = temporal_mv;
        temporal_reference_offsets[count++] = temporal_reference_offset;
      }
    }
  }
  if (count != 0) {
    BlockParameters* bp = block.bp;
    int reference_offsets[2];
    reference_offsets[0] = GetRelativeDistance(
        tile.frame_header().order_hint,
        tile.current_frame().order_hint(bp->reference_frame[0]),
        tile.sequence_header().order_hint_shift_bits);
    if (is_compound) {
      reference_offsets[1] = GetRelativeDistance(
          tile.frame_header().order_hint,
          tile.current_frame().order_hint(bp->reference_frame[1]),
          tile.sequence_header().order_hint_shift_bits);
    }
    AddTemporalReferenceMvCandidate(
        tile.frame_header(), reference_offsets, temporal_mvs,
        temporal_reference_offsets, count, is_compound, zero_mv_context,
        num_mv_found, &(*bp->prediction_parameters));
  }
}

// Part of 7.10.2.13.
void AddExtraCompoundMvCandidate(const Tile::Block& block, int mv_row,
                                 int mv_column, int* const ref_id_count,
                                 MotionVector ref_id[2][2],
                                 int* const ref_diff_count,
                                 MotionVector ref_diff[2][2]) {
  const auto& bp = block.tile.Parameters(mv_row, mv_column);
  const std::array<bool, kNumReferenceFrameTypes>& reference_frame_sign_bias =
      block.tile.reference_frame_sign_bias();
  for (int i = 0; i < 2; ++i) {
    const ReferenceFrameType candidate_reference_frame = bp.reference_frame[i];
    if (candidate_reference_frame <= kReferenceFrameIntra) continue;
    for (int j = 0; j < 2; ++j) {
      MotionVector candidate_mv = bp.mv.mv[i];
      const ReferenceFrameType block_reference_frame =
          block.bp->reference_frame[j];
      if (candidate_reference_frame == block_reference_frame &&
          ref_id_count[j] < 2) {
        ref_id[j][ref_id_count[j]] = candidate_mv;
        ++ref_id_count[j];
      } else if (ref_diff_count[j] < 2) {
        if (reference_frame_sign_bias[candidate_reference_frame] !=
            reference_frame_sign_bias[block_reference_frame]) {
          candidate_mv.mv[0] *= -1;
          candidate_mv.mv[1] *= -1;
        }
        ref_diff[j][ref_diff_count[j]] = candidate_mv;
        ++ref_diff_count[j];
      }
    }
  }
}

// Part of 7.10.2.13.
void AddExtraSingleMvCandidate(const Tile::Block& block, int mv_row,
                               int mv_column, int* const num_mv_found) {
  const auto& bp = block.tile.Parameters(mv_row, mv_column);
  const std::array<bool, kNumReferenceFrameTypes>& reference_frame_sign_bias =
      block.tile.reference_frame_sign_bias();
  const ReferenceFrameType block_reference_frame = block.bp->reference_frame[0];
  PredictionParameters& prediction_parameters =
      *block.bp->prediction_parameters;
  MotionVector* const ref_mv_stack = prediction_parameters.ref_mv_stack;
  for (int i = 0; i < 2; ++i) {
    const ReferenceFrameType candidate_reference_frame = bp.reference_frame[i];
    if (candidate_reference_frame <= kReferenceFrameIntra) continue;
    MotionVector candidate_mv = bp.mv.mv[i];
    if (reference_frame_sign_bias[candidate_reference_frame] !=
        reference_frame_sign_bias[block_reference_frame]) {
      candidate_mv.mv[0] *= -1;
      candidate_mv.mv[1] *= -1;
    }
    assert(*num_mv_found <= 2);
    if ((*num_mv_found != 0 && ref_mv_stack[0] == candidate_mv) ||
        (*num_mv_found == 2 && ref_mv_stack[1] == candidate_mv)) {
      continue;
    }
    ref_mv_stack[*num_mv_found] = candidate_mv;
    prediction_parameters.SetWeightIndexStackEntry(*num_mv_found, 0);
    ++*num_mv_found;
  }
}

// 7.10.2.12.
void ExtraSearch(const Tile::Block& block, bool is_compound,
                 int* const num_mv_found) {
  const Tile& tile = block.tile;
  const int num4x4 = std::min({static_cast<int>(block.width4x4),
                               tile.frame_header().columns4x4 - block.column4x4,
                               static_cast<int>(block.height4x4),
                               tile.frame_header().rows4x4 - block.row4x4, 16});
  int ref_id_count[2] = {};
  MotionVector ref_id[2][2] = {};
  int ref_diff_count[2] = {};
  MotionVector ref_diff[2][2] = {};
  PredictionParameters& prediction_parameters =
      *block.bp->prediction_parameters;
  for (int pass = 0; pass < 2 && *num_mv_found < 2; ++pass) {
    for (int i = 0; i < num4x4;) {
      const int mv_row = block.row4x4 + ((pass == 0) ? -1 : i);
      const int mv_column = block.column4x4 + ((pass == 0) ? i : -1);
      if (!tile.IsTopLeftInside(mv_row + 1, mv_column + 1)) break;
      if (is_compound) {
        AddExtraCompoundMvCandidate(block, mv_row, mv_column, ref_id_count,
                                    ref_id, ref_diff_count, ref_diff);
      } else {
        AddExtraSingleMvCandidate(block, mv_row, mv_column, num_mv_found);
        if (*num_mv_found >= 2) break;
      }
      const auto& bp = tile.Parameters(mv_row, mv_column);
      i +=
          (pass == 0) ? kNum4x4BlocksWide[bp.size] : kNum4x4BlocksHigh[bp.size];
    }
  }
  if (is_compound) {
    // Merge compound mode extra search into mv stack.
    CompoundMotionVector* const compound_ref_mv_stack =
        prediction_parameters.compound_ref_mv_stack;
    CompoundMotionVector combined_mvs[2] = {};
    for (int i = 0; i < 2; ++i) {
      int count = 0;
      assert(ref_id_count[i] <= 2);
      for (int j = 0; j < ref_id_count[i]; ++j, ++count) {
        combined_mvs[count].mv[i] = ref_id[i][j];
      }
      for (int j = 0; j < ref_diff_count[i] && count < 2; ++j, ++count) {
        combined_mvs[count].mv[i] = ref_diff[i][j];
      }
      for (; count < 2; ++count) {
        combined_mvs[count].mv[i] = prediction_parameters.global_mv[i];
      }
    }
    if (*num_mv_found == 1) {
      if (combined_mvs[0] == compound_ref_mv_stack[0]) {
        compound_ref_mv_stack[1] = combined_mvs[1];
      } else {
        compound_ref_mv_stack[1] = combined_mvs[0];
      }
      prediction_parameters.SetWeightIndexStackEntry(1, 0);
    } else {
      assert(*num_mv_found == 0);
      for (int i = 0; i < 2; ++i) {
        compound_ref_mv_stack[i] = combined_mvs[i];
        prediction_parameters.SetWeightIndexStackEntry(i, 0);
      }
    }
    *num_mv_found = 2;
  } else {
    // single prediction mode
    MotionVector* const ref_mv_stack = prediction_parameters.ref_mv_stack;
    for (int i = *num_mv_found; i < 2; ++i) {
      ref_mv_stack[i] = prediction_parameters.global_mv[0];
      prediction_parameters.SetWeightIndexStackEntry(i, 0);
    }
  }
}

void DescendingOrderTwo(int* const a, int* const b) {
  if (*a < *b) {
    std::swap(*a, *b);
  }
}

// Comparator used for sorting candidate motion vectors in descending order of
// their weights (as specified in 7.10.2.11).
bool CompareCandidateMotionVectors(const int16_t& lhs, const int16_t& rhs) {
  return lhs > rhs;
}

void SortWeightIndexStack(const int size, int16_t* const weight_index_stack) {
  if (size <= 1) return;
  if (size <= 3) {
    // Specialize small sort sizes to speed up.
    int weight_index_0 = weight_index_stack[0];
    int weight_index_1 = weight_index_stack[1];
    DescendingOrderTwo(&weight_index_0, &weight_index_1);
    if (size == 3) {
      int weight_index_2 = weight_index_stack[2];
      DescendingOrderTwo(&weight_index_1, &weight_index_2);
      DescendingOrderTwo(&weight_index_0, &weight_index_1);
      weight_index_stack[2] = weight_index_2;
    }
    weight_index_stack[0] = weight_index_0;
    weight_index_stack[1] = weight_index_1;
  } else {
    std::sort(&weight_index_stack[0], &weight_index_stack[size],
              CompareCandidateMotionVectors);
  }
}

// 7.10.2.14 (part 2).
void ComputeContexts(bool found_new_mv, int nearest_matches, int total_matches,
                     int* new_mv_context, int* reference_mv_context) {
  switch (nearest_matches) {
    case 0:
      *new_mv_context = std::min(total_matches, 1);
      *reference_mv_context = total_matches;
      break;
    case 1:
      *new_mv_context = 3 - static_cast<int>(found_new_mv);
      *reference_mv_context = 2 + total_matches;
      break;
    default:
      *new_mv_context = 5 - static_cast<int>(found_new_mv);
      *reference_mv_context = 5;
      break;
  }
}

// 7.10.4.2.
void AddSample(const Tile::Block& block, int delta_row, int delta_column,
               int* const num_warp_samples, int* const num_samples_scanned,
               int candidates[kMaxLeastSquaresSamples][4]) {
  if (*num_samples_scanned >= kMaxLeastSquaresSamples) return;
  const int mv_row = block.row4x4 + delta_row;
  const int mv_column = block.column4x4 + delta_column;
  const Tile& tile = block.tile;
  if (!tile.IsInside(mv_row, mv_column) ||
      !tile.HasParameters(mv_row, mv_column)) {
    return;
  }
  const BlockParameters& bp = *block.bp;
  const BlockParameters& mv_bp = tile.Parameters(mv_row, mv_column);
  if (mv_bp.reference_frame[0] != bp.reference_frame[0] ||
      mv_bp.reference_frame[1] != kReferenceFrameNone) {
    return;
  }
  ++*num_samples_scanned;
  const int candidate_height4x4 = kNum4x4BlocksHigh[mv_bp.size];
  const int candidate_row = mv_row & ~(candidate_height4x4 - 1);
  const int candidate_width4x4 = kNum4x4BlocksWide[mv_bp.size];
  const int candidate_column = mv_column & ~(candidate_width4x4 - 1);
  const BlockParameters& candidate_bp =
      tile.Parameters(candidate_row, candidate_column);
  const int mv_diff_row =
      std::abs(candidate_bp.mv.mv[0].mv[0] - bp.mv.mv[0].mv[0]);
  const int mv_diff_column =
      std::abs(candidate_bp.mv.mv[0].mv[1] - bp.mv.mv[0].mv[1]);
  const bool is_valid =
      mv_diff_row + mv_diff_column <= kWarpValidThreshold[block.size];
  if (!is_valid && *num_samples_scanned > 1) {
    return;
  }
  const int mid_y =
      MultiplyBy4(candidate_row) + MultiplyBy2(candidate_height4x4) - 1;
  const int mid_x =
      MultiplyBy4(candidate_column) + MultiplyBy2(candidate_width4x4) - 1;
  candidates[*num_warp_samples][0] = MultiplyBy8(mid_y);
  candidates[*num_warp_samples][1] = MultiplyBy8(mid_x);
  candidates[*num_warp_samples][2] =
      MultiplyBy8(mid_y) + candidate_bp.mv.mv[0].mv[0];
  candidates[*num_warp_samples][3] =
      MultiplyBy8(mid_x) + candidate_bp.mv.mv[0].mv[1];
  if (is_valid) ++*num_warp_samples;
}

// 7.9.2.
// In the spec, |dst_sign| is either 1 or -1. Here we set |dst_sign| to either 0
// or -1 so that it can be XORed and subtracted directly in ApplySign() and
// corresponding SIMD implementations.
bool MotionFieldProjection(
    const ObuFrameHeader& frame_header, const RefCountedBuffer& current_frame,
    const std::array<RefCountedBufferPtr, kNumReferenceFrameTypes>&
        reference_frames,
    ReferenceFrameType source, unsigned int order_hint_shift_bits,
    int reference_to_current_with_sign, int dst_sign, int y8_start, int y8_end,
    int x8_start, int x8_end, TemporalMotionField* const motion_field) {
  const int source_index =
      frame_header.reference_frame_index[source - kReferenceFrameLast];
  auto* const source_frame = reference_frames[source_index].get();
  assert(source_frame != nullptr);
  assert(dst_sign == 0 || dst_sign == -1);
  if (source_frame->rows4x4() != frame_header.rows4x4 ||
      source_frame->columns4x4() != frame_header.columns4x4 ||
      IsIntraFrame(source_frame->frame_type())) {
    return false;
  }
  assert(reference_to_current_with_sign >= -kMaxFrameDistance);
  if (reference_to_current_with_sign > kMaxFrameDistance) return true;
  const dsp::Dsp* const dsp = dsp::GetDspTable(8);
  assert(dsp != nullptr);
  dsp->motion_field_projection_kernel(
      source_frame->motion_field_reference_frame(y8_start, 0),
      source_frame->motion_field_mv(y8_start, 0),
      source_frame->order_hint_array(), current_frame.order_hint(source),
      order_hint_shift_bits, reference_to_current_with_sign, dst_sign, y8_start,
      y8_end, x8_start, x8_end, motion_field);
  return true;
}

}  // namespace

void FindMvStack(const Tile::Block& block, bool is_compound,
                 MvContexts* const contexts) {
  PredictionParameters& prediction_parameters =
      *block.bp->prediction_parameters;
  SetupGlobalMv(block, 0, &prediction_parameters.global_mv[0]);
  if (is_compound) SetupGlobalMv(block, 1, &prediction_parameters.global_mv[1]);
  bool found_new_mv = false;
  bool found_row_match = false;
  int num_mv_found = 0;
  ScanRow(block, block.column4x4, -1, is_compound, &found_new_mv,
          &found_row_match, &num_mv_found);
  bool found_column_match = false;
  ScanColumn(block, block.row4x4, -1, is_compound, &found_new_mv,
             &found_column_match, &num_mv_found);
  if (std::max(block.width4x4, block.height4x4) <= 16) {
    ScanPoint(block, -1, block.width4x4, is_compound, &found_new_mv,
              &found_row_match, &num_mv_found);
  }
  const int nearest_matches =
      static_cast<int>(found_row_match) + static_cast<int>(found_column_match);
  prediction_parameters.nearest_mv_count = num_mv_found;
  if (block.tile.frame_header().use_ref_frame_mvs) {
    // Initialize to invalid value, and it will be set when temporal mv is zero.
    contexts->zero_mv = -1;
    TemporalScan(block, is_compound, &contexts->zero_mv, &num_mv_found);
  } else {
    contexts->zero_mv = 0;
  }
  bool dummy_bool = false;
  ScanPoint(block, -1, -1, is_compound, &dummy_bool, &found_row_match,
            &num_mv_found);
  static constexpr int deltas[2] = {-3, -5};
  for (int i = 0; i < 2; ++i) {
    if (i == 0 || block.height4x4 > 1) {
      ScanRow(block, block.column4x4 | 1, deltas[i] + (block.row4x4 & 1),
              is_compound, &dummy_bool, &found_row_match, &num_mv_found);
    }
    if (i == 0 || block.width4x4 > 1) {
      ScanColumn(block, block.row4x4 | 1, deltas[i] + (block.column4x4 & 1),
                 is_compound, &dummy_bool, &found_column_match, &num_mv_found);
    }
  }
  if (num_mv_found < 2) {
    ExtraSearch(block, is_compound, &num_mv_found);
  } else {
    // The sort of |weight_index_stack| could be moved to Tile::AssignIntraMv()
    // and Tile::AssignInterMv(), and only do a partial sort to the max index we
    // need. However, the speed gain is trivial.
    SortWeightIndexStack(prediction_parameters.nearest_mv_count,
                         prediction_parameters.weight_index_stack);
    // For intra case, only the first 1 or 2 mvs in the stack will be used.
    // For inter case, |prediction_parameters.ref_mv_index| is at most 3.
    // When there are 4 or more nearest mvs, the other mvs will not be used.
    if (prediction_parameters.nearest_mv_count < 4) {
      SortWeightIndexStack(
          num_mv_found - prediction_parameters.nearest_mv_count,
          prediction_parameters.weight_index_stack +
              prediction_parameters.nearest_mv_count);
    }
  }
  prediction_parameters.ref_mv_count = num_mv_found;
  const int total_matches =
      static_cast<int>(found_row_match) + static_cast<int>(found_column_match);
  ComputeContexts(found_new_mv, nearest_matches, total_matches,
                  &contexts->new_mv, &contexts->reference_mv);
  // The mv stack clamping process is in Tile::AssignIntraMv() and
  // Tile::AssignInterMv(), and only up to two mvs are clamped.
}

void FindWarpSamples(const Tile::Block& block, int* const num_warp_samples,
                     int* const num_samples_scanned,
                     int candidates[kMaxLeastSquaresSamples][4]) {
  const Tile& tile = block.tile;
  bool top_left = true;
  bool top_right = true;
  int step = 1;
  if (block.top_available[kPlaneY]) {
    BlockSize source_size =
        tile.Parameters(block.row4x4 - 1, block.column4x4).size;
    const int source_width4x4 = kNum4x4BlocksWide[source_size];
    if (block.width4x4 <= source_width4x4) {
      // The & here is equivalent to % since source_width4x4 is a power of two.
      const int column_offset = -(block.column4x4 & (source_width4x4 - 1));
      if (column_offset < 0) top_left = false;
      if (column_offset + source_width4x4 > block.width4x4) top_right = false;
      AddSample(block, -1, 0, num_warp_samples, num_samples_scanned,
                candidates);
    } else {
      for (int i = 0;
           i < std::min(static_cast<int>(block.width4x4),
                        tile.frame_header().columns4x4 - block.column4x4);
           i += step) {
        source_size =
            tile.Parameters(block.row4x4 - 1, block.column4x4 + i).size;
        step = std::min(static_cast<int>(block.width4x4),
                        static_cast<int>(kNum4x4BlocksWide[source_size]));
        AddSample(block, -1, i, num_warp_samples, num_samples_scanned,
                  candidates);
      }
    }
  }
  if (block.left_available[kPlaneY]) {
    BlockSize source_size =
        tile.Parameters(block.row4x4, block.column4x4 - 1).size;
    const int source_height4x4 = kNum4x4BlocksHigh[source_size];
    if (block.height4x4 <= source_height4x4) {
      const int row_offset = -(block.row4x4 & (source_height4x4 - 1));
      if (row_offset < 0) top_left = false;
      AddSample(block, 0, -1, num_warp_samples, num_samples_scanned,
                candidates);
    } else {
      for (int i = 0; i < std::min(static_cast<int>(block.height4x4),
                                   tile.frame_header().rows4x4 - block.row4x4);
           i += step) {
        source_size =
            tile.Parameters(block.row4x4 + i, block.column4x4 - 1).size;
        step = std::min(static_cast<int>(block.height4x4),
                        static_cast<int>(kNum4x4BlocksHigh[source_size]));
        AddSample(block, i, -1, num_warp_samples, num_samples_scanned,
                  candidates);
      }
    }
  }
  if (top_left) {
    AddSample(block, -1, -1, num_warp_samples, num_samples_scanned, candidates);
  }
  if (top_right && block.size <= kBlock64x64) {
    AddSample(block, -1, block.width4x4, num_warp_samples, num_samples_scanned,
              candidates);
  }
  if (*num_warp_samples == 0 && *num_samples_scanned > 0) *num_warp_samples = 1;
}

void SetupMotionField(
    const ObuFrameHeader& frame_header, const RefCountedBuffer& current_frame,
    const std::array<RefCountedBufferPtr, kNumReferenceFrameTypes>&
        reference_frames,
    unsigned int order_hint_shift_bits, int row4x4_start, int row4x4_end,
    int column4x4_start, int column4x4_end,
    TemporalMotionField* const motion_field) {
  assert(frame_header.use_ref_frame_mvs);
  assert(order_hint_shift_bits != 0);
  const int y8_start = DivideBy2(row4x4_start);
  const int y8_end = DivideBy2(std::min(row4x4_end, frame_header.rows4x4));
  const int x8_start = DivideBy2(column4x4_start);
  const int x8_end =
      DivideBy2(std::min(column4x4_end, frame_header.columns4x4));
  const int8_t* const reference_frame_index =
      frame_header.reference_frame_index;
  const int last_index = reference_frame_index[0];
  const int last_alternate_order_hint =
      reference_frames[last_index]->order_hint(kReferenceFrameAlternate);
  const int current_gold_order_hint =
      current_frame.order_hint(kReferenceFrameGolden);
  if (last_alternate_order_hint != current_gold_order_hint) {
    const int reference_offset_last =
        -GetRelativeDistance(current_frame.order_hint(kReferenceFrameLast),
                             frame_header.order_hint, order_hint_shift_bits);
    if (std::abs(reference_offset_last) <= kMaxFrameDistance) {
      MotionFieldProjection(frame_header, current_frame, reference_frames,
                            kReferenceFrameLast, order_hint_shift_bits,
                            reference_offset_last, -1, y8_start, y8_end,
                            x8_start, x8_end, motion_field);
    }
  }
  int ref_stamp = 1;
  const int reference_offset_backward =
      GetRelativeDistance(current_frame.order_hint(kReferenceFrameBackward),
                          frame_header.order_hint, order_hint_shift_bits);
  if (reference_offset_backward > 0 &&
      MotionFieldProjection(frame_header, current_frame, reference_frames,
                            kReferenceFrameBackward, order_hint_shift_bits,
                            reference_offset_backward, 0, y8_start, y8_end,
                            x8_start, x8_end, motion_field)) {
    --ref_stamp;
  }
  const int reference_offset_alternate2 =
      GetRelativeDistance(current_frame.order_hint(kReferenceFrameAlternate2),
                          frame_header.order_hint, order_hint_shift_bits);
  if (reference_offset_alternate2 > 0 &&
      MotionFieldProjection(frame_header, current_frame, reference_frames,
                            kReferenceFrameAlternate2, order_hint_shift_bits,
                            reference_offset_alternate2, 0, y8_start, y8_end,
                            x8_start, x8_end, motion_field)) {
    --ref_stamp;
  }
  if (ref_stamp >= 0) {
    const int reference_offset_alternate =
        GetRelativeDistance(current_frame.order_hint(kReferenceFrameAlternate),
                            frame_header.order_hint, order_hint_shift_bits);
    if (reference_offset_alternate > 0 &&
        MotionFieldProjection(frame_header, current_frame, reference_frames,
                              kReferenceFrameAlternate, order_hint_shift_bits,
                              reference_offset_alternate, 0, y8_start, y8_end,
                              x8_start, x8_end, motion_field)) {
      --ref_stamp;
    }
  }
  if (ref_stamp >= 0) {
    const int reference_offset_last2 =
        -GetRelativeDistance(current_frame.order_hint(kReferenceFrameLast2),
                             frame_header.order_hint, order_hint_shift_bits);
    if (std::abs(reference_offset_last2) <= kMaxFrameDistance) {
      MotionFieldProjection(frame_header, current_frame, reference_frames,
                            kReferenceFrameLast2, order_hint_shift_bits,
                            reference_offset_last2, -1, y8_start, y8_end,
                            x8_start, x8_end, motion_field);
    }
  }
}

}  // namespace libgav1
