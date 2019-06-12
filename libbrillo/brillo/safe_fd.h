// Copyright 2019 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// This provides an API for performing typical filesystem related tasks while
// guaranteeing certain security properties are maintained. Specifically, checks
// are performed to disallow symbolic links, and exotic file objects. The goal
// behind these checks is to thwart attacks that rely on confusing system
// services to perform unintended file operations like ownership changes or
// copy-as-root attack primitives. To accomplish this these operations are
// written to avoid susceptibility to TOCTOU (time-of-check-time-of-use)
// attacks.

// To use this API start with the root path and work from there. For example:
// SafeFD fd(SafeDirFD::Root().MakeFile(PATH).first);
// if (!fd.is_valid()) {
//   LOG(ERROR) << "Failed to open " << PATH;
//   return false;
// }
// if (fd.WriteString(CONTENTS) != SafeFD::kNoError) {
//   LOG(ERROR) << "Failed to write to " << PATH;
//   return false;
// }
// auto read_result = fd.ReadString();
// if (!read_result.second != SafeFD::kNoError) {
//   LOG(ERROR) << "Failed to read from " << PATH;
//   return false;
// }

#ifndef LIBBRILLO_BRILLO_SAFE_FD_H_
#define LIBBRILLO_BRILLO_SAFE_FD_H_

#include <fcntl.h>

#include <string>
#include <utility>
#include <vector>

#include <base/files/file_path.h>
#include <base/files/scoped_file.h>
#include <base/optional.h>
#include <base/synchronization/lock.h>
#include <brillo/brillo_export.h>

namespace brillo {

class SafeFDTest;

class SafeFD {
 public:
  enum class Error {
    kNoError = 0,
    kBadArgument,
    kNotInitialized,  // Invalid operation on a SafeFD that was not initialized.
    kIOError,         // (e.g. called OpenExistingFile on a non-existent file)
    kSymlinkDetected,
    kWrongType,  // (e.g. got a directory and expected a file)
    kWrongUID,
    kWrongGID,
    kWrongPermissions,
    kExceededMaximum,  // The maximum allowed read size was reached.
  };

  typedef std::pair<SafeFD, Error> SafeFDResult;

  // 100 MiB
  BRILLO_EXPORT static constexpr size_t kDefaultMaxRead = 100 << 20;
  // User read and write only.
  BRILLO_EXPORT static constexpr size_t kDefaultFilePermissions = 0640;
  // User read, write, and execute. Group read and execute.
  BRILLO_EXPORT static constexpr size_t kDefaultDirPermissions = 0750;

  // Get a SafeFD to the root path.
  BRILLO_EXPORT static SafeFDResult Root() WARN_UNUSED_RESULT;
  BRILLO_EXPORT static void SetRootPathForTesting(const char* new_root_path);

  // Constructs an invalid fd;
  BRILLO_EXPORT SafeFD() = default;

  // Move-based constructor and assignment.
  BRILLO_EXPORT SafeFD(SafeFD&&) = default;
  BRILLO_EXPORT SafeFD& operator=(SafeFD&&) = default;

  // Return the fd number.
  BRILLO_EXPORT int get() const WARN_UNUSED_RESULT;

  // Check the validity of the file descriptor.
  BRILLO_EXPORT bool is_valid() const WARN_UNUSED_RESULT;

  // Close the scoped file if one was open.
  BRILLO_EXPORT void reset();

  // Wrap |fd| with a SafeFD which will close the fd when this goes out of
  // scope. This closes the original fd if one was open.
  // This is named "Unsafe" because the recommended way to get a SafeFD
  // instance is opening one from SafeFD::Root().
  BRILLO_EXPORT void UnsafeReset(int fd);

  // Writes |size| bytes from |data| into a file and returns kNoError on
  // success. Note the file will be truncated to the size of the content.
  //
  // Parameters
  //  data - The buffer to write to the file.
  //  size - The number of bytes to write.
  BRILLO_EXPORT Error Write(const char* data, size_t size) WARN_UNUSED_RESULT;

  // Read the contents of the file and return it as a string.
  //
  // Parameters
  //  size - The max number of bytes to read.
  BRILLO_EXPORT std::pair<std::vector<char>, Error> ReadContents(
      size_t max_size = kDefaultMaxRead) WARN_UNUSED_RESULT;

  // Reads exactly |size| bytes into |data|.
  //
  // Parameters
  //  data - The buffer to read the file into.
  //  size - The number of bytes to read.
  BRILLO_EXPORT Error Read(char* data, size_t size) WARN_UNUSED_RESULT;

  // Open an existing file relative to this directory.
  //
  // Parameters
  //  path - The path to open relative to the current directory.
  BRILLO_EXPORT SafeFDResult OpenExistingFile(const base::FilePath& path,
                                              int flags = O_RDWR | O_CLOEXEC)
      WARN_UNUSED_RESULT;

  // Open an existing directory relative to this directory.
  //
  // Parameters
  //  path - The path to open relative to the current directory.
  BRILLO_EXPORT SafeFDResult OpenExistingDir(const base::FilePath& path,
                                             int flags = O_RDONLY | O_CLOEXEC)
      WARN_UNUSED_RESULT;

  // Open a file relative to this directory creating the parent directories and
  // file if they don't already exist.
  BRILLO_EXPORT SafeFDResult
  MakeFile(const base::FilePath& path,
           mode_t permissions = kDefaultFilePermissions,
           uid_t uid = getuid(),
           gid_t gid = getgid(),
           int flags = O_RDWR | O_CLOEXEC) WARN_UNUSED_RESULT;

  // Create the directories in the relative path with the given ownership and
  // permissions and return a file descriptor to the result.
  BRILLO_EXPORT SafeFDResult
  MakeDir(const base::FilePath& path,
          mode_t permissions = kDefaultDirPermissions,
          uid_t uid = getuid(),
          gid_t gid = getgid(),
          int flags = O_RDONLY | O_CLOEXEC) WARN_UNUSED_RESULT;

  // Hard link |fd| in the directory represented by |this| with the specified
  // name |filename|.
  //
  // Parameters
  //  data - The buffer to write to the file.
  //  size - The number of bytes to write.
  BRILLO_EXPORT Error Link(const std::string& filename,
                           int fd) WARN_UNUSED_RESULT;

  // Deletes the child path named |name|.
  //
  // Parameters
  //  name - the name of the filesystem object to delete.
  BRILLO_EXPORT Error Unlink(const std::string& name) WARN_UNUSED_RESULT;

  // Deletes a child directory.
  //
  // Parameters
  //  name - the name of the directory to delete.
  BRILLO_EXPORT Error Rmdir(const std::string& name) WARN_UNUSED_RESULT;

 private:
  BRILLO_EXPORT static const char* RootPath;

  base::ScopedFD fd_;

  DISALLOW_COPY_AND_ASSIGN(SafeFD);
};

}  // namespace brillo

#endif  // LIBBRILLO_BRILLO_SAFE_FD_H_