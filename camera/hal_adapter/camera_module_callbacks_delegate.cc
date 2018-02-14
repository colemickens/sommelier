/*
 * Copyright 2016 The Chromium OS Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

#include "hal_adapter/camera_module_callbacks_delegate.h"

#include <utility>

#include <base/bind.h>
#include <base/bind_helpers.h>

#include "cros-camera/common.h"

namespace cros {

CameraModuleCallbacksDelegate::CameraModuleCallbacksDelegate(
    scoped_refptr<base::SingleThreadTaskRunner> task_runner)
    : internal::MojoChannel<mojom::CameraModuleCallbacks>(task_runner) {}

void CameraModuleCallbacksDelegate::CameraDeviceStatusChange(int camera_id,
                                                             int new_status) {
  VLOGF_ENTER();
  task_runner_->PostTask(
      FROM_HERE,
      base::Bind(
          &CameraModuleCallbacksDelegate::CameraDeviceStatusChangeOnThread,
          base::AsWeakPtr(this), camera_id, new_status));
}

void CameraModuleCallbacksDelegate::TorchModeStatusChange(int camera_id,
                                                          int new_status) {
  VLOGF_ENTER();
  task_runner_->PostTask(
      FROM_HERE,
      base::Bind(&CameraModuleCallbacksDelegate::TorchModeStatusChangeOnThread,
                 base::AsWeakPtr(this), camera_id, new_status));
}

void CameraModuleCallbacksDelegate::CameraDeviceStatusChangeOnThread(
    int camera_id,
    int new_status) {
  VLOGF_ENTER();
  DCHECK(task_runner_->BelongsToCurrentThread());
  interface_ptr_->CameraDeviceStatusChange(
      camera_id, static_cast<mojom::CameraDeviceStatus>(new_status));
}

void CameraModuleCallbacksDelegate::TorchModeStatusChangeOnThread(
    int camera_id, int new_status) {
  VLOGF_ENTER();
  DCHECK(task_runner_->BelongsToCurrentThread());
  interface_ptr_->TorchModeStatusChange(
      camera_id, static_cast<mojom::TorchModeStatus>(new_status));
}

}  // namespace cros
