// Copyright 2018 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "shill/dbus/rpc_task_dbus_adaptor.h"

#include "shill/error.h"
#include "shill/logging.h"
#include "shill/rpc_task.h"

using std::map;
using std::string;

namespace shill {

namespace Logging {
static auto kModuleLogScope = ScopeLogger::kDBus;
static string ObjectID(RpcTaskDBusAdaptor* r) {
  return r->GetRpcIdentifier();
}
}  // namespace Logging

// static
const char RpcTaskDBusAdaptor::kPath[] = "/task/";

RpcTaskDBusAdaptor::RpcTaskDBusAdaptor(const scoped_refptr<dbus::Bus>& bus,
                                       RpcTask* task)
    : org::chromium::flimflam::TaskAdaptor(this),
      DBusAdaptor(bus, kPath + task->UniqueName()),
      task_(task),
      connection_name_(bus->GetConnectionName()) {
  // Register DBus object.
  RegisterWithDBusObject(dbus_object());
  dbus_object()->RegisterAndBlock();
}

RpcTaskDBusAdaptor::~RpcTaskDBusAdaptor() {
  dbus_object()->UnregisterAsync();
  task_ = nullptr;
}

RpcIdentifier RpcTaskDBusAdaptor::GetRpcIdentifier() const {
  return RpcIdentifier(dbus_path().value());
}

RpcIdentifier RpcTaskDBusAdaptor::GetRpcConnectionIdentifier() const {
  return RpcIdentifier(connection_name_);
}

bool RpcTaskDBusAdaptor::getsec(brillo::ErrorPtr* /*error*/,
                                string* user,
                                string* password) {
  SLOG(this, 2) << __func__ << ": " << user;
  task_->GetLogin(user, password);
  return true;
}

bool RpcTaskDBusAdaptor::notify(brillo::ErrorPtr* /*error*/,
                                const string& reason,
                                const map<string, string>& dict) {
  SLOG(this, 2) << __func__ << ": " << reason;
  task_->Notify(reason, dict);
  return true;
}

}  // namespace shill
