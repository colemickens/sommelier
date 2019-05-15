// Copyright 2019 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CRYPTOHOME_USERDATAAUTH_H_
#define CRYPTOHOME_USERDATAAUTH_H_

#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <base/files/file_path.h>
#include <base/location.h>
#include <base/threading/thread.h>
#include <brillo/secure_blob.h>
#include <dbus/bus.h>

#include "cryptohome/challenge_credentials/challenge_credentials_helper.h"
#include "cryptohome/credentials.h"
#include "cryptohome/crypto.h"
#include "cryptohome/homedirs.h"
#include "cryptohome/install_attributes.h"
#include "cryptohome/mount.h"
#include "cryptohome/mount_factory.h"
#include "cryptohome/platform.h"

#include "UserDataAuth.pb.h"

namespace cryptohome {

class UserDataAuth {
 public:
  UserDataAuth();
  ~UserDataAuth();

  // Note that this function must be called from the thread that created this
  // object, so that |origin_task_runner_| is initialized correctly.
  bool Initialize();

  // =============== Mount Related Public DBus API ===============
  // Methods below are used directly by the DBus interface

  // If username is empty, returns true if any mount is mounted, otherwise,
  // returns true if the mount associated with the given |username| is mounted.
  // For |is_ephemeral_out|, if no username is given, then is_ephemeral_out is
  // set to true when any mount is ephemeral. Otherwise, is_ephemeral_out is set
  // to true when the mount associated with the given |username| is mounted in
  // an ephemeral manner. If nullptr is passed in for is_ephemeral_out, then it
  // won't be touched. Ephemeral mount means that the content of the mount is
  // cleared once the user logs out.
  bool IsMounted(const std::string& username = "",
                 bool* is_ephemeral_out = nullptr);

  // Returns true if the mount that corresponds to the username is mounted,
  // false otherwise. If that mount is ephemeral, then is_ephemeral_out is set
  // to true, false otherwise. If nullptr is passed in for is_ephemeral_out,
  // then it won't be touched. Ephemeral mount means that the content of the
  // mount is cleared once the user logs out.
  bool IsMountedForUser(const std::string& username,
                        bool* is_ephemeral_out = nullptr);

  // Calling this function will unmount all mounted cryptohomes. It'll return
  // true if all mounts are cleanly unmounted.
  // Note: This must only be called on mount thread
  bool Unmount();

  // This function will attempt to mount the requested user's home directory, as
  // specified in |request|. Once that's done, it'll call |on_done| to notify
  // the result. Note that there's no guarantee on whether |on_done| is called
  // before or after this function returns.
  void DoMount(
      user_data_auth::MountRequest request,
      base::OnceCallback<void(const user_data_auth::MountReply&)> on_done);

  // =============== Mount Related Public Utilities ===============

  // Called during initialization (and on mount events) to ensure old mounts
  // are marked for unmount when possible by the kernel.  Returns true if any
  // mounts were stale and not cleaned up (because of open files).
  // Note: This must only be called on mount thread
  //
  // Parameters
  // - force: if true, unmounts all existing shadow mounts.
  //          if false, unmounts shadows mounts with no open files.
  bool CleanUpStaleMounts(bool force);

  // Calling this function will reset the TPM context of every mount, that is,
  // it'll force a reload of all cryptohome key that is associated with mounts.
  void ResetAllTPMContext();

  // Set the |force_ecryptfs_| variable, if true, all mounts will use eCryptfs
  // for encryption. If eCryptfs is not used, then dircrypto -- the native ext4
  // directory encryption mechanism is used. Note that this is usually used in
  // main() because there's a command line switch for selecting dircrypto or
  // eCryptfs.
  void set_force_ecryptfs(bool force_ecryptfs) {
    force_ecryptfs_ = force_ecryptfs;
  }

  // Set the |legacy_mount_| variable. For more information on legacy_mount_,
  // see comment of Mount::MountLegacyHome(). Note that this is usually used in
  // main() because there's a command line switch for selecting this.
  void set_legacy_mount(bool legacy) { legacy_mount_ = legacy; }

