// Copyright 2018 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SHILL_MOCK_PROFILE_H_
#define SHILL_MOCK_PROFILE_H_

#include <string>

#include <gmock/gmock.h>

#include "shill/profile.h"
#include "shill/wifi/wifi_provider.h"

namespace shill {

class MockProfile : public Profile {
 public:
  explicit MockProfile(Manager* manager);
  MockProfile(Manager* manager, const std::string& identifier);
  ~MockProfile() override;

  MOCK_METHOD(bool, AdoptService, (const ServiceRefPtr&), (override));
  MOCK_METHOD(bool, AbandonService, (const ServiceRefPtr&), (override));
  MOCK_METHOD(bool, LoadService, (const ServiceRefPtr&), (override));
  MOCK_METHOD(bool, ConfigureService, (const ServiceRefPtr&), (override));
  MOCK_METHOD(bool, ConfigureDevice, (const DeviceRefPtr&), (override));
  MOCK_METHOD(void, DeleteEntry, (const std::string&, Error*), (override));
  MOCK_METHOD(RpcIdentifier, GetRpcIdentifier, (), (const, override));
  MOCK_METHOD(bool, UpdateService, (const ServiceRefPtr&), (override));
  MOCK_METHOD(bool, UpdateDevice, (const DeviceRefPtr&), (override));
#if !defined(DISABLE_WIFI)
  MOCK_METHOD(bool, UpdateWiFiProvider, (const WiFiProvider&), (override));
#endif  // DISABLE_WIFI
  MOCK_METHOD(bool, Save, (), (override));
  MOCK_METHOD(StoreInterface*, GetStorage, (), (override));
  MOCK_METHOD(const StoreInterface*, GetConstStorage, (), (const, override));
  MOCK_METHOD(bool, IsDefault, (), (const, override));

 private:
  DISALLOW_COPY_AND_ASSIGN(MockProfile);
};

}  // namespace shill

#endif  // SHILL_MOCK_PROFILE_H_
