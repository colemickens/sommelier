// Copyright 2019 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "diagnostics/wilco_dtc_supportd/telemetry/fake_ec_event_service.h"

namespace diagnostics {

FakeEcEventService::FakeEcEventService() = default;
FakeEcEventService::~FakeEcEventService() = default;

void FakeEcEventService::EmitEcEvent(
    const EcEventService::EcEvent& ec_event) const {
  for (auto& observer : observers_) {
    observer.OnEcEvent(ec_event);
  }
}

}  // namespace diagnostics
