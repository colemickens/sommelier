// Copyright 2019 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef DIAGNOSTICS_WILCO_DTC_SUPPORTD_TELEMETRY_BLUETOOTH_EVENT_SERVICE_H_
#define DIAGNOSTICS_WILCO_DTC_SUPPORTD_TELEMETRY_BLUETOOTH_EVENT_SERVICE_H_

#include <cstdint>
#include <string>
#include <vector>

#include <base/observer_list.h>
#include <base/macros.h>

namespace diagnostics {

// BluetoothEventService is used for monitoring objects representing Bluetooth
// Adapters and Devices.
class BluetoothEventService {
 public:
  struct AdapterData {
    std::string name;
    std::string address;
    bool powered = false;
    uint32_t connected_devices_count = 0;

    bool operator==(const AdapterData& data) const;
  };

  // Interface for observing bluetooth adapters and devices changes.
  class Observer {
   public:
    virtual ~Observer() = default;

    virtual void BluetoothAdapterDataChanged(
        const std::vector<AdapterData>& adapters) = 0;
  };

  BluetoothEventService() = default;
  virtual ~BluetoothEventService() = default;

  void AddObserver(Observer* observer);
  void RemoveObserver(Observer* observer);

 protected:
  base::ObserverList<Observer> observers_;

 private:
  DISALLOW_COPY_AND_ASSIGN(BluetoothEventService);
};

}  // namespace diagnostics

#endif  // DIAGNOSTICS_WILCO_DTC_SUPPORTD_TELEMETRY_BLUETOOTH_EVENT_SERVICE_H_
