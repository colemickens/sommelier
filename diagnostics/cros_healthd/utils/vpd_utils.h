// Copyright 2019 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef DIAGNOSTICS_CROS_HEALTHD_UTILS_VPD_UTILS_H_
#define DIAGNOSTICS_CROS_HEALTHD_UTILS_VPD_UTILS_H_

#include <base/files/file_path.h>
#include <base/macros.h>
#include <chromeos/chromeos-config/libcros_config/cros_config_interface.h>

#include "mojo/cros_healthd_probe.mojom.h"

namespace diagnostics {

class CachedVpdFetcher final {
 public:
  explicit CachedVpdFetcher(brillo::CrosConfigInterface* cros_config);
  ~CachedVpdFetcher();

  chromeos::cros_healthd::mojom::CachedVpdInfoPtr FetchCachedVpdInfo(
      const base::FilePath& root_dir);

 private:
  // Unowned pointer that outlives this CachedVpdFetcher instance.
  brillo::CrosConfigInterface* cros_config_;

  DISALLOW_COPY_AND_ASSIGN(CachedVpdFetcher);
};

}  // namespace diagnostics

#endif  // DIAGNOSTICS_CROS_HEALTHD_UTILS_VPD_UTILS_H_
