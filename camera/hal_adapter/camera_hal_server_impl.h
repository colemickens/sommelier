/*
 * Copyright 2017 The Chromium OS Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

#ifndef CAMERA_HAL_ADAPTER_CAMERA_HAL_SERVER_IMPL_H_
#define CAMERA_HAL_ADAPTER_CAMERA_HAL_SERVER_IMPL_H_

#include <memory>

#include <base/files/file_path.h>
#include <base/files/file_path_watcher.h>
#include <base/single_thread_task_runner.h>
#include <mojo/public/cpp/bindings/binding.h>

#include "hal_adapter/camera_hal_adapter.h"
#include "mojo/cros_camera_service.mojom.h"

namespace cros {

class CameraMojoChannelManager;

// CameraHalServerImpl is the implementation of the CameraHalServer Mojo
// interface.  It hosts the camera HAL v3 adapter and registers itself to the
// CameraHalDispatcher Mojo proxy in started by Chrome.  Camera clients such
// as Chrome VideoCaptureDeviceFactory and Android cameraserver process connect
// to the CameraHalDispatcher to ask for camera service; CameraHalDispatcher
// proxies the service requests to CameraHalServerImpl.
class CameraHalServerImpl final : public mojom::CameraHalServer {
 public:
  CameraHalServerImpl();
  ~CameraHalServerImpl();

  // Initializes the threads and start monitoring the unix domain socket file
  // created by Chrome.
  bool Start();

  // CameraHalServer Mojo interface implementation.  This method runs on
  // |ipc_thread_|.
  void CreateChannel(mojom::CameraModuleRequest camera_module_request) final;

  void SetTracingEnabled(bool enabled);

 private:
  // Callback method for the unix domain socket file change events.  The method
  // will try to establish the Mojo connection to the CameraHalDispatcher
  // started by Chrome.
  void OnSocketFileStatusChange(const base::FilePath& socket_path, bool error);

  // Loads all the camera HAL implementations.
  void LoadCameraHal();

  // Registers with the CameraHalDispatcher Mojo proxy.  After registration the
  // CameraHalDispatcher proxy will call CreateChannel for each connected
  // service clients to create a Mojo channel handle to the HAL adapter.  This
  // method runs on |ipc_thread_|.
  void RegisterCameraHal();

  // Connection error handler for the Mojo connection to CameraHalDispatcher.
  // This method runs on |ipc_thread_|.
  void OnServiceMojoChannelError();

  void ExitOnMainThread(int exit_status);

  // Watches for change events on the unix domain socket file created by Chrome.
  // Upon file change OnScoketFileStatusChange will be called to initiate
  // connection to CameraHalDispatcher.
  base::FilePathWatcher watcher_;

  std::unique_ptr<CameraMojoChannelManager> camera_mojo_channel_manager_;

  // The Mojo IPC task runner.
  scoped_refptr<base::SingleThreadTaskRunner> ipc_task_runner_;

  const scoped_refptr<base::SingleThreadTaskRunner> main_task_runner_;

  // The CameraHalServer implementation binding.  All the function calls to
  // |binding_| runs on |ipc_thread_|.
  mojo::Binding<mojom::CameraHalServer> binding_;

  // The camera HAL adapter instance.  Each call to CreateChannel creates a
  // new Mojo binding in the camera HAL adapter.  Currently the camera HAL
  // adapter serves two clients: Chrome VideoCaptureDeviceFactory and Android
  // cameraserver process.
  std::unique_ptr<CameraHalAdapter> camera_hal_adapter_;

  DISALLOW_COPY_AND_ASSIGN(CameraHalServerImpl);
};

}  // namespace cros

#endif  // CAMERA_HAL_ADAPTER_CAMERA_HAL_SERVER_IMPL_H_
