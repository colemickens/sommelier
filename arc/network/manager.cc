// Copyright 2016 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "arc/network/manager.h"

#include <arpa/inet.h>
#include <net/if.h>
#include <netinet/in.h>
#include <stdint.h>
#include <sys/prctl.h>
#include <sys/socket.h>
#include <sys/un.h>

#include <utility>

#include <base/bind.h>
#include <base/logging.h>
#include <base/message_loop/message_loop.h>
#include <base/strings/string_number_conversions.h>
#include <base/strings/string_split.h>
#include <base/strings/string_util.h>
#include <base/strings/stringprintf.h>
#include <brillo/key_value_store.h>
#include <brillo/minijail/minijail.h>

#include "arc/network/ipc.pb.h"

namespace arc_networkd {
namespace {
constexpr int kSubprocessRestartDelayMs = 900;

constexpr char kNDProxyFeatureName[] = "ARC NDProxy";
constexpr int kNDProxyMinAndroidSdkVersion = 28;  // P
constexpr int kNDProxyMinChromeMilestone = 80;

// Passes |method_call| to |handler| and passes the response to
// |response_sender|. If |handler| returns nullptr, an empty response is
// created and sent.
void HandleSynchronousDBusMethodCall(
    base::Callback<std::unique_ptr<dbus::Response>(dbus::MethodCall*)> handler,
    dbus::MethodCall* method_call,
    dbus::ExportedObject::ResponseSender response_sender) {
  std::unique_ptr<dbus::Response> response = handler.Run(method_call);
  if (!response)
    response = dbus::Response::FromMethodCall(method_call);
  response_sender.Run(std::move(response));
}

}  // namespace

Manager::Manager(std::unique_ptr<HelperProcess> adb_proxy,
                 std::unique_ptr<HelperProcess> mcast_proxy,
                 std::unique_ptr<HelperProcess> nd_proxy)
    : adb_proxy_(std::move(adb_proxy)),
      mcast_proxy_(std::move(mcast_proxy)),
      nd_proxy_(std::move(nd_proxy)),
      addr_mgr_({
          AddressManager::Guest::ARC,
          AddressManager::Guest::ARC_NET,
          AddressManager::Guest::CONTAINER,
          AddressManager::Guest::VM_ARC,
          AddressManager::Guest::VM_TERMINA,
      }) {
  runner_ = std::make_unique<MinijailedProcessRunner>();
  datapath_ = std::make_unique<Datapath>(runner_.get());
}

Manager::~Manager() {
  OnShutdown(nullptr);
}

std::map<const std::string, bool> Manager::cached_feature_enabled_ = {};

bool Manager::ShouldEnableFeature(
    int min_android_sdk_version,
    int min_chrome_milestone,
    const std::vector<std::string>& supported_boards,
    const std::string& feature_name) {
  static const char kLsbReleasePath[] = "/etc/lsb-release";

  const auto& cached_result = cached_feature_enabled_.find(feature_name);
  if (cached_result != cached_feature_enabled_.end())
    return cached_result->second;

  auto check = [min_android_sdk_version, min_chrome_milestone,
                &supported_boards, &feature_name]() {
    brillo::KeyValueStore store;
    if (!store.Load(base::FilePath(kLsbReleasePath))) {
      LOG(ERROR) << "Could not read lsb-release";
      return false;
    }

    std::string value;
    if (!store.GetString("CHROMEOS_ARC_ANDROID_SDK_VERSION", &value)) {
      LOG(ERROR) << feature_name
                 << " disabled - cannot determine Android SDK version";
      return false;
    }
    int ver = 0;
    if (!base::StringToInt(value.c_str(), &ver)) {
      LOG(ERROR) << feature_name << " disabled - invalid Android SDK version";
      return false;
    }
    if (ver < min_android_sdk_version) {
      LOG(INFO) << feature_name << " disabled for Android SDK " << value;
      return false;
    }

    if (!store.GetString("CHROMEOS_RELEASE_CHROME_MILESTONE", &value)) {
      LOG(ERROR) << feature_name
                 << " disabled - cannot determine ChromeOS milestone";
      return false;
    }
    if (!base::StringToInt(value.c_str(), &ver)) {
      LOG(ERROR) << feature_name << " disabled - invalid ChromeOS milestone";
      return false;
    }
    if (ver < min_chrome_milestone) {
      LOG(INFO) << feature_name << " disabled for ChromeOS milestone " << value;
      return false;
    }

    if (!store.GetString("CHROMEOS_RELEASE_BOARD", &value)) {
      LOG(ERROR) << feature_name << " disabled - cannot determine board";
      return false;
    }
    if (!supported_boards.empty() &&
        std::find(supported_boards.begin(), supported_boards.end(), value) ==
            supported_boards.end()) {
      LOG(INFO) << feature_name << " disabled for board " << value;
      return false;
    }
    return true;
  };

  bool result = check();
  cached_feature_enabled_.emplace(feature_name, result);
  return result;
}

int Manager::OnInit() {
  prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0);

