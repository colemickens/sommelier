// Copyright 2019 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "smbfs/inode_map.h"

#include <utility>

#include <base/logging.h>
#include <base/stl_util.h>

namespace smbfs {

struct InodeMap::Entry {
  Entry(ino_t inode, const base::FilePath& path) : inode(inode), path(path) {}
  ~Entry() = default;

  uint64_t refcount = 1;

  ino_t inode;
  base::FilePath path;

 private:
  DISALLOW_IMPLICIT_CONSTRUCTORS(Entry);
};

InodeMap::InodeMap(ino_t root_inode)
    : root_inode_(root_inode), seq_num_(root_inode + 1) {
  DCHECK(root_inode_);

  // Insert an entry for the root inode.
  std::unique_ptr<Entry> entry =
      std::make_unique<Entry>(root_inode, base::FilePath("/"));
  files_.emplace("/", entry.get());
  inodes_.emplace(root_inode, std::move(entry));
}

InodeMap::~InodeMap() = default;

ino_t InodeMap::IncInodeRef(const base::FilePath& path) {
  CHECK(!path.empty());
  CHECK(path.IsAbsolute());
  CHECK(!path.ReferencesParent());

  const auto it = files_.find(path.value());
  if (it != files_.end()) {
    Entry* entry = it->second;
    entry->refcount++;
    return entry->inode;
  }

  DCHECK(!base::ContainsKey(inodes_, seq_num_));

  ino_t inode = seq_num_++;
  CHECK(inode) << "Inode wrap around";
  std::unique_ptr<Entry> entry = std::make_unique<Entry>(inode, path);
  files_.emplace(path.value(), entry.get());
  inodes_.emplace(inode, std::move(entry));
  return inode;
}

base::FilePath InodeMap::GetPath(ino_t inode) const {
  const auto it = inodes_.find(inode);
  if (it == inodes_.end()) {
    return {};
  }
  return it->second->path;
}

void InodeMap::Forget(ino_t inode, uint64_t forget_count) {
  if (inode == root_inode_) {
    // Ignore the root inode.
    return;
  }

  const auto it = inodes_.find(inode);
  CHECK(it != inodes_.end());

  Entry* entry = it->second.get();
  CHECK_GE(entry->refcount, forget_count);
  entry->refcount -= forget_count;
  if (entry->refcount > 0) {
    return;
  }
  size_t removed = files_.erase(entry->path.value());
  DCHECK_EQ(removed, 1);
  inodes_.erase(it);
}

}  // namespace smbfs