  // =============== Key Related Public Utilities ===============
  // Add the key specified in the request, and return a CryptohomeErrorCode to
  // indicate the status of adding the key. If CryptohomeErrorCode is
  // CRYPTOHOME_ERROR_NOT_SET, then the key is successfully added.
  user_data_auth::CryptohomeErrorCode AddKey(
      const user_data_auth::AddKeyRequest request);

  // Check the key given in |request| again the currently mounted directories
  // and other credentials. Returns CRYPTOHOME_ERROR_NOT_SET if the key is found
  // to match.
  user_data_auth::CryptohomeErrorCode CheckKey(
      const user_data_auth::CheckKeyRequest request);

  // Remove the key given in |request.key| with the authorization given in
  // |request.authorization_request|. Returns CRYPTOHOME_ERROR_NOT_SET if the
  // key is successfully removed, returns other Error Code if not.
  user_data_auth::CryptohomeErrorCode RemoveKey(
      const user_data_auth::RemoveKeyRequest request);

  // List the keys stored in |homedirs_|. If CRYPTOHOME_ERROR_NOT_SET is
  // returned, then |labels_out| contains the label of the keys. Otherwise, the
  // content of |labels_out| is undefined.
  user_data_auth::CryptohomeErrorCode ListKeys(
      const user_data_auth::ListKeysRequest& request,
      std::vector<std::string>* labels_out);

  // =============== PKCS#11 Related Public Methods ===============

  // This initializes the PKCS#11 for a particular mount. Note that this is
  // used mostly internally, by Mount related functions to bring up the PKCS#11
  // functionalities after mounting.
  void InitializePkcs11(cryptohome::Mount* mount);

  // =============== Install Attributes Related Utilities ===============

  // Return true if this device is enterprise owned.
  bool IsEnterpriseOwned() { return enterprise_owned_; }

  // =============== Miscellaneous ===============

  // This is called by tpm_init_ when there's any update on ownership status
  // of the TPM.
  void OwnershipCallback(bool status, bool took_ownership);

  // This is called whenever we try to create a Mount object. This callback is
  // used by |mount_factory_|.
  void PreMountCallback();

  // Set the current dbus connection, this is usually used by the dbus daemon
  // object that owns the instance of this object.
  void set_dbus(scoped_refptr<::dbus::Bus> bus) { bus_ = bus; }

  // ================= Threading Utilities ==================

  // Returns true if we are currently running on the origin thread
  bool IsOnOriginThread() {
    // Note that this function should not rely on |origin_task_runner_| because
    // it may be unavailable when this function is first called by
    // UserDataAuth::Initialize()
    return disable_threading_ ||
           base::PlatformThread::CurrentId() == origin_thread_id_;
  }

  // Returns true if we are currently running on the mount thread
  bool IsOnMountThread() {
    // GetThreadId blocks if the thread is not started yet.
    return disable_threading_ ||
           (mount_thread_.IsRunning() &&
            (base::PlatformThread::CurrentId() == mount_thread_.GetThreadId()));
  }

  // DCHECK if we are running on the origin thread. Will have no effect
  // in production.
  void AssertOnOriginThread() { DCHECK(IsOnOriginThread()); }

  // DCHECK if we are running on the mount thread. Will have no effect
  // in production.
  void AssertOnMountThread() { DCHECK(IsOnMountThread()); }

  // Post Task to origin thread. For the caller, from_here is usually FROM_HERE
  // macro, while task is a callback function to be posted. Will return true if
  // the task may be run sometime in the future, false if it will definitely not
  // run.
  bool PostTaskToOriginThread(const base::Location& from_here,
                              base::OnceClosure task);

  // Post Task to mount thread. For the caller, from_here is usually FROM_HERE
  // macro, while task is a callback function to be posted. Will return true if
  // the task may be run sometime in the future, false if it will definitely not
  // run.
  bool PostTaskToMountThread(const base::Location& from_here,
                             base::OnceClosure task);

  // ================= Testing Utilities ==================
  // Note that all functions below in this section should only be used for unit
  // testing purpose only.

  // Override |crypto_| for testing purpose
  void set_crypto(cryptohome::Crypto* crypto) { crypto_ = crypto; }

  // Override |homedirs_| for testing purpose
  void set_homedirs(cryptohome::HomeDirs* homedirs) { homedirs_ = homedirs; }

  // Override |tpm_| for testing purpose
  void set_tpm(Tpm* tpm) { tpm_ = tpm; }