  // Handle subprocess lifecycle.
  process_reaper_.Register(this);

  CHECK(process_reaper_.WatchForChild(
      FROM_HERE, adb_proxy_->pid(),
      base::Bind(&Manager::OnSubprocessExited, weak_factory_.GetWeakPtr(),
                 adb_proxy_->pid())))
      << "Failed to watch adb-proxy child process";
  CHECK(process_reaper_.WatchForChild(
      FROM_HERE, mcast_proxy_->pid(),
      base::Bind(&Manager::OnSubprocessExited, weak_factory_.GetWeakPtr(),
                 nd_proxy_->pid())))
      << "Failed to watch multicast-proxy child process";
  CHECK(process_reaper_.WatchForChild(
      FROM_HERE, nd_proxy_->pid(),
      base::Bind(&Manager::OnSubprocessExited, weak_factory_.GetWeakPtr(),
                 nd_proxy_->pid())))
      << "Failed to watch nd-proxy child process";

  // Run after Daemon::OnInit().
  base::MessageLoopForIO::current()->task_runner()->PostTask(
      FROM_HERE,
      base::Bind(&Manager::InitialSetup, weak_factory_.GetWeakPtr()));

  return DBusDaemon::OnInit();
}

void Manager::InitialSetup() {
  LOG(INFO) << "Setting up DBus service interface";
  dbus_svc_path_ = bus_->GetExportedObject(
      dbus::ObjectPath(patchpanel::kPatchPanelServicePath));
  if (!dbus_svc_path_) {
    LOG(FATAL) << "Failed to export " << patchpanel::kPatchPanelServicePath
               << " object";
  }

  using ServiceMethod =
      std::unique_ptr<dbus::Response> (Manager::*)(dbus::MethodCall*);
  const std::map<const char*, ServiceMethod> kServiceMethods = {
      {patchpanel::kArcStartupMethod, &Manager::OnArcStartup},
      {patchpanel::kArcShutdownMethod, &Manager::OnArcShutdown},
      {patchpanel::kArcVmStartupMethod, &Manager::OnArcVmStartup},
      {patchpanel::kArcVmShutdownMethod, &Manager::OnArcVmShutdown},
      {patchpanel::kTerminaVmStartupMethod, &Manager::OnTerminaVmStartup},
      {patchpanel::kTerminaVmShutdownMethod, &Manager::OnTerminaVmShutdown},
  };

  for (const auto& kv : kServiceMethods) {
    if (!dbus_svc_path_->ExportMethodAndBlock(
            patchpanel::kPatchPanelInterface, kv.first,
            base::Bind(&HandleSynchronousDBusMethodCall,
                       base::Bind(kv.second, base::Unretained(this))))) {
      LOG(FATAL) << "Failed to export method " << kv.first;
    }
  }

  if (!bus_->RequestOwnershipAndBlock(patchpanel::kPatchPanelServiceName,
                                      dbus::Bus::REQUIRE_PRIMARY)) {
    LOG(FATAL) << "Failed to take ownership of "
               << patchpanel::kPatchPanelServiceName;
  }
  LOG(INFO) << "DBus service interface ready";

  auto& runner = datapath_->runner();
  // Limit local port range: Android owns 47104-61000.
  // TODO(garrick): The original history behind this tweak is gone. Some
  // investigation is needed to see if it is still applicable.
  if (runner.sysctl_w("net.ipv4.ip_local_port_range", "32768 47103") != 0) {
    LOG(ERROR) << "Failed to limit local port range. Some Android features or"
               << " apps may not work correctly.";
  }
  // Enable IPv6 packet forarding
  if (runner.sysctl_w("net.ipv6.conf.all.forwarding", "1") != 0) {
    LOG(ERROR) << "Failed to update net.ipv6.conf.all.forwarding."
               << " IPv6 functionality may be broken.";
  }
  // Kernel proxy_ndp is only needed for legacy IPv6 configuration
  if (!ShouldEnableFeature(kNDProxyMinAndroidSdkVersion,
                           kNDProxyMinChromeMilestone,
                           std::vector<std::string>(), kNDProxyFeatureName) &&
      runner.sysctl_w("net.ipv6.conf.all.proxy_ndp", "1") != 0) {
    LOG(ERROR) << "Failed to update net.ipv6.conf.all.proxy_ndp."
               << " IPv6 functionality may be broken.";
  }

  nd_proxy_->RegisterDeviceMessageHandler(base::Bind(
      &Manager::OnDeviceMessageFromNDProxy, weak_factory_.GetWeakPtr()));

  device_mgr_ = std::make_unique<DeviceManager>(
      std::make_unique<ShillClient>(bus_), &addr_mgr_, datapath_.get(),
      static_cast<TrafficForwarder*>(this));

  arc_svc_ = std::make_unique<ArcService>(device_mgr_.get(), datapath_.get());
  cros_svc_ =
      std::make_unique<CrostiniService>(device_mgr_.get(), datapath_.get());

  nd_proxy_->Listen();
}

