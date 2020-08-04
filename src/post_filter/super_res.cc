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
#include "src/post_filter.h"
#include "src/utils/blocking_counter.h"

namespace libgav1 {

template <bool in_place>
void PostFilter::ApplySuperRes(const std::array<uint8_t*, kMaxPlanes>& src,
                               const std::array<int, kMaxPlanes>& rows,
                               size_t line_buffer_offset,
                               const std::array<uint8_t*, kMaxPlanes>& dst) {
  // Only used when |in_place| == false.
  uint8_t* const line_buffer_start = superres_line_buffer_ +
                                     line_buffer_offset +
                                     kSuperResHorizontalBorder * pixel_size_;
  int plane = kPlaneY;
  do {
    const int8_t subsampling_x = subsampling_x_[plane];
    const int plane_width =
        MultiplyBy4(frame_header_.columns4x4) >> subsampling_x;
    uint8_t* input = src[plane];
    uint8_t* output = dst[plane];
#if LIBGAV1_MAX_BITDEPTH >= 10
    if (bitdepth_ >= 10) {
      for (int y = 0; y < rows[plane]; ++y,
               input += frame_buffer_.stride(plane),
               output += frame_buffer_.stride(plane)) {
        if (!in_place) {
          memcpy(line_buffer_start, input, plane_width * sizeof(uint16_t));
        }
        ExtendLine<uint16_t>(in_place ? input : line_buffer_start, plane_width,
                             kSuperResHorizontalBorder,
                             kSuperResHorizontalBorder);
        dsp_.super_res_row(in_place ? input : line_buffer_start,
                           super_res_info_[plane].upscaled_width,
                           super_res_info_[plane].initial_subpixel_x,
                           super_res_info_[plane].step, output);
      }
      continue;
    }
#endif  // LIBGAV1_MAX_BITDEPTH >= 10
    for (int y = 0; y < rows[plane]; ++y, input += frame_buffer_.stride(plane),
             output += frame_buffer_.stride(plane)) {
      if (!in_place) memcpy(line_buffer_start, input, plane_width);
      ExtendLine<uint8_t>(in_place ? input : line_buffer_start, plane_width,
                          kSuperResHorizontalBorder, kSuperResHorizontalBorder);
      dsp_.super_res_row(in_place ? input : line_buffer_start,
                         super_res_info_[plane].upscaled_width,
                         super_res_info_[plane].initial_subpixel_x,
                         super_res_info_[plane].step, output);
    }
  } while (++plane < planes_);
}

template void PostFilter::ApplySuperRes<false>(
    const std::array<uint8_t*, kMaxPlanes>& src,
    const std::array<int, kMaxPlanes>& rows, size_t line_buffer_offset,
    const std::array<uint8_t*, kMaxPlanes>& dst);

void PostFilter::ApplySuperResForOneSuperBlockRow(int row4x4_start, int sb4x4,
                                                  bool is_last_row) {
  assert(row4x4_start >= 0);
  assert(DoSuperRes());
  // If not doing cdef, then LR needs two rows of border with superres applied.
  const int num_rows_extra = (DoCdef() || !DoRestoration()) ? 0 : 2;
  std::array<uint8_t*, kMaxPlanes> src;
  std::array<uint8_t*, kMaxPlanes> dst;
  std::array<int, kMaxPlanes> rows;
  // Apply superres for the last 8-num_rows_extra rows of the previous
  // superblock.
  if (row4x4_start > 0) {
    const int row4x4 = row4x4_start - 2;
    int plane = 0;
    do {
      const int row =
          (MultiplyBy4(row4x4) >> subsampling_y_[plane]) + num_rows_extra;
      const ptrdiff_t row_offset = row * frame_buffer_.stride(plane);
      src[plane] = cdef_buffer_[plane] + row_offset;
      dst[plane] = superres_buffer_[plane] + row_offset;
      // Note that the |num_rows_extra| subtraction is done after the value is
      // subsampled since we always need to work on |num_rows_extra| extra rows
      // irrespective of the plane subsampling.
      rows[plane] = (8 >> subsampling_y_[plane]) - num_rows_extra;
    } while (++plane < planes_);
    ApplySuperRes<true>(src, rows, /*line_buffer_offset=*/0, dst);
  }
  // Apply superres for the current superblock row (except for the last
  // 8-num_rows_extra rows).
  const int num_rows4x4 =
      std::min(sb4x4, frame_header_.rows4x4 - row4x4_start) -
      (is_last_row ? 0 : 2);
  int plane = 0;
  do {
    const ptrdiff_t row_offset =
        (MultiplyBy4(row4x4_start) >> subsampling_y_[plane]) *
        frame_buffer_.stride(plane);
    src[plane] = cdef_buffer_[plane] + row_offset;
    dst[plane] = superres_buffer_[plane] + row_offset;
    // Note that the |num_rows_extra| subtraction is done after the value is
    // subsampled since we always need to work on |num_rows_extra| extra rows
    // irrespective of the plane subsampling.
    rows[plane] = (MultiplyBy4(num_rows4x4) >> subsampling_y_[plane]) +
                  (is_last_row ? 0 : num_rows_extra);
  } while (++plane < planes_);
  ApplySuperRes<true>(src, rows, /*line_buffer_offset=*/0, dst);
}

void PostFilter::ApplySuperResThreaded() {
  const int num_threads = thread_pool_->num_threads() + 1;
  // The number of rows4x4 that will be processed by each thread in the thread
  // pool (other than the current thread).
  const int thread_pool_rows4x4 = frame_header_.rows4x4 / num_threads;
  // For the current thread, we round up to process all the remaining rows so
  // that the current thread's job will potentially run the longest.
  const int current_thread_rows4x4 =
      frame_header_.rows4x4 - (thread_pool_rows4x4 * (num_threads - 1));
  // The size of the line buffer required by each thread. In the multi-threaded
  // case we are guaranteed to have a line buffer which can store |num_threads|
  // rows at the same time.
  const size_t line_buffer_size =
      (MultiplyBy4(frame_header_.columns4x4) +
       MultiplyBy2(kSuperResHorizontalBorder) + kSuperResHorizontalPadding) *
      pixel_size_;
  size_t line_buffer_offset = 0;
  BlockingCounter pending_workers(num_threads - 1);
  for (int i = 0, row4x4_start = 0; i < num_threads; ++i,
           row4x4_start += thread_pool_rows4x4,
           line_buffer_offset += line_buffer_size) {
    std::array<uint8_t*, kMaxPlanes> src;
    std::array<uint8_t*, kMaxPlanes> dst;
    std::array<int, kMaxPlanes> rows;
    int plane = 0;
    do {
      src[plane] =
          GetBufferOffset(cdef_buffer_[plane], frame_buffer_.stride(plane),
                          static_cast<Plane>(plane), row4x4_start, 0);
      dst[plane] =
          GetSuperResBuffer(static_cast<Plane>(plane), row4x4_start, 0);
      if (i < num_threads - 1) {
        rows[plane] = MultiplyBy4(thread_pool_rows4x4) >> subsampling_y_[plane];
      } else {
        rows[plane] =
            MultiplyBy4(current_thread_rows4x4) >> subsampling_y_[plane];
      }
    } while (++plane < planes_);
    if (i < num_threads - 1) {
      thread_pool_->Schedule(
          [this, src, rows, line_buffer_offset, dst, &pending_workers]() {
            ApplySuperRes<false>(src, rows, line_buffer_offset, dst);
            pending_workers.Decrement();
          });
    } else {
      ApplySuperRes<false>(src, rows, line_buffer_offset, dst);
    }
  }
  // Wait for the threadpool jobs to finish.
  pending_workers.Wait();
}

}  // namespace libgav1