  // Override |tpm_init_| for testing purpose
  void set_tpm_init(TpmInit* tpm_init) { tpm_init_ = tpm_init; }

  // Override |platform_| for testing purpose
  void set_platform(cryptohome::Platform* platform) { platform_ = platform; }

  // override |chaps_client_| for testing purpose
  void set_chaps_client(chaps::TokenManagerClient* chaps_client) {
    chaps_client_ = chaps_client;
  }

  // Override |install_attrs_| for testing purpose
  void set_install_attrs(InstallAttributes* install_attrs) {
    install_attrs_ = install_attrs;
  }

  // Associate a particular mount object |mount| with the username |username|
  // for testing purpose
  void set_mount_for_user(const std::string& username,
                          cryptohome::Mount* mount) {
    mounts_[username] = mount;
  }

  // Set |disable_threading_|, so that we can disable threading in this class
  // for testing purpose. See comment of |disable_threading_| for more
  // information.
  void set_disable_threading(bool disable_threading) {
    disable_threading_ = disable_threading;
  }

 private:
  // Note: In Service class (the class that this class is refactored from),
  // there is a use_tpm_ member variable, but it is almost unused and always set
  // to true there, so in this class, if we are migrating any code from Service
  // class and use_tpm_ is used there, then we'll just assume it's true and not
  // have a use_tpm_ variable here.
  // The same is true for initialize_tpm_ variable, it is assumed to be true.

  // =============== Mount Related Utilities ===============
  // Returns the mount object associated with the given username
  scoped_refptr<cryptohome::Mount> GetMountForUser(const std::string& username);

  // Filters out active mounts from |mounts|, populating |active_mounts| set.
  // If |include_busy_mount| is false, then stale mounts with open files will be
  // treated as active mount, and be moved from |mounts| to |active_mounts|.
  // Otherwise,  all stale mounts are included in |mounts|. Returns true if
  // |include_busy_mount| is true and there's at least one stale mount with open
  // file(s) and treated as active mount during the process.
  bool FilterActiveMounts(
      std::multimap<const base::FilePath, const base::FilePath>* mounts,
      std::multimap<const base::FilePath, const base::FilePath>* active_mounts,
      bool include_busy_mount);

  // Populates |mounts| with ephemeral cryptohome mount points.
  void GetEphemeralLoopDevicesMounts(
      std::multimap<const base::FilePath, const base::FilePath>* mounts);

  // Unload any user pkcs11 tokens _not_ belonging to one of the mounts in
  // |exclude|. This is used to clean up any stale loaded tokens after a
  // cryptohome crash.
  // Note that system tokens are not affected.
  bool UnloadPkcs11Tokens(const std::vector<base::FilePath>& exclude);

  // Safely empties the MountMap and may request unmounting. If |unmount| is
  // true, the return value will reflect if all mounts unmounted cleanly or not.
  // That is, it'll be true if all unmounts are clean and successful.
  // Note: This must only be called on mount thread
  bool RemoveAllMounts(bool unmount);

  // Returns true if any of the path in |prefixes| starts with |path|
  // Note that this function is case insensitive
  static bool PrefixPresent(const std::vector<base::FilePath>& prefixes,
                            const std::string path);

  // Calling this function will try to ensure that |public_mount_salt_| is ready
  // to use. If it's not ready, we'll generate it. Returns true if
  // |public_mount_salt_| is ready.
  bool CreatePublicMountSaltIfNeeded();

  // Gets passkey for |public_mount_id|. Returns true if a passkey is generated
  // successfully. Otherwise, returns false.
  bool GetPublicMountPassKey(const std::string& public_mount_id,
                             std::string* public_mount_passkey);

  // Determines whether the mount request should be ephemeral. On error, returns
  // false and sets the error code in |error|. Otherwise, returns true and fills
  // the result in |is_ephemeral|.
  bool GetShouldMountAsEphemeral(
      const std::string& account_id,
      bool is_ephemeral_mount_requested,
      bool has_create_request,
      bool* is_ephemeral,
      user_data_auth::CryptohomeErrorCode* error) const;

  // Does what its name suggests. The use of this method is to ensure that only
  // one Mount is ever created per username.
  scoped_refptr<cryptohome::Mount> GetOrCreateMountForUser(
      const std::string& username);