void Manager::OnShutdown(int* exit_code) {
  if (!device_mgr_)
    return;

  LOG(INFO) << "Shutting down and cleaning up";
  cros_svc_.reset();
  arc_svc_.reset();
  device_mgr_.reset();

  // Restore original local port range.
  // TODO(garrick): The original history behind this tweak is gone. Some
  // investigation is needed to see if it is still applicable.
  if (datapath_->runner().sysctl_w("net.ipv4.ip_local_port_range",
                                   "32768 61000") != 0) {
    LOG(ERROR) << "Failed to restore local port range";
  }
}

void Manager::OnSubprocessExited(pid_t pid, const siginfo_t&) {
  LOG(ERROR) << "Subprocess " << pid << " exited unexpectedly -"
             << " attempting to restart";

  HelperProcess* proc;
  if (pid == adb_proxy_->pid()) {
    proc = adb_proxy_.get();
  } else if (pid == mcast_proxy_->pid()) {
    proc = mcast_proxy_.get();
  } else if (pid == nd_proxy_->pid()) {
    proc = nd_proxy_.get();
  } else {
    LOG(DFATAL) << "Unknown child process";
    return;
  }

  process_reaper_.ForgetChild(pid);

  base::MessageLoopForIO::current()->task_runner()->PostDelayedTask(
      FROM_HERE,
      base::Bind(&Manager::RestartSubprocess, weak_factory_.GetWeakPtr(), proc),
      base::TimeDelta::FromMilliseconds((2 << proc->restarts()) *
                                        kSubprocessRestartDelayMs));
}

void Manager::RestartSubprocess(HelperProcess* subproc) {
  if (subproc->Restart()) {
    DCHECK(process_reaper_.WatchForChild(
        FROM_HERE, subproc->pid(),
        base::Bind(&Manager::OnSubprocessExited, weak_factory_.GetWeakPtr(),
                   subproc->pid())))
        << "Failed to watch child process " << subproc->pid();
  }
}

bool Manager::StartArc(pid_t pid) {
  if (!arc_svc_->Start(pid))
    return false;

  GuestMessage msg;
  msg.set_event(GuestMessage::START);
  msg.set_type(GuestMessage::ARC);
  msg.set_arc_pid(pid);
  SendGuestMessage(msg);

  return true;
}

