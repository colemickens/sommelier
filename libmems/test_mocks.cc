// Copyright 2019 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "libmems/test_mocks.h"

#include <base/logging.h>

namespace libmems {
namespace mocks {

MockIioChannel::MockIioChannel(const std::string& id, bool enabled)
    : id_(id), enabled_(enabled) {}

bool MockIioChannel::SetEnabled(bool en) {
  enabled_ = en;
  return true;
}

MockIioDevice::MockIioDevice(MockIioContext* ctx,
                             const std::string& name,
                             const std::string& id)
    : IioDevice(), context_(ctx), name_(name), id_(id) {}

base::Optional<std::string> MockIioDevice::ReadStringAttribute(
    const std::string& name) const {
  auto k = text_attributes_.find(name);
  if (k == text_attributes_.end())
    return base::nullopt;
  return k->second;
}
base::Optional<int64_t> MockIioDevice::ReadNumberAttribute(
    const std::string& name) const {
  auto k = numeric_attributes_.find(name);
  if (k == numeric_attributes_.end())
    return base::nullopt;
  return k->second;
}

bool MockIioDevice::WriteStringAttribute(const std::string& name,
                                         const std::string& value) {
  text_attributes_[name] = value;
  return true;
}
bool MockIioDevice::WriteNumberAttribute(const std::string& name,
                                         int64_t value) {
  numeric_attributes_[name] = value;
  return true;
}

bool MockIioDevice::SetTrigger(IioDevice* trigger) {
  trigger_ = trigger;
  return true;
}

IioChannel* MockIioDevice::GetChannel(const std::string& id) {
  auto k = channels_.find(id);
  if (k == channels_.end())
    return nullptr;
  return k->second;
}

bool MockIioDevice::EnableBuffer(size_t n) {
  buffer_length_ = n;
  buffer_enabled_ = true;
  return true;
}
bool MockIioDevice::DisableBuffer() {
  buffer_enabled_ = false;
  return true;
}
bool MockIioDevice::IsBufferEnabled(size_t* n) const {
  if (n && buffer_enabled_)
    *n = buffer_length_;
  return buffer_enabled_;
}

void MockIioContext::AddDevice(MockIioDevice* device) {
  CHECK(device);
  devices_.emplace(device->GetName(), device);
  devices_.emplace(device->GetId(), device);
}

IioDevice* MockIioContext::GetDevice(const std::string& name) {
  auto k = devices_.find(name);
  return (k == devices_.end()) ? nullptr : k->second;
}

}  // namespace mocks
}  // namespace libmems