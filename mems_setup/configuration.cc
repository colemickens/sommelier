// Copyright 2019 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <vector>

#include <base/files/file_util.h>
#include <base/logging.h>
#include <base/strings/string_number_conversions.h>
#include <base/strings/stringprintf.h>

#include "mems_setup/configuration.h"
#include "mems_setup/iio_channel.h"
#include "mems_setup/iio_context.h"
#include "mems_setup/iio_device.h"
#include "mems_setup/sensor_location.h"

namespace mems_setup {

namespace {

struct VpdCalibrationEntry {
  std::string name_;
  std::string calib_;
  base::Optional<int> value_;
};

constexpr char kCalibrationBias[] = "bias";

constexpr int kGyroMaxVpdCalibration = 16384;  // 16dps
constexpr int kAccelMaxVpdCalibration = 256;  // .250g
constexpr int kAccelSysfsTriggerId = 0;
}  // namespace

Configuration::Configuration(IioDevice* sensor, SensorKind kind, Delegate* del)
    : delegate_(del), kind_(kind), sensor_(sensor) {}

bool Configuration::Configure() {
  switch (kind_) {
    case SensorKind::ACCELEROMETER:
      return ConfigAccelerometer();
    case SensorKind::GYROSCOPE:
      return ConfigGyro();
    default:
      LOG(ERROR) << "unimplemented";
  }
  return false;
}

bool Configuration::CopyCalibrationBiasFromVpd(int max_value) {
  if (sensor_->IsSingleSensor()) {
    auto location = sensor_->ReadStringAttribute("location");
    if (!location || location->empty()) {
      LOG(ERROR) << "cannot read a valid sensor location";
      return false;
    }
    return CopyCalibrationBiasFromVpd(max_value, location->c_str());
  } else {
    bool base_config = CopyCalibrationBiasFromVpd(max_value,
                                                  kBaseSensorLocation);
    bool lid_config = CopyCalibrationBiasFromVpd(max_value,
                                                 kLidSensorLocation);
    return base_config && lid_config;
  }
}

bool Configuration::CopyCalibrationBiasFromVpd(int max_value,
                                               const std::string& location) {
  const bool is_single_sensor = sensor_->IsSingleSensor();
  std::string kind = SensorKindToString(kind_);

  std::vector<VpdCalibrationEntry> calib_attributes = {
      {"x", kCalibrationBias, base::nullopt},
      {"y", kCalibrationBias, base::nullopt},
      {"z", kCalibrationBias, base::nullopt},
  };

  for (auto& calib_attribute : calib_attributes) {
    auto attrib_name = base::StringPrintf(
        "in_%s_%s_%s_calib%s", kind.c_str(), calib_attribute.name_.c_str(),
        location.c_str(), calib_attribute.calib_.c_str());
    auto attrib_value = delegate_->ReadVpdValue(attrib_name.c_str());
    if (!attrib_value.has_value()) {
      LOG(ERROR) << "VPD missing calibration value " << attrib_name;
      continue;
    }

    int value;
    if (!base::StringToInt(attrib_value.value(), &value)) {
      LOG(ERROR) << "VPD calibration value " << attrib_name
                 << " has invalid value " << attrib_value.value();
      continue;
    }
    if (abs(value) > max_value) {
      LOG(ERROR) << "VPD calibration value " << attrib_name
                 << " has out-of-range value " << attrib_value.value();
      continue;
    } else {
      calib_attribute.value_ = value;
    }
  }

  for (const auto& calib_attribute : calib_attributes) {
    if (!calib_attribute.value_)
      continue;
    auto attrib_name = base::StringPrintf("in_%s_%s", kind.c_str(),
                                          calib_attribute.name_.c_str());

    if (!is_single_sensor)
      attrib_name =
          base::StringPrintf("%s_%s", attrib_name.c_str(), location.c_str());
    attrib_name = base::StringPrintf("%s_calib%s", attrib_name.c_str(),
                                     calib_attribute.calib_.c_str());

    if (!sensor_->WriteNumberAttribute(attrib_name, *calib_attribute.value_))
      LOG(ERROR) << "failed to set calibration value " << attrib_name;
  }

  LOG(INFO) << "VPD calibration complete";
  return true;
}

bool Configuration::AddSysfsTrigger(int trigger_id) {
  auto iio_trig = sensor_->GetContext()->GetDevice("iio_sysfs_trigger");
  if (iio_trig == nullptr) {
    LOG(ERROR) << "cannot find iio_trig_sysfs kernel module";
    return false;
  }

  // There is a potential cross-process race here, where multiple instances
  // of this tool may be trying to access the trigger at once. To solve this,
  // first see if the trigger is already there. If not, try to create it, and
  // then try to access it again. Only if the latter access fails then
  // error out.
  std::string trigger_name = base::StringPrintf("trigger%d", trigger_id);
  auto trigger = sensor_->GetContext()->GetDevice(trigger_name);
  if (trigger == nullptr) {
    LOG(INFO) << trigger_name << " not found; adding";
    if (!iio_trig->WriteNumberAttribute("add_trigger", trigger_id)) {
      LOG(WARNING) << "cannot instantiate trigger " << trigger_name;
    }

    sensor_->GetContext()->Reload();
    trigger = sensor_->GetContext()->GetDevice(trigger_name);
    if (trigger == nullptr) {
      LOG(ERROR) << "cannot find trigger device " << trigger_name;
      return false;
    }
  }

  if (!sensor_->SetTrigger(trigger)) {
    LOG(ERROR) << "cannot set sensor's trigger to " << trigger_name;
    return false;
  }

  base::FilePath trigger_now = trigger->GetPath().Append("trigger_now");

  base::Optional<gid_t> chronos_gid = delegate_->FindGroupId("chronos");
  if (!chronos_gid) {
    LOG(ERROR) << "chronos group not found";
    return false;
  }

  if (!delegate_->SetOwnership(trigger_now, -1, chronos_gid.value())) {
    LOG(ERROR) << "cannot configure ownership on the trigger";
    return false;
  }

  int permission = delegate_->GetPermissions(trigger_now);
  permission |= base::FILE_PERMISSION_WRITE_BY_GROUP;
  if (!delegate_->SetPermissions(trigger_now, permission)) {
    LOG(ERROR) << "cannot configure permissions on the trigger";
    return false;
  }

  LOG(INFO) << "sysfs trigger setup complete";
  return true;
}

bool Configuration::ConfigGyro() {
  if (!CopyCalibrationBiasFromVpd(kGyroMaxVpdCalibration))
    return false;

  LOG(INFO) << "gyroscope configuration complete";
  return true;
}

bool Configuration::ConfigAccelerometer() {
  if (!CopyCalibrationBiasFromVpd(kAccelMaxVpdCalibration))
    return false;

  if (!AddSysfsTrigger(kAccelSysfsTriggerId))
    return false;

  LOG(INFO) << "accelerometer configuration complete";
  return true;
}

}  // namespace mems_setup