void Manager::StopArc(pid_t pid) {
  GuestMessage msg;
  msg.set_event(GuestMessage::STOP);
  msg.set_type(GuestMessage::ARC);
  SendGuestMessage(msg);

  arc_svc_->Stop(pid);
}

bool Manager::StartArcVm(int32_t cid) {
  if (!arc_svc_->Start(cid))
    return false;

  GuestMessage msg;
  msg.set_event(GuestMessage::START);
  msg.set_type(GuestMessage::ARC_VM);
  msg.set_arcvm_vsock_cid(cid);
  SendGuestMessage(msg);

  return true;
}

void Manager::StopArcVm(int32_t cid) {
  GuestMessage msg;
  msg.set_event(GuestMessage::STOP);
  msg.set_type(GuestMessage::ARC_VM);
  SendGuestMessage(msg);

  arc_svc_->Stop(cid);
}

bool Manager::StartTerminaVm(int32_t cid) {
  if (!cros_svc_->Start(cid))
    return false;

  GuestMessage msg;
  msg.set_event(GuestMessage::START);
  msg.set_type(GuestMessage::TERMINA_VM);
  msg.set_arcvm_vsock_cid(cid);
  SendGuestMessage(msg);

  return true;
}

void Manager::StopTerminaVm(int32_t cid) {
  GuestMessage msg;
  msg.set_event(GuestMessage::STOP);
  msg.set_type(GuestMessage::TERMINA_VM);
  SendGuestMessage(msg);

  cros_svc_->Stop(cid);
}

std::unique_ptr<dbus::Response> Manager::OnArcStartup(
    dbus::MethodCall* method_call) {
  LOG(INFO) << "ARC++ starting up";

  std::unique_ptr<dbus::Response> dbus_response(
      dbus::Response::FromMethodCall(method_call));

  dbus::MessageReader reader(method_call);
  dbus::MessageWriter writer(dbus_response.get());

  patchpanel::ArcStartupRequest request;
  patchpanel::ArcStartupResponse response;

  if (!reader.PopArrayOfBytesAsProto(&request)) {
    LOG(ERROR) << "Unable to parse request";
    writer.AppendProtoAsArrayOfBytes(response);
    return dbus_response;
  }

  if (!StartArc(request.pid()))
    LOG(ERROR) << "Failed to start ARC++ network service";

  writer.AppendProtoAsArrayOfBytes(response);
  return dbus_response;
}

std::unique_ptr<dbus::Response> Manager::OnArcShutdown(
    dbus::MethodCall* method_call) {
  LOG(INFO) << "ARC++ shutting down";

  std::unique_ptr<dbus::Response> dbus_response(
      dbus::Response::FromMethodCall(method_call));

  dbus::MessageReader reader(method_call);
  dbus::MessageWriter writer(dbus_response.get());

  patchpanel::ArcShutdownRequest request;
  patchpanel::ArcShutdownResponse response;

  if (!reader.PopArrayOfBytesAsProto(&request)) {
    LOG(ERROR) << "Unable to parse request";
    writer.AppendProtoAsArrayOfBytes(response);
    return dbus_response;
  }

  StopArc(request.pid());

  writer.AppendProtoAsArrayOfBytes(response);
  return dbus_response;
}

std::unique_ptr<dbus::Response> Manager::OnArcVmStartup(
    dbus::MethodCall* method_call) {
  LOG(INFO) << "ARCVM starting up";

  std::unique_ptr<dbus::Response> dbus_response(
      dbus::Response::FromMethodCall(method_call));

  dbus::MessageReader reader(method_call);
  dbus::MessageWriter writer(dbus_response.get());

  patchpanel::ArcVmStartupRequest request;
  patchpanel::ArcVmStartupResponse response;

  if (!reader.PopArrayOfBytesAsProto(&request)) {
    LOG(ERROR) << "Unable to parse request";
    writer.AppendProtoAsArrayOfBytes(response);
    return dbus_response;
  }

  if (!StartArcVm(request.cid())) {
    LOG(ERROR) << "Failed to start ARCVM network service";
    writer.AppendProtoAsArrayOfBytes(response);
    return dbus_response;
  }

  // Populate the response with the known devices.
  auto build_resp = [](patchpanel::ArcVmStartupResponse* resp, Device* device) {
    auto* ctx = dynamic_cast<ArcService::Context*>(device->context());
    if (!ctx || ctx->TAP().empty())
      return;

    const auto& config = device->config();
    auto* dev = resp->add_devices();
    dev->set_ifname(ctx->TAP());
    dev->set_ipv4_addr(config.guest_ipv4_addr());
  };

  device_mgr_->ProcessDevices(
      base::Bind(build_resp, base::Unretained(&response)));

  writer.AppendProtoAsArrayOfBytes(response);
  return dbus_response;
}

