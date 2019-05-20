// Copyright 2019 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef MEMS_SETUP_CONFIGURATION_H_
#define MEMS_SETUP_CONFIGURATION_H_

#include <memory>
#include <string>

#include <base/macros.h>

#include "mems_setup/delegate.h"
#include "mems_setup/iio_device.h"
#include "mems_setup/sensor_kind.h"

namespace mems_setup {

class Configuration {
 public:
  Configuration(IioDevice* sensor, SensorKind kind, Delegate* delegate);

  bool Configure();

 private:
  bool ConfigGyro();

  bool CopyCalibrationBiasFromVpd(int max_value);
  bool CopyCalibrationBiasFromVpd(int max_value, const std::string& location);

  Delegate* delegate_;  // non-owned
  SensorKind kind_;
  IioDevice* sensor_;  // non-owned

  DISALLOW_COPY_AND_ASSIGN(Configuration);
};

}  // namespace mems_setup

#endif  // MEMS_SETUP_CONFIGURATION_H_