  // Called during mount requests to ensure old hidden mounts are unmounted.
  // Note that this only cleans up |mounts_| entries which were mounted with the
  // hidden_mount=true parameter, as these are supposed to be temporary. Old
  // mounts from another cryptohomed run (e.g. after a crash) are cleaned up in
  // CleanUpStaleMounts().
  bool CleanUpHiddenMounts();

  // Builds the PCR restrictions to be applied to the challenge-protected vault
  // keyset.
  void GetChallengeCredentialsPcrRestrictions(
      const std::string& obfuscated_username,
      std::vector<std::map<uint32_t, brillo::Blob>>* pcr_restrictions);

  // This is a utility function used by DoMount(). It is called if the request
  // mounting operation requires challenge response authentication. i.e. The key
  // for the storage is sealed.
  void DoChallengeResponseMount(
      const user_data_auth::MountRequest& request,
      const Mount::MountArgs& mount_args,
      base::OnceCallback<void(const user_data_auth::MountReply&)> on_done);

  // This is a utility function used by DoChallengeResponseMount(), and is
  // called once we're done doing challenge response authentication.
  void OnChallengeResponseMountCredentialsObtained(
      const user_data_auth::MountRequest& request,
      const Mount::MountArgs mount_args,
      base::OnceCallback<void(const user_data_auth::MountReply&)> on_done,
      std::unique_ptr<Credentials> credentials);

  // This is a utility function used by DoMount(). It is called either by
  // DoMount() (when using password for authentication.), or by
  // OnChallengeResponseMountCredentialsObtained() (when using challenge
  // response authentication.).
  void ContinueMountWithCredentials(
      const user_data_auth::MountRequest& request,
      std::unique_ptr<Credentials> credentials,
      const Mount::MountArgs& mount_args,
      base::OnceCallback<void(const user_data_auth::MountReply&)> on_done);

  // =============== PKCS#11 Related Utilities ===============

  // This is called when TPM is enabled and owned, so that we can continue
  // the initialization of any PKCS#11 that was paused because TPM wasn't
  // ready.
  void ResumeAllPkcs11Initialization();

  // =============== Install Attributes Related Utilities ===============

  // Set whether this device is enterprise owned. Calling this method will have
  // effect on all currently mounted mounts. This can only be called on
  // mount_thread_.
  void SetEnterpriseOwned(bool enterprise_owned);

  // Detect whether this device is enterprise owned, and call
  // SetEnterpriseOwned(). This can only be called on origin thread.
  void DetectEnterpriseOwnership();

  // Call this method to initialize the install attributes functionality. This
  // can only be called on origin thread.
  void InitializeInstallAttributes();

  // Calling this method will finalize the install attributes if we current have
  // a non-guest mount mounted. This can only be called on mount thread.
  void FinalizeInstallAttributesIfMounted();

  // =============== Threading Related Variables ===============

  // The task runner that belongs to the thread that created this UserDataAuth
  // object. Currently, this is required to be the same as the dbus thread's
  // task runner.
  scoped_refptr<base::SingleThreadTaskRunner> origin_task_runner_;

  // The thread ID of the thread that created this UserDataAuth object.
  // Currently, this is required to be th esame as the dbus thread's task
  // runner.
  base::PlatformThreadId origin_thread_id_;

  // The thread for performing long running, or mount related operations
  base::Thread mount_thread_;

  // This variable is used only for unit testing purpose. If set to true, it'll
  // disable the the threading mechanism in this class so that testing doesn't
  // fail. When threading is disabled, posting to origin or mount thread will
  // execute immediately, and all checks for whether we are on mount or origin
  // thread will result in true.
  bool disable_threading_;

  // =============== Basic Utilities Related Variables ===============
  // The system salt that is used for obfuscating the username
  brillo::SecureBlob system_salt_;

  // The object for accessing the TPM
  // Note that TPM doesn't use the unique_ptr for default pattern, since the tpm
  // is a singleton - we don't want it getting destroyed when we are.
  Tpm* tpm_;

  // The default TPM init object.
  std::unique_ptr<TpmInit> default_tpm_init_;

