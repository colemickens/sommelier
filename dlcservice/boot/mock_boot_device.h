// Copyright 2018 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef DLCSERVICE_BOOT_MOCK_BOOT_DEVICE_H_
#define DLCSERVICE_BOOT_MOCK_BOOT_DEVICE_H_

#include "dlcservice/boot/boot_device.h"

#include <string>

#include <base/macros.h>

namespace dlcservice {

class MockBootDevice : public BootDeviceInterface {
 public:
  MockBootDevice() = default;

  MOCK_METHOD(bool, IsRemovableDevice, (const std::string&), (override));
  MOCK_METHOD(std::string, GetBootDevice, (), (override));

 private:
  DISALLOW_COPY_AND_ASSIGN(MockBootDevice);
};

}  // namespace dlcservice

#endif  // DLCSERVICE_BOOT_MOCK_BOOT_DEVICE_H_
