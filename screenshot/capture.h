// Copyright 2018 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SCREENSHOT_CAPTURE_H_
#define SCREENSHOT_CAPTURE_H_

#include <stdint.h>

#include <memory>

#include <base/macros.h>
#include <gbm.h>

#include "screenshot/ptr_util.h"

namespace screenshot {

class Crtc;

// Utility class to map/unmap GBM buffer with RAII.
class GbmBoMap {
 public:
  GbmBoMap(ScopedGbmDevicePtr device, ScopedGbmBoPtr bo, uint32_t x, uint32_t y,
           uint32_t width, uint32_t height);
  ~GbmBoMap();

  uint32_t width() const { return width_; }
  uint32_t height() const { return height_; }
  uint32_t stride() const { return stride_; }
  void* buffer() const { return buffer_; }

 private:
  const ScopedGbmDevicePtr device_;
  const ScopedGbmBoPtr bo_;
  const uint32_t width_;
  const uint32_t height_;
  uint32_t stride_ = 0;
  void* map_data_ = nullptr;
  void* buffer_ = nullptr;

  DISALLOW_COPY_AND_ASSIGN(GbmBoMap);
};

// Captures a screenshot from the specified CRTC.
std::unique_ptr<GbmBoMap> Capture(const Crtc& crtc, uint32_t x, uint32_t y,
                                  uint32_t width, uint32_t height);

}  // namespace screenshot

#endif  // SCREENSHOT_CAPTURE_H_