  // The TPM init object. Note that |tpm_init_| and |default_tpm_init_| will be
  // removed at the end of the refactoring that's happening in cryptohome
  // (b/123679223).
  TpmInit* tpm_init_;

  // The default platform object for accessing platform related functionalities
  std::unique_ptr<cryptohome::Platform> default_platform_;

  // The actual platform object used by this class, usually set to
  // default_platform_, but can be overridden for testing
  cryptohome::Platform* platform_;

  // The default crypto object for performing cryptographic operations
  std::unique_ptr<cryptohome::Crypto> default_crypto_;

  // The actual crypto object used by this class, usually set to
  // default_crypto_, but can be overridden for testing
  cryptohome::Crypto* crypto_;

  // The default token manager client for accessing chapsd's PKCS#11 interface
  std::unique_ptr<chaps::TokenManagerClient> default_chaps_client_;

  // The actual token manager client used by this class, usually set to
  // default_chaps_client_, but can be overridden for testing.
  chaps::TokenManagerClient* chaps_client_;

  // A dbus connection, this is used by any code in this class that needs access
  // to the system DBus. Such as when creating an instance of
  // KeyChallengeService.
  scoped_refptr<::dbus::Bus> bus_;

  // =============== Install Attributes Related Variables ===============

  // The default install attributes object, for accessing install attributes
  // related functionality.
  std::unique_ptr<cryptohome::InstallAttributes> default_install_attrs_;

  // The actual install attributes object used by this class, usually set to
  // |default_install_attrs_|, but can be overriden for testing. This object
  // should only be accessed on the origin thread.
  cryptohome::InstallAttributes* install_attrs_;

  // Whether this device is an enterprise owned device. Write access should only
  // happen on mount thread.
  bool enterprise_owned_;

  // =============== Mount Related Variables ===============

  // Defines a type for tracking Mount objects for each user by username.
  typedef std::map<const std::string, scoped_refptr<cryptohome::Mount>>
      MountMap;

  // Records the Mount objects associated with each username.
  // This and its content should only be accessed from the mount thread.
  // TODO(b/126022424): Verify that this access paradigm doesn't cause
  // measurable performance impact.
  MountMap mounts_;

  // Note: In Service class (the class that this class is refactored from),
  // there is a mounts_lock_ lock for inserting/removal of mounts_ map. However,
  // in this class, all accesses to mounts_ should happen on the mount thread,
  // so no lock is needed.

  // This is an unused variable that's lifted over from service.cc. It is kept
  // here for the purpose of keeping the code in userdatauth.h/.cc as close as
  // possible to the version in service.cc.
  bool reported_pkcs11_init_fail_;

  // The homedirs_ object in normal operation
  std::unique_ptr<HomeDirs> default_homedirs_;

  // This holds the object that records informations about the homedirs.
  // This is usually set to default_homedirs_, but can be overridden for
  // testing.
  // This is to be accessed from the mount thread only because there's no
  // guarantee on thread safety of the HomeDirs object.
  HomeDirs* homedirs_;

  // This holds a timestamp for each user that is the time that the user was
  // active.
  std::unique_ptr<UserOldestActivityTimestampCache> user_timestamp_cache_;

  // The default mount factory instance that is used for creating Mount objects.
  std::unique_ptr<cryptohome::MountFactory> default_mount_factory_;

  // The mount factory instance that is actually used by this class to create
  // Mount object. This is usually |default_mount_factory_|, but can be
  // overridden for testing.
  cryptohome::MountFactory* mount_factory_;

  // This holds the salt that is used to derive the passkey for public mounts.
  brillo::SecureBlob public_mount_salt_;

  // Challenge credential helper utility class. This class is required for doing
  // a challenge response style login, and is only lazily created when mounting
  // a mount that requires challenge response login type is performed.
  std::unique_ptr<ChallengeCredentialsHelper> challenge_credentials_helper_;

  // Guest user's username.
  std::string guest_user_;

  // Force the use of eCryptfs. If eCryptfs is not used, then dircrypto -- the
  // native ext4 directory encryption is used.
  bool force_ecryptfs_;

  // Whether we are using legacy mount. See Mount::MountLegacyHome()'s comment
  // for more information.
  bool legacy_mount_;
};

}  // namespace cryptohome

#endif  // CRYPTOHOME_USERDATAAUTH_H_
