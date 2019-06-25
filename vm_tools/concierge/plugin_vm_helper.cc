// Copyright 2019 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "vm_tools/concierge/plugin_vm_helper.h"

#include <sys/mount.h>

#include <algorithm>
#include <string>
#include <utility>
#include <vector>

#include <base/files/file_path.h>
#include <base/files/file_util.h>
#include <base/strings/stringprintf.h>
#include <base/logging.h>
#include <chromeos/scoped_minijail.h>

namespace vm_tools {
namespace concierge {
namespace pvm {
namespace helper {
namespace {

constexpr char kVmHelperCommand[] = "/opt/pita/pvm_helper";

constexpr char kVmHelperPolicyPath[] = "/usr/share/policy/pvm_helper.policy";

constexpr char kDispatcherSocketPath[] = "/run/pvm/vmplugin_dispatcher.socket";

// Minimal set of devices needed by the helpers.
constexpr const char* kDeviceNames[] = {"full", "null", "urandom", "zero"};

ScopedMinijail SetupSandbox(const std::string& policy_file) {
  ScopedMinijail jail(minijail_new());
  if (!jail) {
    LOG(ERROR) << "Unable to create minijail";
    return ScopedMinijail();
  }

  minijail_namespace_pids(jail.get());
  minijail_namespace_user(jail.get());
  minijail_namespace_vfs(jail.get());
  minijail_namespace_ipc(jail.get());
  minijail_namespace_net(jail.get());
  minijail_namespace_cgroups(jail.get());
  minijail_namespace_uts(jail.get());

  std::string uid_map = base::StringPrintf("0 %d 1", geteuid());
  std::string gid_map = base::StringPrintf("0 %d 1", getegid());
  minijail_uidmap(jail.get(), uid_map.c_str());
  minijail_gidmap(jail.get(), gid_map.c_str());

  // Use a seccomp filter.
  minijail_log_seccomp_filter_failures(jail.get());
  minijail_parse_seccomp_filters(jail.get(), policy_file.c_str());
  minijail_use_seccomp_filter(jail.get());

  // We will manage this process's lifetime.
  minijail_run_as_init(jail.get());

  // The helpers do not require any capabilities.
  minijail_no_new_privs(jail.get());
  minijail_use_caps(jail.get(), 0);

  if (minijail_enter_pivot_root(jail.get(), "/mnt/empty") < 0) {
    LOG(ERROR) << "Failed to pivot root to /mnt/empty";
    return ScopedMinijail();
  }

  // Set up minimal set of mounts for the helpers to run.
  if (minijail_mount_with_data(jail.get(), "none", "/", "tmpfs",
                               MS_NOSUID | MS_NODEV | MS_NOEXEC,
                               "size=67108864") < 0) {
    LOG(ERROR) << "Failed to mount root tmpfs";
    return ScopedMinijail();
  }

  if (minijail_bind(jail.get(), "/opt/pita/", "/opt/pita", 0)) {
    LOG(ERROR) << "Failed to bind-mount /opt/pita";
    return ScopedMinijail();
  }

  if (minijail_bind(jail.get(), "/run/pvm", "/run/pvm", 1)) {
    LOG(ERROR) << "Failed to bind-mount /run/pvm";
    return ScopedMinijail();
  }

  // Create a minimal /dev with a very restricted set of device nodes.
  // We can't use minijail_mount_dev() because Chrome OS LSM module
  // does not allow unprivileged users mount filesystems other than
  // tmpfs.
  for (auto dev : kDeviceNames) {
    base::FilePath path(base::FilePath("/dev").Append(dev));
    if (minijail_bind(jail.get(), path.value().c_str(), path.value().c_str(),
                      true /* writeable */) < 0) {
      LOG(ERROR) << "Failed to bind-mount " << path.value();
      return ScopedMinijail();
    }
  }

  // Close all file descriptors we may have.
  minijail_close_open_fds(jail.get());

  return jail;
}

bool ConsumeFileDescriptor(int fd, std::string* contents) {
  base::FilePath path(base::StringPrintf("/proc/self/fd/%d", fd));
  return base::ReadFileToString(path, contents);
}

bool ExecutePvmHelper(const std::string& owner_id,
                      std::vector<std::string> params,
                      std::string* stdout_str = nullptr,
                      std::string* stderr_str = nullptr) {
  ScopedMinijail jail = SetupSandbox(kVmHelperPolicyPath);
  if (!jail)
    return false;

  std::vector<std::string> args;
  args.emplace_back(kVmHelperCommand);
  for (auto& param : params)
    args.emplace_back(std::move(param));
  args.emplace_back("--socket-path");
  args.emplace_back(kDispatcherSocketPath);
  args.emplace_back("--user-identity");
  args.emplace_back(owner_id);

  // Convert args to array of pointers. Must be nullptr terminated.
  std::vector<char*> args_ptr;
  for (const auto& arg : args)
    args_ptr.emplace_back(const_cast<char*>(arg.c_str()));
  args_ptr.emplace_back(nullptr);

  pid_t pid = -1;
  int child_stdout = -1, child_stderr = -1;
  int ret =
      minijail_run_pid_pipes(jail.get(), args_ptr[0], args_ptr.data(), &pid,
                             nullptr, stdout_str ? &child_stdout : nullptr,
                             stderr_str ? &child_stderr : nullptr);
  if (ret != 0) {
    LOG(ERROR) << "failed to execute helper in minijail: " << ret;
    return false;
  }

  if (stdout_str) {
    if (child_stdout >= 0) {
      ConsumeFileDescriptor(child_stdout, stdout_str);
      close(child_stdout);
    }
  }

  if (stderr_str) {
    if (child_stderr >= 0) {
      ConsumeFileDescriptor(child_stderr, stderr_str);
      close(child_stderr);
    }
  }

  // Always call minijail_wait(), otherwise exit code is never
  // queried and the process is left dangling.
  int exit_code = minijail_wait(jail.get());
  jail.reset();

  switch (exit_code) {
    case 0:
      return true;

    case MINIJAIL_ERR_JAIL:
      LOG(ERROR) << "helper failed because seccomp blocked a system call";
      return false;

    default:
      LOG(ERROR) << "helper for '" << args[1]
                 << "' failed with error: " << exit_code;
      return false;
  }
}

}  // namespace

bool CreateVm(const VmId& vm_id, std::vector<std::string> params) {
  std::vector<std::string> args = {
      "create",
      vm_id.name(),
  };
  std::move(params.begin(), params.end(), std::back_inserter(args));
  return ExecutePvmHelper(vm_id.owner_id(), std::move(args));
}

bool AttachIso(const VmId& vm_id, const std::string& iso_name) {
  std::vector<std::string> args = {
      "set",          vm_id.name(),
      "--device-add", "cdrom",
      "--image",      base::FilePath("/iso").Append(iso_name).value(),
      "--connect",
  };
  return ExecutePvmHelper(vm_id.owner_id(), std::move(args));
}

bool DeleteVm(const VmId& vm_id) {
  return ExecutePvmHelper(vm_id.owner_id(), {"delete", vm_id.name()});
}

}  // namespace helper
}  // namespace pvm
}  // namespace concierge
}  // namespace vm_tools