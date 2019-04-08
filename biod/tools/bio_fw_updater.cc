// Copyright 2019 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <stdlib.h>

#include <string>

#include <base/command_line.h>
#include <base/logging.h>
#include <brillo/flag_helper.h>
#include <brillo/syslog_logging.h>

#include "biod/cros_fp_device.h"
#include "biod/cros_fp_updater.h"

namespace {

constexpr char kHelpText[] =
    "bio_fw_updater ensures the fingerprint mcu has the latest firmware\n";

void LogFWFileVersion(const biod::CrosFpFirmware& fw) {
  biod::CrosFpFirmware::ImageVersion ver = fw.GetVersion();
  LOG(INFO) << "FWFile RO Version: '" << ver.ro_version << "'";
  LOG(INFO) << "FWFile RW Version: '" << ver.rw_version << "'";
}

void LogFPMCUVersion(const biod::CrosFpDevice::EcVersion& ver) {
  LOG(INFO) << "FPMCU RO Version: '" << ver.ro_version << "'";
  LOG(INFO) << "FPMCU RW Version: '" << ver.rw_version << "'";
  LOG(INFO) << "FPMCU Active Image: "
            << biod::CrosFpDeviceUpdateInterface::EcCurrentImageToString(
                   ver.current_image);
}

}  // namespace

int main(int argc, char* argv[]) {
  brillo::FlagHelper::Init(argc, argv, kHelpText);
  brillo::InitLog(brillo::kLogToSyslog | brillo::kLogHeader |
                  brillo::kLogToStderrIfTty);

  base::CommandLine::StringVector args =
      base::CommandLine::ForCurrentProcess()->GetArgs();

  if (!args.empty()) {
    for (auto a = args.begin(); a != args.end(); a++) {
      LOG(ERROR) << "Invalid argument: '" << *a << "'";
    }
    LOG(ERROR) << "No arguments are expected.";
    LOG(INFO) << "Get help with with the --help flag.";
    return EXIT_FAILURE;
  }

  // Check for firmware disable mechanism
  if (biod::updater::UpdateDisallowed()) {
    LOG(INFO) << "FPMCU update disabled, exiting.";
    return EXIT_SUCCESS;
  }

  // Find a firmware file that matches the firmware file pattern
  base::FilePath file;
  auto status = biod::updater::FindFirmwareFile(
      base::FilePath(biod::updater::kFirmwareDir), &file);

  if (status == biod::updater::FindFirmwareFileStatus::kNoDirectory ||
      status == biod::updater::FindFirmwareFileStatus::kFileNotFound) {
    LOG(INFO) << "No firmware "
              << ((status ==
                   biod::updater::FindFirmwareFileStatus::kNoDirectory)
                      ? "directory"
                      : "file")
              << " on rootfs, exiting.";

    return EXIT_SUCCESS;
  }
  if (status == biod::updater::FindFirmwareFileStatus::kMultipleFiles) {
    LOG(ERROR) << "Found more than one firmware file, aborting.";
    return EXIT_FAILURE;
  }

  biod::CrosFpFirmware fw(file);
  if (!fw.IsValid()) {
    LOG(ERROR) << "Failed to load firmware file '" << fw.GetPath().value()
               << "': " << fw.GetStatusString();
    LOG(ERROR) << "We are aborting update.";
    return EXIT_FAILURE;
  }
  LogFWFileVersion(fw);

  biod::CrosFpDeviceUpdate ec_device;
  biod::CrosFpBootUpdateCtrl boot_ctrl;

  biod::CrosFpDevice::EcVersion ecver;
  if (!ec_device.GetVersion(&ecver)) {
    LOG(INFO) << "Failed to fetch EC version, aborting.";
    return EXIT_FAILURE;
  }
  LogFPMCUVersion(ecver);

  // Run update logic
  if (!biod::updater::DoUpdate(ec_device, boot_ctrl, fw)) {
    LOG(ERROR) << "Failed to update FPMCU firmware.";
    LOG(ERROR) << "We are aborting update.";
    return EXIT_FAILURE;
  }

  // Log the new FPMCU firmware version.
  if (!ec_device.GetVersion(&ecver)) {
    LOG(INFO) << "Failed to fetch final EC version, update failed.";
    return EXIT_FAILURE;
  }
  LogFPMCUVersion(ecver);

  LOG(INFO) << "The update was successful.";
  return EXIT_SUCCESS;
}