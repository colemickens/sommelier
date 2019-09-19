// Copyright 2019 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef U2FD_USER_STATE_H_
#define U2FD_USER_STATE_H_

#include <string>
#include <vector>

#include <base/files/file_util.h>
#include <base/optional.h>
#include <brillo/secure_blob.h>
#include <dbus/bus.h>
#include <session_manager/dbus-proxies.h>

namespace u2f {

// Encapsulates access to user-specific U2F state. This class is not
// thread-safe.
class UserState {
 public:
  // Constructs a new UserState object using the specified dbus object.
  // The counter values returned by this object will be >= counter_min.
  UserState(org::chromium::SessionManagerInterfaceProxy* sm_proxy,
            uint32_t counter_min);

  // Get*() methods return base::nullopt if user state is currently
  // unavailable.

  // Get the user secret.
  base::Optional<brillo::SecureBlob> GetUserSecret();

  // Returns the current counter value. The returned value must not be
  // returned externally until the counter has succesfully been
  // incremented (and persisted to disk).
  base::Optional<std::vector<uint8_t>> GetCounter();

  // Increments the counter value, which is subsequently immediately
  // flushed to disk. Returns true on success, false if the counter
  // could not be persisted to disk.
  bool IncrementCounter();

 private:
  // Handler for the SessionStateChanged signal.
  void OnSessionStateChanged(const std::string& state);

  // Fetches the sanitized username for the primary session.
  void UpdatePrimarySessionSanitizedUser();

  // Attempts to load state for the current primary session.
  void LoadState();

  // The following methods assume that a sanitized_user_ is currently available.

  // Loads an existing user secret if available, or creates a new secret if not.
  void LoadOrCreateUserSecret();

  // Loads an existing user secret from disk.
  void LoadUserSecret(const base::FilePath& path);

  // Creates a new user secret and stores it on disk, blocks until the secret
  // has been flushed to disk.
  void CreateUserSecret(const base::FilePath& path);

  // Loads the counter from disk.
  void LoadCounter();

  // Persists the counter to disk, blocks until the counter has been flushed to
  // disk.
  bool PersistCounter();

  // Current sanitized username, if any.
  base::Optional<std::string> sanitized_user_;

  base::Optional<brillo::SecureBlob> user_secret_;
  base::Optional<uint32_t> counter_;

  org::chromium::SessionManagerInterfaceProxy* sm_proxy_;
  base::WeakPtrFactory<UserState> weak_ptr_factory_;

  const uint32_t counter_min_;
};

}  // namespace u2f

#endif  // U2FD_USER_STATE_H_
