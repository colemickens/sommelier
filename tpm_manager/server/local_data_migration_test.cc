// Copyright 2019 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "tpm_manager/server/local_data_migration.h"

#include <attestation/proto_bindings/attestation_ca.pb.h>
#include <base/files/file_path.h>
#include <brillo/secure_blob.h>
#include <gmock/gmock.h>
#include <gtest/gtest.h>
#include <libtpmcrypto/tpm.h>
#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <openssl/sha.h>

#include <algorithm>
#include <limits>
#include <memory>
#include <unordered_map>
#include <utility>

#include "tpm_manager/proto_bindings/tpm_manager.pb.h"
#include "tpm_manager/server/legacy_local_data.pb.h"

namespace {

constexpr size_t kAesKeySize = 32;
constexpr size_t kAesBlockSize = 16;
constexpr char kIVForTest[kAesBlockSize + 1] = "I'm your father.";
constexpr char kAesKeyForTest[kAesKeySize + 1] =
    "NOOOOOOOOOOOOOOO~!!!!!!!!!!!!!!!";
constexpr char kNonNullPointerCheckFailedRegex[] =
    R"(Check failed: ([a-z_]+ != nullptr))";

// The following few functions are simplied to minimum from
// "attestation/common/crypto_utility_impl.cc" so we can decrypt the encrypted
// database. So far it's okay to have duplicated utility functions as those in
// |local_data_migration.cc| because it's only for testing purpose and one day
// the shared code should be put in a common library.
//
// TODO(cylai): replace this with calls to commmon library once we have a
// library shared across all hwsec daemons.
unsigned char* SecureBlobAsSSLBuffer(const brillo::SecureBlob& blob) {
  return static_cast<unsigned char*>(
      const_cast<brillo::SecureBlob::value_type*>(blob.data()));
}

bool AesEncrypt(const EVP_CIPHER* cipher,
                const brillo::SecureBlob& data,
                const brillo::SecureBlob& key,
                const brillo::SecureBlob& iv,
                brillo::SecureBlob* encrypted_data) {
  if (key.size() != static_cast<size_t>(EVP_CIPHER_key_length(cipher)) ||
      iv.size() != kAesBlockSize) {
    return false;
  }
  if (data.size() > static_cast<size_t>(std::numeric_limits<int>::max())) {
    // EVP_EncryptUpdate takes a signed int.
    return false;
  }
  unsigned char* input_buffer = SecureBlobAsSSLBuffer(data);
  unsigned char* key_buffer = SecureBlobAsSSLBuffer(key);
  unsigned char* iv_buffer = SecureBlobAsSSLBuffer(iv);
  // Allocate enough space for the output (including padding).
  encrypted_data->resize(data.size() + kAesBlockSize);
  unsigned char* output_buffer = SecureBlobAsSSLBuffer(*encrypted_data);
  int output_size = 0;
  EVP_CIPHER_CTX encryption_context;
  EVP_CIPHER_CTX_init(&encryption_context);
  if (!EVP_EncryptInit_ex(&encryption_context, cipher, nullptr, key_buffer,
                          iv_buffer)) {
    return false;
  }
  if (!EVP_EncryptUpdate(&encryption_context, output_buffer, &output_size,
                         input_buffer, data.size())) {
    EVP_CIPHER_CTX_cleanup(&encryption_context);
    return false;
  }
  size_t total_size = output_size;
  output_buffer += output_size;
  output_size = 0;
  if (!EVP_EncryptFinal_ex(&encryption_context, output_buffer, &output_size)) {
    EVP_CIPHER_CTX_cleanup(&encryption_context);
    return false;
  }
  total_size += output_size;
  encrypted_data->resize(total_size);
  EVP_CIPHER_CTX_cleanup(&encryption_context);
  return true;
}

brillo::SecureBlob HmacSha512(const brillo::SecureBlob& key,
                              const brillo::SecureBlob& data) {
  brillo::SecureBlob mac;
  mac.resize(SHA512_DIGEST_LENGTH);
  HMAC(EVP_sha512(), SecureBlobAsSSLBuffer(key), key.size(),
       SecureBlobAsSSLBuffer(data), data.size(), mac.data(), nullptr);
  return mac;
}

bool EncryptLegacyAttestationDatabase(
    const tpm_manager::LegacyAttestationDatabase& database,
    tpmcrypto::Tpm* tpm,
    brillo::SecureBlob* serialized_encrypted_data) {
  std::string serialized_database = database.SerializeAsString();
  if (serialized_database.empty()) {
    return false;
  }
  attestation::EncryptedData encrypted_data;
  brillo::SecureBlob wrapped_key;
  if (!tpm->SealToPCR0(brillo::SecureBlob(std::string(kAesKeyForTest)),
                       &wrapped_key)) {
    return false;
  }
  encrypted_data.set_wrapped_key(wrapped_key.to_string());
  brillo::SecureBlob encrypted_output;
  if (!AesEncrypt(EVP_aes_256_cbc(), brillo::SecureBlob(serialized_database),
                  brillo::SecureBlob(std::string(kAesKeyForTest)),
                  brillo::SecureBlob(std::string(kIVForTest)),
                  &encrypted_output)) {
    return false;
  }
  encrypted_data.set_encrypted_data(encrypted_output.to_string());
  encrypted_data.set_iv(kIVForTest);
  encrypted_data.set_mac(
      HmacSha512(
          brillo::SecureBlob(std::string(kAesKeyForTest)),
          brillo::SecureBlob::Combine(
              brillo::SecureBlob(std::string(kIVForTest)), encrypted_output))
          .to_string());
  serialized_encrypted_data->resize(encrypted_data.ByteSize());
  encrypted_data.SerializeWithCachedSizesToArray(
      serialized_encrypted_data->data());
  return true;
}

// A fake implementation of |Tpm| to emulate the behavior of sealing/unsealing
// operation by a real TPM object. The sealing and unsealing are emulated by
// reversing the content.
//
// TODO(cylai): implement a mock class in |tpmcrypto| project instead so the
// faking/mocking logic can be shared in a better way.
class FakeTpm : public tpmcrypto::Tpm {
 public:
  FakeTpm() = default;
  ~FakeTpm() override = default;

