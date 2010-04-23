// Copyright (c) 2009 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <sys/types.h>
#include <sys/stat.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <set>
#include <string>
#include <vector>
#include <gflags/gflags.h>
#include <glib.h>
#include "base/command_line.h"
#include "chromeos/obsolete_logging.h"
#include "update_engine/delta_diff_generator.h"
#include "update_engine/delta_performer.h"
#include "update_engine/subprocess.h"
#include "update_engine/update_metadata.pb.h"
#include "update_engine/utils.h"

DEFINE_string(old_dir, "",
              "Directory where the old rootfs is loop mounted read-only");
DEFINE_string(new_dir, "",
              "Directory where the new rootfs is loop mounted read-only");
DEFINE_string(old_image, "", "Path to the old rootfs");
DEFINE_string(new_image, "", "Path to the new rootfs");
DEFINE_string(out_file, "", "Path to output file");
DEFINE_string(apply_delta, "",
              "If set, apply delta over old_image. (For debugging.)");

// This file contains a simple program that takes an old path, a new path,
// and an output file as arguments and the path to an output file and
// generates a delta that can be sent to Chrome OS clients.

using std::set;
using std::string;
using std::vector;

namespace chromeos_update_engine {

namespace {

bool IsDir(const char* path) {
  struct stat stbuf;
  TEST_AND_RETURN_FALSE_ERRNO(lstat(path, &stbuf) == 0);
  return S_ISDIR(stbuf.st_mode);
}

int Main(int argc, char** argv) {
  g_thread_init(NULL);
  google::ParseCommandLineFlags(&argc, &argv, true);
  CommandLine::Init(argc, argv);
  Subprocess::Init();
  logging::InitLogging("delta_generator.log",
                       logging::LOG_ONLY_TO_SYSTEM_DEBUG_LOG,
                       logging::DONT_LOCK_LOG_FILE,
                       logging::APPEND_TO_OLD_LOG_FILE);
  if (!FLAGS_apply_delta.empty()) {
    if (FLAGS_old_image.empty()) {
      LOG(FATAL) << "Must pass --old_image with --apply_delta.";
    }
    DeltaPerformer performer;
    CHECK_EQ(performer.Open(FLAGS_old_image.c_str(), 0, 0), 0);
    vector<char> buf(1024 * 1024);
    int fd = open(FLAGS_apply_delta.c_str(), O_RDONLY, 0);
    CHECK_GE(fd, 0);
    ScopedFdCloser fd_closer(&fd);
    for (off_t offset = 0;; offset += buf.size()) {
      ssize_t bytes_read;
      CHECK(utils::PReadAll(fd, &buf[0], buf.size(), offset, &bytes_read));
      if (bytes_read == 0)
        break;
      CHECK_EQ(performer.Write(&buf[0], bytes_read), bytes_read);
    }
    CHECK_EQ(performer.Close(), 0);
    LOG(INFO) << "done applying delta.";
    return 0;
  }
  if (FLAGS_old_dir.empty() || FLAGS_new_dir.empty() ||
      FLAGS_old_image.empty() || FLAGS_new_image.empty() ||
      FLAGS_out_file.empty()) {
    LOG(FATAL) << "Missing required argument(s)";
  }
  if ((!IsDir(FLAGS_old_dir.c_str())) || (!IsDir(FLAGS_new_dir.c_str()))) {
    LOG(FATAL) << "old_dir or new_dir not directory";
  }
  
  DeltaDiffGenerator::GenerateDeltaUpdateFile(FLAGS_old_dir,
                                              FLAGS_old_image,
                                              FLAGS_new_dir,
                                              FLAGS_new_image,
                                              FLAGS_out_file);

  return 0;
}

}  // namespace {}

}  // namespace chromeos_update_engine

int main(int argc, char** argv) {
  return chromeos_update_engine::Main(argc, argv);
}
