// Copyright (c) 2013 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "shill/cellular_error.h"

#include <string>

#include <ModemManager/ModemManager.h>

// TODO(armansito): Once we refactor the code to handle the ModemManager D-Bus
// bindings in a dedicated class, this code should move there.
// (See crbug.com/246425)

using std::string;

namespace shill {

namespace {

const char *kErrorGprsNotSubscribed =
    MM_MOBILE_EQUIPMENT_ERROR_DBUS_PREFIX ".GprsServiceOptionNotSubscribed";

const char *kErrorIncorrectPassword =
    MM_MOBILE_EQUIPMENT_ERROR_DBUS_PREFIX ".IncorrectPassword";

const char *kErrorSimPin =
    MM_MOBILE_EQUIPMENT_ERROR_DBUS_PREFIX ".SimPin";

const char *kErrorSimPuk =
    MM_MOBILE_EQUIPMENT_ERROR_DBUS_PREFIX ".SimPuk";

}  // namespace

// static
void CellularError::FromMM1DBusError(const DBus::Error &dbus_error,
                                     Error *error) {
  if (!error)
    return;

  if (!dbus_error.is_set()) {
    error->Reset();
    return;
  }

  string name(dbus_error.name());
  const char *msg = dbus_error.message();
  Error::Type type;

  if (name == kErrorIncorrectPassword)
    type = Error::kIncorrectPin;
  else if (name == kErrorSimPin)
    type = Error::kPinRequired;
  else if (name == kErrorSimPuk)
    type = Error::kPinBlocked;
  else if (name == kErrorGprsNotSubscribed)
    type = Error::kInvalidApn;
  else
    type = Error::kOperationFailed;

  if (msg)
    return error->Populate(type, msg);
  else
    return error->Populate(type);
}

}  // namespace shill
