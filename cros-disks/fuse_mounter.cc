// Copyright (c) 2013 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "cros-disks/fuse_mounter.h"

// Has to come before linux/fs.h due to conflicting definitions of MS_*
// constants.
#include <sys/mount.h>

#include <fcntl.h>
#include <linux/capability.h>
#include <linux/fs.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <memory>
#include <string>
#include <utility>

#include <base/macros.h>
#include <base/bind.h>
#include <base/callback_helpers.h>
#include <base/files/file.h>
#include <base/logging.h>
#include <base/memory/weak_ptr.h>
#include <base/strings/stringprintf.h>
#include <base/strings/string_util.h>
#include <brillo/process_reaper.h>

#include "cros-disks/error_logger.h"
#include "cros-disks/mount_point.h"
#include "cros-disks/platform.h"
#include "cros-disks/quote.h"
#include "cros-disks/sandboxed_process.h"

namespace cros_disks {

namespace {

const mode_t kSourcePathPermissions = S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP;

const char kFuseDeviceFile[] = "/dev/fuse";
const MountOptions::Flags kRequiredFuseMountFlags =
    MS_NODEV | MS_NOEXEC | MS_NOSUID;

class FUSEMountPoint : public MountPoint {
 public:
  FUSEMountPoint(const base::FilePath& path, const Platform* platform)
      : MountPoint(path), platform_(platform) {}

  ~FUSEMountPoint() override { DestructorUnmount(); }

  base::WeakPtr<FUSEMountPoint> GetWeakPtr() {
    return weak_factory_.GetWeakPtr();
  }

 private:
  // MountPoint overrides:
  MountErrorType UnmountImpl() override {
    // We take a 2-step approach to unmounting FUSE filesystems. First, try a
    // normal unmount. This lets the VFS flush any pending data and lets the
    // filesystem shut down cleanly. If the filesystem is busy, force unmount
    // the filesystem. This is done because there is no good recovery path the
    // user can take, and these filesystem are sometimes unmounted implicitly on
    // login/logout/suspend. This action is similar to native filesystems (i.e.
    // FAT32, ext2/3/4, etc) which are lazy unmounted if a regular unmount fails
    // because the filesystem is busy.

    MountErrorType error = platform_->Unmount(path().value(), 0 /* flags */);
    if (error != MOUNT_ERROR_PATH_ALREADY_MOUNTED) {
      // MOUNT_ERROR_PATH_ALREADY_MOUNTED is returned on EBUSY.
      return error;
    }

    // For FUSE filesystems, MNT_FORCE will cause the kernel driver to
    // immediately close the channel to the user-space driver program and cancel
    // all outstanding requests. However, if any program is still accessing the
    // filesystem, the umount2() will fail with EBUSY and the mountpoint will
    // still be attached. Since the mountpoint is no longer valid, use
    // MNT_DETACH to also force the mountpoint to be disconnected.
    LOG(WARNING) << "Mount point " << quote(path())
                 << " is busy, using force unmount";
    return platform_->Unmount(path().value(), MNT_FORCE | MNT_DETACH);
  }

  const Platform* platform_;

  base::WeakPtrFactory<FUSEMountPoint> weak_factory_{this};