std::unique_ptr<dbus::Response> Manager::OnArcVmShutdown(
    dbus::MethodCall* method_call) {
  LOG(INFO) << "ARCVM shutting down";

  std::unique_ptr<dbus::Response> dbus_response(
      dbus::Response::FromMethodCall(method_call));

  dbus::MessageReader reader(method_call);
  dbus::MessageWriter writer(dbus_response.get());

  patchpanel::ArcVmShutdownRequest request;
  patchpanel::ArcVmShutdownResponse response;

  if (!reader.PopArrayOfBytesAsProto(&request)) {
    LOG(ERROR) << "Unable to parse request";
    writer.AppendProtoAsArrayOfBytes(response);
    return dbus_response;
  }

  StopArcVm(request.cid());

  writer.AppendProtoAsArrayOfBytes(response);
  return dbus_response;
}

std::unique_ptr<dbus::Response> Manager::OnTerminaVmStartup(
    dbus::MethodCall* method_call) {
  LOG(INFO) << "Termina VM starting up";

  std::unique_ptr<dbus::Response> dbus_response(
      dbus::Response::FromMethodCall(method_call));

  dbus::MessageReader reader(method_call);
  dbus::MessageWriter writer(dbus_response.get());

  patchpanel::TerminaVmStartupRequest request;
  patchpanel::TerminaVmStartupResponse response;

  if (!reader.PopArrayOfBytesAsProto(&request)) {
    LOG(ERROR) << "Unable to parse request";
    writer.AppendProtoAsArrayOfBytes(response);
    return dbus_response;
  }

  const int32_t cid = request.cid();
  if (!StartTerminaVm(cid)) {
    LOG(ERROR) << "Failed to start Termina VM network service";
    writer.AppendProtoAsArrayOfBytes(response);
    return dbus_response;
  }

  const auto* const tap = cros_svc_->TAP(cid);
  if (!tap) {
    LOG(DFATAL) << "TAP device missing";
    writer.AppendProtoAsArrayOfBytes(response);
    return dbus_response;
  }

  const auto& config = tap->config();
  auto* dev = response.mutable_device();
  dev->set_ifname(config.host_ifname());
  const auto* subnet = config.ipv4_subnet();
  if (!subnet) {
    LOG(DFATAL) << "Missing required subnet for {cid: " << cid << "}";
    writer.AppendProtoAsArrayOfBytes(response);
    return dbus_response;
  }
  auto* resp_subnet = dev->mutable_ipv4_subnet();
  resp_subnet->set_base_addr(subnet->BaseAddress());
  resp_subnet->set_prefix_len(subnet->PrefixLength());
  subnet = config.lxd_ipv4_subnet();
  if (!subnet) {
    LOG(DFATAL) << "Missing required lxd subnet for {cid: " << cid << "}";
    writer.AppendProtoAsArrayOfBytes(response);
    return dbus_response;
  }
  resp_subnet = response.mutable_container_subnet();
  resp_subnet->set_base_addr(subnet->BaseAddress());
  resp_subnet->set_prefix_len(subnet->PrefixLength());

  writer.AppendProtoAsArrayOfBytes(response);
  return dbus_response;
}

