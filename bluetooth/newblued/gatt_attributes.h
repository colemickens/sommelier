// Copyright 2019 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef BLUETOOTH_NEWBLUED_GATT_ATTRIBUTES_H_
#define BLUETOOTH_NEWBLUED_GATT_ATTRIBUTES_H_

#include <list>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include <base/macros.h>
#include <gtest/gtest_prod.h>

#include "bluetooth/common/uuid.h"

namespace bluetooth {

class GattIncludedService;
class GattCharacteristic;
class GattDescriptor;

// Represents a GATT primary/secondary service.
class GattService {
 public:
  GattService(const std::string& device_address,
              uint16_t first_handle,
              uint16_t last_handle,
              bool primary,
              const Uuid& uuid);
  GattService(uint16_t first_handle,
              uint16_t last_handle,
              bool primary,
              const Uuid& uuid);
  virtual ~GattService() {}

  // Sets the owner device address. |device_address| cannot be empty.
  void SetDeviceAddress(const std::string& device_address);

  // Adds included service to the service.
  void AddIncludedService(
      std::unique_ptr<GattIncludedService> included_service);

  // Adds characteristic to the service.
  void AddCharacteristic(std::unique_ptr<GattCharacteristic> characteristic);

  // Indicates whether there is a device address associated with it.
  bool HasOwner() const;

  const std::string& device_address() const { return device_address_; }
  uint16_t first_handle() const { return first_handle_; }
  uint16_t last_handle() const { return last_handle_; }
  bool primary() const { return primary_; }
  const Uuid& uuid() const { return uuid_; }

 private:
  std::string device_address_;
  uint16_t first_handle_;
  uint16_t last_handle_;
  bool primary_;
  Uuid uuid_;
  std::map<uint16_t, std::unique_ptr<GattCharacteristic>> characteristics_;
  std::map<uint16_t, std::unique_ptr<GattIncludedService>> included_services_;

  FRIEND_TEST(GattAttributesTest, GattServiceAddIncludedServiceCharacteristic);
  FRIEND_TEST(UtilTest, ConvertToGattService);

  DISALLOW_COPY_AND_ASSIGN(GattService);
};

// Represents a GATT included service.
class GattIncludedService {
 public:
  GattIncludedService(GattService* service,
                      uint16_t included_handle,
                      uint16_t first_handle,
                      uint16_t last_handle,
                      const Uuid& uuid);
  virtual ~GattIncludedService() {}

  const GattService* service() const { return service_; }
  uint16_t included_handle() const { return included_handle_; }
  uint16_t first_handle() const { return first_handle_; }
  uint16_t last_handle() const { return last_handle_; }
  const Uuid& uuid() const { return uuid_; }

 private:
  GattService* service_;

  uint16_t included_handle_;
  uint16_t first_handle_;
  uint16_t last_handle_;
  Uuid uuid_;

  DISALLOW_COPY_AND_ASSIGN(GattIncludedService);
};

// Represents a GATT characteristic.
class GattCharacteristic {
 public:
  enum class NotifySetting : uint8_t {
    NONE,
    NOTIFICATION,
    INDICATION,
  };

  GattCharacteristic(GattService* service,
                     uint16_t value_handle,
                     uint16_t first_handle,
                     uint16_t last_handle,
                     uint8_t properties,
                     const Uuid& uuid);
  virtual ~GattCharacteristic() {}

  // Adds descriptor to the characteristic.
  void AddDescriptor(std::unique_ptr<GattDescriptor> descriptor);

  const GattService* service() const { return service_; }
  uint16_t value_handle() const { return value_handle_; }
  uint16_t first_handle() const { return first_handle_; }
  uint16_t last_handle() const { return last_handle_; }
  uint8_t properties() const { return properties_; }
  const Uuid& uuid() const { return uuid_; }
  const std::vector<uint8_t>& value() const { return value_; }
  NotifySetting notify_setting() const { return notify_setting_; }

 private:
  GattService* service_;

  uint16_t value_handle_;
  uint16_t first_handle_;
  uint16_t last_handle_;
  uint8_t properties_;
  Uuid uuid_;
  std::vector<uint8_t> value_;
  std::map<uint16_t, std::unique_ptr<GattDescriptor>> descriptors_;

  NotifySetting notify_setting_;

  FRIEND_TEST(GattAttributesTest, GattCharacteristicAddDescriptor);
  FRIEND_TEST(UtilTest, ConvertToGattService);

  DISALLOW_COPY_AND_ASSIGN(GattCharacteristic);
};

// Represents a GATT descriptor.
class GattDescriptor {
 public:
  GattDescriptor(GattCharacteristic* characteristic,
                 uint16_t handle,
                 const Uuid& uuid);
  virtual ~GattDescriptor() {}

  const GattCharacteristic* characteristic() const { return characteristic_; }
  uint16_t handle() const { return handle_; }
  const Uuid& uuid() const { return uuid_; }
  const std::vector<uint8_t>& value() const { return value_; }

 private:
  GattCharacteristic* characteristic_;

  uint16_t handle_;
  Uuid uuid_;
  std::vector<uint8_t> value_;

  DISALLOW_COPY_AND_ASSIGN(GattDescriptor);
};

}  // namespace bluetooth

#endif  // BLUETOOTH_NEWBLUED_GATT_ATTRIBUTES_H_
