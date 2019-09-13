// Copyright 2019 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "diagnostics/dpsl/test_utils/common.h"

#include <iostream>
#include <memory>
#include <string>
#include <utility>

#include <base/json/json_reader.h>
#include <base/json/json_writer.h>
#include <base/values.h>
#include <google/protobuf/message.h>
#include <google/protobuf/util/json_util.h>

namespace diagnostics {
namespace test_utils {

bool PrintProto(const google::protobuf::Message& message) {
  // Convert the Proto to JSON.
  std::string body_json;
  auto status =
      google::protobuf::util::MessageToJsonString(message, &body_json);
  if (!status.ok()) {
    std::cerr << "Failed to convert proto to JSON: " << status << "\n";
    return false;
  }

  // Then convert the JSON back to a base::Value.
  std::unique_ptr<base::Value> body = base::JSONReader::Read(body_json);
  if (!body) {
    std::cerr << "Failed to parse JSON to base::Value: " << body_json << '\n';
    return false;
  }

  // Embed the body and name of the proto in a base::DictionaryValue.
  base::DictionaryValue dv;
  dv.SetString("name", message.GetDescriptor()->name());
  dv.Set("body", std::move(body));

  // Serialize the base::DictionaryValue back to JSON.
  std::string message_json;
  base::JSONWriter::WriteWithOptions(dv, base::JSONWriter::OPTIONS_PRETTY_PRINT,
                                     &message_json);
  std::cout << message_json;

  return true;
}

}  // namespace test_utils
}  // namespace diagnostics