  bool SealToPCR0(const brillo::SecureBlob& value,
                  brillo::SecureBlob* sealed_value) override {
    CHECK(sealed_value);
    return Reverse(value, sealed_value);
  }

  bool Unseal(const brillo::SecureBlob& sealed_value,
              brillo::SecureBlob* value) override {
    CHECK(value);
    return !is_failure_mode_ && Reverse(sealed_value, value);
  }

  bool GetNVAttributes(uint32_t index, uint32_t* attributes) override {
    return true;
  }

  bool NVReadNoAuth(uint32_t index,
                    uint32_t offset,
                    size_t size,
                    std::string* data) override {
    return true;
  }

  // TODO(cylai): as the TODO above, determine if gmock is a more decent
  // choice.
  void SetIsFailureMode(bool flag) { is_failure_mode_ = flag; }

 private:
  bool Reverse(const brillo::SecureBlob& input, brillo::SecureBlob* output) {
    output->assign(input.rbegin(), input.rend());
    return true;
  }
  bool is_failure_mode_{false};

  DISALLOW_COPY_AND_ASSIGN(FakeTpm);
};

MATCHER_P(
    EqualsDelegate,
    d,
    "Compares |arg| against |d| regardless of their protobuf types as long as "
    "they have both have |blob|, |secret|, and |has_reset_lock_permissions| "
    "fields.") {
  return arg.blob() == d.blob() && arg.secret() == d.secret() &&
         arg.has_reset_lock_permissions() == d.has_reset_lock_permissions();
}

brillo::SecureBlob GenerateInvalidSerializedMessage() {
  return brillo::SecureBlob(128, 0);
}

std::string SealString(const std::string& s, tpmcrypto::Tpm* tpm) {
  brillo::SecureBlob output;
  CHECK(tpm->SealToPCR0(brillo::SecureBlob(s), &output));
  return output.to_string();
}

}  // namespace

