//
// Copyright (C) 2015 The Android Open Source Project
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
//

#include "tpm_manager/server/mock_tpm_status.h"

using testing::Invoke;
using testing::Return;
using testing::_;

namespace tpm_manager {

bool GetDefaultDictionaryAttackInfo(uint32_t* counter,
                                    uint32_t* threshold,
                                    bool* lockout,
                                    uint32_t* seconds_remaining) {
  *counter = 0;
  *threshold = 10;
  *lockout = false;
  *seconds_remaining = 0;
  return true;
}

bool GetDefaultVersionInfo(uint32_t* family,
                           uint64_t* spec_level,
                           uint32_t* manufacturer,
                           uint32_t* tpm_model,
                           uint64_t* firmware_version,
                           std::vector<uint8_t>* vendor_specific) {
  *family = 0x312e3200;
  *spec_level = (0ULL << 32) | 138;
  *manufacturer = 0x90091;
  *tpm_model = 0x1234;
  *firmware_version = 0xdeadc0de;
  *vendor_specific = { 0xda, 0x7a };
  return true;
}

MockTpmStatus::MockTpmStatus() {
  ON_CALL(*this, IsTpmEnabled()).WillByDefault(Return(true));
  ON_CALL(*this, CheckAndNotifyIfTpmOwned())
      .WillByDefault(Return(TpmStatus::kTpmOwned));
  ON_CALL(*this, GetDictionaryAttackInfo(_, _, _, _))
      .WillByDefault(Invoke(GetDefaultDictionaryAttackInfo));
  ON_CALL(*this, GetVersionInfo(_, _, _, _, _, _))
      .WillByDefault(Invoke(GetDefaultVersionInfo));
}
MockTpmStatus::~MockTpmStatus() {}

}  // namespace tpm_manager
