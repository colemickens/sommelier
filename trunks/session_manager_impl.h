// Copyright 2015 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef TRUNKS_SESSION_MANAGER_IMPL_H_
#define TRUNKS_SESSION_MANAGER_IMPL_H_

#include "trunks/session_manager.h"

#include <string>

#include <gtest/gtest_prod.h>

#include "trunks/tpm_generated.h"
#include "trunks/trunks_factory.h"

namespace trunks {

// This class is used to keep track of a TPM session. Each instance of this
// class is used to account for one instance of a TPM session. Currently
// this class is used by AuthorizationSession instances to keep track of TPM
// sessions.
class TRUNKS_EXPORT SessionManagerImpl : public SessionManager {
 public:
  explicit SessionManagerImpl(const TrunksFactory& factory);
  ~SessionManagerImpl() override;

  TPM_HANDLE GetSessionHandle() const override { return session_handle_; }
  void CloseSession() override;
  TPM_RC StartSession(TPM_SE session_type,
                      TPMI_DH_ENTITY bind_entity,
                      const std::string& bind_authorization_value,
                      bool salted,
                      bool enable_encryption,
                      HmacAuthorizationDelegate* delegate) override;

 private:
  // This function is used to encrypt a plaintext salt |salt|, using RSA
  // public encrypt with the SaltingKey PKCS1_OAEP padding. It follows the
  // specification defined in TPM2.0 Part 1 Architecture, Appendix B.10.2.
  // The encrypted salt is stored in the out parameter |encrypted_salt|.
  TPM_RC EncryptSalt(const std::string& salt, std::string* encrypted_salt);

  // This factory is only set in the constructor and is used to instantiate
  // The TPM class to forward commands to the TPM chip.
  const TrunksFactory& factory_;
  // This handle keeps track of the TPM session. It is issued by the TPM,
  // and is only modified when a new TPM session is started using
  // StartBoundSession or StartUnboundSession. We use this to keep track of
  // the session handle, so that we can clean it up when this class is
  // destroyed.
  TPM_HANDLE session_handle_;

  friend class SessionManagerTest;
  DISALLOW_COPY_AND_ASSIGN(SessionManagerImpl);
};

}  // namespace trunks

#endif  // TRUNKS_SESSION_MANAGER_IMPL_H_
