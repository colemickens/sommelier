// Copyright (c) 2013 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef LIBBRILLO_BRILLO_UDEV_UDEV_H_
#define LIBBRILLO_BRILLO_UDEV_UDEV_H_

#include <sys/types.h>

#include <memory>

#include <base/macros.h>
#include <brillo/brillo_export.h>

struct udev;
struct udev_device;

namespace brillo {

class UdevDevice;
class UdevEnumerate;
class UdevMonitor;

// A udev library context, which wraps a udev C struct from libudev and related
// library functions into a C++ object.
class BRILLO_EXPORT Udev {
 public:
  Udev();
  virtual ~Udev();

  // Initializes this object with a udev library context created via udev_new().
  // Returns true on success.
  virtual bool Initialize();

  // Wraps udev_device_new_from_syspath().
  virtual std::unique_ptr<UdevDevice> CreateDeviceFromSysPath(
      const char* sys_path);

  // Wraps udev_device_new_from_devnum().
  virtual std::unique_ptr<UdevDevice> CreateDeviceFromDeviceNumber(
      char type, dev_t device_number);

  // Wraps udev_device_new_from_subsystem_sysname().
  virtual std::unique_ptr<UdevDevice> CreateDeviceFromSubsystemSysName(
      const char* subsystem, const char* sys_name);

  // Wraps udev_enumerate_new().
  virtual std::unique_ptr<UdevEnumerate> CreateEnumerate();

  // Wraps udev_monitor_new_from_netlink().
  virtual std::unique_ptr<UdevMonitor> CreateMonitorFromNetlink(
      const char* name);

 private:
  // Creates a UdevDevice object that wraps a given udev_device struct pointed
  // by |device|. The ownership of |device| is transferred to returned
  // UdevDevice object.
  static std::unique_ptr<UdevDevice> CreateDevice(udev_device* device);

  struct udev* udev_;

  DISALLOW_COPY_AND_ASSIGN(Udev);
};

}  // namespace brillo

#endif  // LIBBRILLO_BRILLO_UDEV_UDEV_H_