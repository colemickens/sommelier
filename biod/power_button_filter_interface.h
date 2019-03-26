// Copyright 2019 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef BIOD_POWER_BUTTON_FILTER_INTERFACE_H_
#define BIOD_POWER_BUTTON_FILTER_INTERFACE_H_

namespace biod {

class PowerButtonFilterInterface {
 public:
  PowerButtonFilterInterface() = default;
  virtual ~PowerButtonFilterInterface() = default;

  // Returns true if a power Button event is seen in the last
  // |kAuthIgnoreTimeoutmsecs| and if we have not filtered a match after latest
  // power button press.
  virtual bool ShouldFilterFingerprintMatch() = 0;

 private:
  DISALLOW_COPY_AND_ASSIGN(PowerButtonFilterInterface);
};

}  // namespace biod

#endif  // BIOD_POWER_BUTTON_FILTER_INTERFACE_H_
