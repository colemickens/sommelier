// Copyright 2019 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "debugd/src/helper_utils.h"

#include <gtest/gtest.h>

namespace debugd {

TEST(SandboxedProcessTest, GetHelperPath) {
  std::string full_path;

  EXPECT_TRUE(GetHelperPath("", &full_path));
  EXPECT_EQ("/usr/libexec/debugd/helpers/", full_path);

  EXPECT_TRUE(GetHelperPath("test/me", &full_path));
  EXPECT_EQ("/usr/libexec/debugd/helpers/test/me", full_path);
}

}  // namespace debugd