std::unique_ptr<dbus::Response> Manager::OnTerminaVmShutdown(
    dbus::MethodCall* method_call) {
  LOG(INFO) << "Termina VM shutting down";

  std::unique_ptr<dbus::Response> dbus_response(
      dbus::Response::FromMethodCall(method_call));

  dbus::MessageReader reader(method_call);
  dbus::MessageWriter writer(dbus_response.get());

  patchpanel::TerminaVmShutdownRequest request;
  patchpanel::TerminaVmShutdownResponse response;

  if (!reader.PopArrayOfBytesAsProto(&request)) {
    LOG(ERROR) << "Unable to parse request";
    writer.AppendProtoAsArrayOfBytes(response);
    return dbus_response;
  }

  StopTerminaVm(request.cid());

  writer.AppendProtoAsArrayOfBytes(response);
  return dbus_response;
}

void Manager::SendGuestMessage(const GuestMessage& msg) {
  IpHelperMessage ipm;
  *ipm.mutable_guest_message() = msg;
  adb_proxy_->SendMessage(ipm);
  mcast_proxy_->SendMessage(ipm);
  nd_proxy_->SendMessage(ipm);
}

void Manager::StartForwarding(const std::string& ifname_physical,
                              const std::string& ifname_virtual,
                              uint32_t ipv4_addr_virtual,
                              bool ipv6,
                              bool multicast) {
  if (ifname_physical.empty())
    return;

  IpHelperMessage ipm;
  DeviceMessage* msg = ipm.mutable_device_message();
  msg->set_dev_ifname(ifname_physical);
  msg->set_guest_ip4addr(ipv4_addr_virtual);
  msg->set_br_ifname(ifname_virtual);

  if (ipv6) {
    LOG(INFO) << "Starting IPv6 forwarding from " << ifname_physical << " to "
              << ifname_virtual;

    if (!datapath_->AddIPv6Forwarding(ifname_physical, ifname_virtual)) {
      LOG(ERROR) << "Failed to setup iptables forwarding rule for IPv6 from "
                 << ifname_physical << " to " << ifname_virtual;
    }
    if (!datapath_->MaskInterfaceFlags(ifname_physical, IFF_ALLMULTI)) {
      LOG(WARNING) << "Failed to setup all multicast mode for interface "
                   << ifname_physical;
    }
    if (!datapath_->MaskInterfaceFlags(ifname_virtual, IFF_ALLMULTI)) {
      LOG(WARNING) << "Failed to setup all multicast mode for interface "
                   << ifname_virtual;
    }
    nd_proxy_->SendMessage(ipm);
  }

  if (multicast) {
    LOG(INFO) << "Starting multicast forwarding from " << ifname_physical
              << " to " << ifname_virtual;
    mcast_proxy_->SendMessage(ipm);
  }
}

void Manager::StopForwarding(const std::string& ifname_physical,
                             const std::string& ifname_virtual,
                             bool ipv6,
                             bool multicast) {
  if (ifname_physical.empty())
    return;

  IpHelperMessage ipm;
  DeviceMessage* msg = ipm.mutable_device_message();
  msg->set_dev_ifname(ifname_physical);
  msg->set_teardown(true);

  if (ipv6) {
    LOG(INFO) << "Stopping IPv6 forwarding from " << ifname_physical << " to "
              << ifname_virtual;

    datapath_->RemoveIPv6Forwarding(ifname_physical, ifname_virtual);
    nd_proxy_->SendMessage(ipm);
  }

  if (multicast) {
    LOG(INFO) << "Stopping multicast forwarding from " << ifname_physical
              << " to " << ifname_virtual;
    mcast_proxy_->SendMessage(ipm);
  }
}

bool Manager::ForwardsLegacyIPv6() const {
  return !ShouldEnableFeature(kNDProxyMinAndroidSdkVersion,
                              kNDProxyMinChromeMilestone,
                              std::vector<std::string>(), kNDProxyFeatureName);
}

void Manager::OnDeviceMessageFromNDProxy(const DeviceMessage& msg) {
  LOG_IF(DFATAL, msg.dev_ifname().empty())
      << "Received DeviceMessage w/ empty dev_ifname";

  if (!datapath_->AddIPv6HostRoute(msg.dev_ifname(), msg.guest_ip6addr(),
                                   128)) {
    LOG(WARNING) << "Failed to setup the IPv6 route for interface "
                 << msg.dev_ifname();
  }
}

}  // namespace arc_networkd
