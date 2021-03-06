// Copyright 2019 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "biod/ec_command_factory.h"

namespace biod {

std::unique_ptr<EcCommandInterface> EcCommandFactory::FpContextCommand(
    CrosFpDeviceInterface* cros_fp, const std::string& user_id) {
  return FpContextCommandFactory::Create(cros_fp, user_id);
}

}  // namespace biod