  DISALLOW_IMPLICIT_CONSTRUCTORS(FUSEMountPoint);
};

void CleanUpCallback(base::OnceClosure cleanup,
                     const base::FilePath& mount_path,
                     const siginfo_t& info) {
  CHECK_EQ(SIGCHLD, info.si_signo);
  if (info.si_code != CLD_EXITED || info.si_status != 0) {
    LOG(WARNING) << "FUSE daemon for " << quote(mount_path)
                 << " exited with code " << info.si_code << " and status "
                 << info.si_status;
  } else {
    LOG(INFO) << "FUSE daemon for " << quote(mount_path) << " exited normally";
  }
  std::move(cleanup).Run();
}

MountErrorType ConfigureCommonSandbox(SandboxedProcess* sandbox,
                                      const Platform* platform,
                                      bool network_ns,
                                      const base::FilePath& seccomp) {
  sandbox->SetCapabilities(0);
  sandbox->SetNoNewPrivileges();

  // The FUSE mount program is put under a new mount namespace, so mounts
  // inside that namespace don't normally propagate.
  sandbox->NewMountNamespace();

  // TODO(crbug.com/707327): Remove this when we get rid of AVFS.
  sandbox->SkipRemountPrivate();

  // TODO(benchan): Re-enable cgroup namespace when either Chrome OS
  // kernel 3.8 supports it or no more supported devices use kernel
  // 3.8.
  // mount_process.NewCgroupNamespace();

  sandbox->NewIpcNamespace();

  sandbox->NewPidNamespace();

  if (network_ns) {
    sandbox->NewNetworkNamespace();
  }

  if (!seccomp.empty()) {
    if (!platform->PathExists(seccomp.value())) {
      LOG(ERROR) << "Seccomp policy " << quote(seccomp) << " is missing";
      return MOUNT_ERROR_INTERNAL;
    }
    sandbox->LoadSeccompFilterPolicy(seccomp.value());
  }

  // Prepare mounts for pivot_root.
  if (!sandbox->SetUpMinimalMounts()) {
    LOG(ERROR) << "Can't set up minijail mounts";
    return MOUNT_ERROR_INTERNAL;
  }

  // Data dirs if any are mounted inside /run/fuse.
  if (!sandbox->Mount("tmpfs", "/run", "tmpfs", "mode=0755,size=10M")) {
    LOG(ERROR) << "Can't mount /run";
    return MOUNT_ERROR_INTERNAL;
  }
  if (!sandbox->BindMount("/run/fuse", "/run/fuse", false, false)) {
    LOG(ERROR) << "Can't bind /run/fuse";
    return MOUNT_ERROR_INTERNAL;
  }

  if (!network_ns) {
    // Network DNS configs are in /run/shill.
    if (!sandbox->BindMount("/run/shill", "/run/shill", false, false)) {
      LOG(ERROR) << "Can't bind /run/shill";
      return MOUNT_ERROR_INTERNAL;
    }
    // Hardcoded hosts are mounted into /etc/hosts.d when Crostini is enabled.
    if (platform->PathExists("/etc/hosts.d") &&
        !sandbox->BindMount("/etc/hosts.d", "/etc/hosts.d", false, false)) {
      LOG(ERROR) << "Can't bind /etc/hosts.d";
      return MOUNT_ERROR_INTERNAL;
    }
  }
  if (!sandbox->EnterPivotRoot()) {
    LOG(ERROR) << "Can't pivot root";
    return MOUNT_ERROR_INTERNAL;
  }

  return MOUNT_ERROR_NONE;
}

bool GetPhysicalBlockSize(const std::string& source, int* size) {
  base::ScopedFD fd(open(source.c_str(), O_RDONLY | O_CLOEXEC));

  *size = 0;
  if (!fd.is_valid()) {
    PLOG(WARNING) << "Couldn't open " << source;
    return false;
  }

  if (ioctl(fd.get(), BLKPBSZGET, size) < 0) {
    PLOG(WARNING) << "Failed to get block size for" << source;
    return false;
  }

  return true;
}

MountErrorType MountFuseDevice(const Platform* platform,
                               const std::string& source,
                               const std::string& filesystem_type,
                               const base::FilePath& target,
                               const base::File& fuse_file,
                               uid_t mount_user_id,
                               gid_t mount_group_id,
                               const MountOptions& options) {
  // Mount options for FUSE:
  // fd - File descriptor for /dev/fuse.
  // user_id/group_id - user/group for file access control. Essentially
  //     bypassed due to allow_other, but still required to be set.
  // allow_other - Allows users other than user_id/group_id to access files
  //     on the file system. By default, FUSE prevents any process other than
  //     ones running under user_id/group_id to access files, regardless of
  //     the file's permissions.
  // default_permissions - Enforce permission checking.
  // rootmode - Mode bits for the root inode.
  std::string fuse_mount_options = base::StringPrintf(
      "fd=%d,user_id=%u,group_id=%u,allow_other,default_permissions,"
      "rootmode=%o",
      fuse_file.GetPlatformFile(), mount_user_id, mount_group_id, S_IFDIR);

  // "nosymfollow" is a special mount option that's passed to the Chromium LSM
  // and not forwarded to the FUSE driver. If it's set, add it as a mount
  // option.
  if (options.HasOption(MountOptions::kOptionNoSymFollow)) {
    fuse_mount_options.append(",");
    fuse_mount_options.append(MountOptions::kOptionNoSymFollow);
  }

  std::string fuse_type = "fuse";
  struct stat statbuf = {0};
  if (stat(source.c_str(), &statbuf) == 0 && S_ISBLK(statbuf.st_mode)) {
    int blksize = 0;

    // TODO(crbug.com/931500): It's possible that specifying a block size equal
    // to the file system cluster size (which might be larger than the physical
    // block size) might be more efficient. Data would be needed to see what
    // kind of performance benefit, if any, could be gained. At the very least,
    // specify the block size of the underlying device. Without this, UFS cards
    // with 4k sector size will fail to mount.
    if (GetPhysicalBlockSize(source, &blksize) && blksize > 0)
      fuse_mount_options.append(base::StringPrintf(",blksize=%d", blksize));

    LOG(INFO) << "Source file " << quote(source)
              << " is a block device with block size " << blksize;

    fuse_type = "fuseblk";
  }
  if (!filesystem_type.empty()) {
    fuse_type += ".";
    fuse_type += filesystem_type;
  }

  auto flags = options.ToMountFlagsAndData().first;

  return platform->Mount(source.empty() ? filesystem_type : source,
                         target.value(), fuse_type,
                         flags | kRequiredFuseMountFlags, fuse_mount_options);
}

}  // namespace

FUSEMounter::FUSEMounter(const std::string& filesystem_type,
                         const MountOptions& mount_options,
                         const Platform* platform,
                         brillo::ProcessReaper* process_reaper,
                         const std::string& mount_program_path,
                         const std::string& mount_user,
                         const std::string& seccomp_policy,
                         const std::vector<BindPath>& accessible_paths,
                         bool permit_network_access,
                         const std::string& mount_group)
    : MounterCompat(filesystem_type, mount_options),
      platform_(platform),
      process_reaper_(process_reaper),
      mount_program_path_(mount_program_path),
      mount_user_(mount_user),
      mount_group_(mount_group),
      seccomp_policy_(seccomp_policy),
      accessible_paths_(accessible_paths),
      permit_network_access_(permit_network_access) {}

std::unique_ptr<MountPoint> FUSEMounter::Mount(
    const std::string& source,
    const base::FilePath& target_path,
    std::vector<std::string> options,
    MountErrorType* error) const {
  auto mount_process = CreateSandboxedProcess();
  *error = ConfigureCommonSandbox(mount_process.get(), platform_,
                                  !permit_network_access_,
                                  base::FilePath(seccomp_policy_));
  if (*error != MOUNT_ERROR_NONE) {
    return nullptr;
  }

  uid_t mount_user_id;
  gid_t mount_group_id;
  if (!platform_->GetUserAndGroupId(mount_user_, &mount_user_id,
                                    &mount_group_id)) {
    LOG(ERROR) << "Cannot resolve user " << quote(mount_user_);
    *error = MOUNT_ERROR_INTERNAL;
    return nullptr;
  }
  if (!mount_group_.empty() &&
      !platform_->GetGroupId(mount_group_, &mount_group_id)) {
    *error = MOUNT_ERROR_INTERNAL;
    return nullptr;
  }

  mount_process->SetUserId(mount_user_id);
  mount_process->SetGroupId(mount_group_id);

  if (!platform_->PathExists(mount_program_path_)) {
    LOG(ERROR) << "Cannot find mount program " << quote(mount_program_path_);
    *error = MOUNT_ERROR_MOUNT_PROGRAM_NOT_FOUND;
    return nullptr;
  }
  mount_process->AddArgument(mount_program_path_);

  const base::File fuse_file = base::File(
      base::FilePath(kFuseDeviceFile),
      base::File::FLAG_OPEN | base::File::FLAG_READ | base::File::FLAG_WRITE);
  if (!fuse_file.IsValid()) {
    LOG(ERROR) << "Unable to open FUSE device file. Error: "
               << fuse_file.error_details() << " "
               << base::File::ErrorToString(fuse_file.error_details());
    *error = MOUNT_ERROR_INTERNAL;
    return nullptr;
  }

  *error = MountFuseDevice(platform_, source, filesystem_type(), target_path,
                           fuse_file, mount_user_id, mount_group_id,
                           mount_options());
  if (*error != MOUNT_ERROR_NONE) {
    LOG(ERROR) << "Can't perform unprivileged FUSE mount: " << error;
    return nullptr;
  }

  // The |fuse_failure_unmounter| closure runner is used to unmount the FUSE
  // filesystem if any part of starting the FUSE helper process fails.
  base::ScopedClosureRunner fuse_cleanup_runner(base::BindOnce(
      [](const Platform* platform, const std::string& target_path) {
        LOG(INFO) << "FUSE cleanup on start failure for " << quote(target_path);

        MountErrorType unmount_error = platform->Unmount(target_path, 0);
        LOG_IF(ERROR, unmount_error != MOUNT_ERROR_NONE)
            << "Cannot unmount FUSE mount point " << quote(target_path)
            << " after launch failure: " << unmount_error;
      },
      platform_, target_path.value()));

  // If a block device is being mounted, bind mount it into the sandbox.
  if (base::StartsWith(source, "/dev/", base::CompareCase::SENSITIVE)) {
    // Re-own source.
    if (!platform_->SetOwnership(source, getuid(), mount_group_id) ||
        !platform_->SetPermissions(source, kSourcePathPermissions)) {
      LOG(ERROR) << "Can't set up permissions on " << quote(source);
      *error = MOUNT_ERROR_INSUFFICIENT_PERMISSIONS;
      return nullptr;
    }

    if (!mount_process->BindMount(source, source, true, false)) {
      LOG(ERROR) << "Cannot bind mount device " << quote(source);
      *error = MOUNT_ERROR_INVALID_ARGUMENT;
      return nullptr;
    }
  }

  // TODO(crbug.com/933018): Remove when DriveFS helper is refactored.
  if (!mount_process->Mount("tmpfs", "/home", "tmpfs", "mode=0755,size=10M")) {
    LOG(ERROR) << "Can't mount /home";
    *error = MOUNT_ERROR_INTERNAL;
    return nullptr;
  }

  // This is for additional data dirs.
  for (const auto& path : accessible_paths_) {
    if (!mount_process->BindMount(path.path, path.path, path.writable,
                                  path.recursive)) {
      LOG(ERROR) << "Can't bind " << quote(path.path);
      *error = MOUNT_ERROR_INVALID_ARGUMENT;
      return nullptr;
    }
  }

  std::string options_string = mount_options().ToString();
  if (!options_string.empty()) {
    mount_process->AddArgument("-o");
    mount_process->AddArgument(options_string);
  }
  if (!source.empty()) {
    mount_process->AddArgument(source);
  }
  mount_process->AddArgument(
      base::StringPrintf("/dev/fd/%d", fuse_file.GetPlatformFile()));

  std::vector<std::string> output;
  int return_code = mount_process->Run(&output);
  if (return_code != 0) {
    LOG(ERROR) << "FUSE mount program failed with return code " << return_code;
    if (!output.empty()) {
      LOG(ERROR) << "FUSE mount program outputted " << output.size()
                 << " lines:";
      for (const std::string& line : output) {
        LOG(ERROR) << line;
      }
    }
    *error = MOUNT_ERROR_MOUNT_PROGRAM_FAILED;
    return nullptr;
  }

  // At this point, the FUSE daemon has successfully started. Release the
  // cleanup closure which is only intended to cleanup on failure.
  ignore_result(fuse_cleanup_runner.Release());

  std::unique_ptr<FUSEMountPoint> mount_point =
      std::make_unique<FUSEMountPoint>(target_path, platform_);

  // Add a watcher that cleans up the FUSE mount when the process exits.
  // This is defined as in-jail "init" process, denoted by pid(),
  // terminates, which happens only when the last process in the jailed PID
  // namespace terminates.
  process_reaper_->WatchForChild(
      FROM_HERE, mount_process->pid(),
      base::BindOnce(CleanUpCallback,
                     base::BindOnce(
                         [](base::WeakPtr<FUSEMountPoint> mount_point,
                            const Platform* platform) {
                           if (!mount_point) {
                             // If |mount_point| has been deleted, it was
                             // already unmounted and cleaned up due to a
                             // request from the browser (or logout). In this
                             // case, there's nothing to do.
                             return;
                           }

                           MountErrorType unmount_error =
                               mount_point->Unmount();
                           LOG_IF(ERROR, unmount_error != MOUNT_ERROR_NONE)
                               << "Cannot unmount FUSE mount point "
                               << quote(mount_point->path())
                               << " after process exit: " << unmount_error;

                           if (!platform->RemoveEmptyDirectory(
                                   mount_point->path().value())) {
                             PLOG(ERROR) << "Cannot remove FUSE mount point "
                                         << quote(mount_point->path().value())
                                         << " after process exit";
                           }
                         },
                         mount_point->GetWeakPtr(), platform_),
                     target_path));

  *error = MOUNT_ERROR_NONE;
  return std::move(mount_point);
}

std::unique_ptr<SandboxedProcess> FUSEMounter::CreateSandboxedProcess() const {
  return std::make_unique<SandboxedProcess>();
}

}  // namespace cros_disks