namespace tpm_manager {

TEST(LocalDataMigrationTest, MigrateAuthDelegateDecrypt) {
  FakeTpm fake_tpm;
  LegacyAttestationDatabase expected_database;
  expected_database.mutable_delegate()->set_blob("blob");
  expected_database.mutable_delegate()->set_secret("secret");
  expected_database.mutable_delegate()->set_has_reset_lock_permissions(true);
  brillo::SecureBlob encrypted_database;
  ASSERT_TRUE(EncryptLegacyAttestationDatabase(expected_database, &fake_tpm,
                                               &encrypted_database));

  AuthDelegate result_delegate;
  EXPECT_TRUE(
      MigrateAuthDelegate(encrypted_database, &fake_tpm, &result_delegate));
  EXPECT_THAT(result_delegate, EqualsDelegate(expected_database.delegate()));

  fake_tpm.SetIsFailureMode(true);
  EXPECT_FALSE(
      MigrateAuthDelegate(encrypted_database, &fake_tpm, &result_delegate));
}

TEST(LocalDataMigrationTest, MigrateAuthDelegateInvalidParameter) {
  FakeTpm fake_tpm;
  AuthDelegate result_delegate;

  EXPECT_DEATH(
      MigrateAuthDelegate(brillo::SecureBlob{}, nullptr, &result_delegate),
      kNonNullPointerCheckFailedRegex);
  EXPECT_DEATH(MigrateAuthDelegate(brillo::SecureBlob{}, &fake_tpm, nullptr),
               kNonNullPointerCheckFailedRegex);

  brillo::SecureBlob invalid_encrypted_database =
      GenerateInvalidSerializedMessage();
  EXPECT_FALSE(MigrateAuthDelegate(invalid_encrypted_database, &fake_tpm,
                                   &result_delegate));
}

TEST(LocalDataMigrationTest, UnsealOwnerPasswordFromSerializedTpmStatus) {
  LegacyTpmStatus expected_tpm_status;
  FakeTpm fake_tpm;
  expected_tpm_status.set_owner_password(
      SealString("owner password", &fake_tpm));
  brillo::SecureBlob serialized_tpm_status(expected_tpm_status.ByteSize());
  ASSERT_TRUE(expected_tpm_status.SerializeWithCachedSizesToArray(
      serialized_tpm_status.data()));
  brillo::SecureBlob owner_password;
  EXPECT_TRUE(UnsealOwnerPasswordFromSerializedTpmStatus(
      serialized_tpm_status, &fake_tpm, &owner_password));
  EXPECT_EQ(owner_password.to_string(), "owner password");

  fake_tpm.SetIsFailureMode(true);
  EXPECT_FALSE(UnsealOwnerPasswordFromSerializedTpmStatus(
      serialized_tpm_status, &fake_tpm, &owner_password));
  fake_tpm.SetIsFailureMode(false);

  EXPECT_DEATH(UnsealOwnerPasswordFromSerializedTpmStatus(brillo::SecureBlob{},
                                                          &fake_tpm, nullptr),
               kNonNullPointerCheckFailedRegex);
  EXPECT_DEATH(UnsealOwnerPasswordFromSerializedTpmStatus(
                   brillo::SecureBlob{}, nullptr, &owner_password),
               kNonNullPointerCheckFailedRegex);

  EXPECT_FALSE(UnsealOwnerPasswordFromSerializedTpmStatus(
      GenerateInvalidSerializedMessage(), &fake_tpm, &owner_password));
}

// A subclass of |LocalDataMigrator| with the file I/O faked with access of a
// hash table.
class LocalDataMigratorWithFakeFile : public LocalDataMigrator {
 public:
  using FileContentMap = std::unordered_map<std::string, std::string>;
  LocalDataMigratorWithFakeFile() = default;
  ~LocalDataMigratorWithFakeFile() override = default;

  // Updates the entry in |file_content_map_| with |path| as the key and
  // |content| as the value.
  void SetFakeFileContent(std::string path, std::string content) {
    auto result = file_content_map_.emplace(path, content);
    if (!result.second) {
      result.first->second = std::move(content);
    }
  }

  // Removes |path| from |file_content_map_|. Return |true| iff the entry is
  // erased.
  bool RemoveFakeFileContent(const std::string& path) {
    return file_content_map_.erase(path) > 0;
  }

  // Switches the behavior of the fake file reading operation. When
  // |is_failure_mode| is |false|, the content is read from |file_content_map_|;
  // when |is_failure_mode| is |true|, the file reading operation always fails.
  void SetIsFailureModeForRead(bool is_failure_mode) {
    is_failure_mode_ = is_failure_mode;
  }

 protected:
  FileContentMap file_content_map_;
  bool is_failure_mode_{false};

  bool PathExists(const base::FilePath& path) override {
    return file_content_map_.count(path.value()) > 0;
  }

