// Copyright (c) 2014 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "update_engine/policy_manager/real_random_provider.h"
#include "update_engine/policy_manager/real_shill_provider.h"
#include "update_engine/policy_manager/real_state.h"

namespace chromeos_policy_manager {

RealState::RealState() {
  set_random_provider(new RealRandomProvider());
  set_shill_provider(new RealShillProvider());
}

}  // namespace chromeos_policy_manager