  // Fake file reading operation; see |SetIsFailureModeForRead|.
  bool ReadFileToString(const base::FilePath& path,
                        std::string* content) override {
    if (is_failure_mode_) {
      return false;
    }
    auto result = file_content_map_.find(path.value());
    if (result == file_content_map_.end()) {
      return false;
    }
    *content = result->second;
    return true;
  }
};

TEST(LocalDataMigratorTest, MigrateAuthDelegateIfNeeded) {
  FakeTpm fake_tpm;
  LocalDataMigratorWithFakeFile migrator;
  LocalData local_data;

  const base::FilePath fake_path("fake path");
  const base::FilePath non_existent_path("non existent path");

  bool has_migrated;
  EXPECT_DEATH(migrator.MigrateAuthDelegateIfNeeded(non_existent_path, nullptr,
                                                    &local_data, &has_migrated),
               kNonNullPointerCheckFailedRegex);
  EXPECT_DEATH(migrator.MigrateAuthDelegateIfNeeded(
                   non_existent_path, &fake_tpm, nullptr, &has_migrated),
               kNonNullPointerCheckFailedRegex);
  EXPECT_DEATH(migrator.MigrateAuthDelegateIfNeeded(
                   non_existent_path, &fake_tpm, &local_data, nullptr),
               kNonNullPointerCheckFailedRegex);

  EXPECT_TRUE(migrator.MigrateAuthDelegateIfNeeded(non_existent_path, &fake_tpm,
                                                   &local_data, &has_migrated));
  EXPECT_FALSE(has_migrated);

  LegacyAttestationDatabase expected_database;
  expected_database.mutable_delegate()->set_blob("blob");
  expected_database.mutable_delegate()->set_secret("secret");
  expected_database.mutable_delegate()->set_has_reset_lock_permissions(true);
  brillo::SecureBlob encrypted_database;
  ASSERT_TRUE(EncryptLegacyAttestationDatabase(expected_database, &fake_tpm,
                                               &encrypted_database));
  migrator.SetFakeFileContent(fake_path.value(),
                              encrypted_database.to_string());

  fake_tpm.SetIsFailureMode(true);
  EXPECT_FALSE(migrator.MigrateAuthDelegateIfNeeded(
      fake_path, &fake_tpm, &local_data, &has_migrated));
  fake_tpm.SetIsFailureMode(false);

  migrator.SetIsFailureModeForRead(true);
  EXPECT_FALSE(migrator.MigrateAuthDelegateIfNeeded(
      fake_path, &fake_tpm, &local_data, &has_migrated));
  migrator.SetIsFailureModeForRead(false);

  EXPECT_TRUE(migrator.MigrateAuthDelegateIfNeeded(fake_path, &fake_tpm,
                                                   &local_data, &has_migrated));
  EXPECT_TRUE(has_migrated);
  EXPECT_THAT(local_data.owner_delegate(),
              EqualsDelegate(expected_database.delegate()));

  // Checks if the migration does no-ops if the data exist at both sides
  local_data.mutable_owner_delegate()->set_blob("another blob");
  local_data.mutable_owner_delegate()->set_secret("another secret");
  const LocalData local_data_before_migration_again = local_data;
  EXPECT_TRUE(migrator.MigrateAuthDelegateIfNeeded(fake_path, &fake_tpm,
                                                   &local_data, &has_migrated));
  EXPECT_FALSE(has_migrated);
  EXPECT_THAT(
      local_data.owner_delegate(),
      EqualsDelegate(local_data_before_migration_again.owner_delegate()));
}

TEST(LocalDataMigratorTest, MigrateOwnerPasswordIfNeeded) {
  LocalDataMigratorWithFakeFile migrator;
  LocalData local_data;
  FakeTpm fake_tpm;
  const base::FilePath fake_path("fake path");
  const base::FilePath non_existent_path("non existent path");
  bool has_migrated;

  EXPECT_DEATH(migrator.MigrateOwnerPasswordIfNeeded(
                   non_existent_path, &fake_tpm, nullptr, &has_migrated),
               kNonNullPointerCheckFailedRegex);
  EXPECT_DEATH(migrator.MigrateOwnerPasswordIfNeeded(
                   non_existent_path, &fake_tpm, &local_data, nullptr),
               kNonNullPointerCheckFailedRegex);
  EXPECT_TRUE(migrator.MigrateOwnerPasswordIfNeeded(
      non_existent_path, &fake_tpm, &local_data, &has_migrated));
  EXPECT_FALSE(has_migrated);

  LegacyTpmStatus expected_tpm_status;
  expected_tpm_status.set_owner_password(
      SealString("owner password", &fake_tpm));
  brillo::SecureBlob serialized_tpm_status(expected_tpm_status.ByteSize());
  ASSERT_TRUE(expected_tpm_status.SerializeWithCachedSizesToArray(
      serialized_tpm_status.data()));

  migrator.SetFakeFileContent(fake_path.value(),
                              GenerateInvalidSerializedMessage().to_string());
  EXPECT_FALSE(migrator.MigrateOwnerPasswordIfNeeded(
      fake_path, &fake_tpm, &local_data, &has_migrated));

  migrator.SetFakeFileContent(fake_path.value(),
                              serialized_tpm_status.to_string());
  migrator.SetIsFailureModeForRead(true);
  EXPECT_FALSE(migrator.MigrateOwnerPasswordIfNeeded(
      fake_path, &fake_tpm, &local_data, &has_migrated));

  migrator.SetIsFailureModeForRead(false);
  EXPECT_TRUE(migrator.MigrateOwnerPasswordIfNeeded(
      fake_path, &fake_tpm, &local_data, &has_migrated));
  EXPECT_TRUE(has_migrated);
  EXPECT_EQ(local_data.owner_password(), "owner password");

  // Checks if the migration does no-ops if the data exist at both sides
  local_data.set_owner_password(
      SealString("another owner password", &fake_tpm));
  const LocalData local_data_before_migration_again = local_data;
  EXPECT_TRUE(migrator.MigrateOwnerPasswordIfNeeded(
      fake_path, &fake_tpm, &local_data, &has_migrated));
  EXPECT_FALSE(has_migrated);
  EXPECT_EQ(local_data.owner_password(),
            local_data_before_migration_again.owner_password());
}

}  //  namespace tpm_manager
