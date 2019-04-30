// Copyright (c) 2013 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "debugd/src/anonymizer_tool.h"

#include <pcrecpp.h>

#include <base/files/file_path.h>
#include <base/strings/string_number_conversions.h>
#include <base/strings/string_util.h>
#include <base/strings/stringprintf.h>

using std::string;

namespace debugd {

namespace {

// The |kCustomPatterns| array defines patterns to match and anonymize. Each
// pattern needs to define three capturing parentheses groups:
//
// - a group for the pattern before the identifier to be anonymized;
// - a group for the identifier to be anonymized;
// - a group for the pattern after the identifier to be anonymized.
//
// Every matched identifier (in the context of the whole pattern) is anonymized
// by replacing it with an incremental instance identifier. Every different
// pattern defines a separate instance identifier space. See the unit test for
// AnonymizerTool::AnonymizeCustomPattern for pattern anonymization examples.
//
// Useful regular expression syntax:
//
// +? is a non-greedy (lazy) +.
// \b matches a word boundary.
// (?i) turns on case insensitivy for the remainder of the regex.
// (?-s) turns off "dot matches newline" for the remainder of the regex.
// (?:regex) denotes non-capturing parentheses group.
const char* const kCustomPatterns[] = {
  "(\\bCell ID: ')([0-9a-fA-F]+)(')",  // ModemManager
  "(\\bLocation area code: ')([0-9a-fA-F]+)(')",  // ModemManager
  "(?i-s)(\\bssid[= ]')(.+)(')",  // wpa_supplicant
  "(?-s)(\\bSSID - hexdump\\(len=[0-9]+\\): )(.+)()",  // wpa_supplicant
  "(?-s)(\\[SSID=)(.+?)(\\])",  // shill
};

const char* const kNonAnonymizedMacAddresses[] = {
  "00:00:00:00:00:00",
  "ff:ff:ff:ff:ff:ff",
};

}  // namespace

AnonymizerTool::AnonymizerTool()
    : custom_patterns_(arraysize(kCustomPatterns)) {
  // Identity-map these, so we don't mangle them.
  for (const char* mac : kNonAnonymizedMacAddresses) {
    mac_addresses_[mac] = mac;
  }
}

string AnonymizerTool::Anonymize(const string& input) {
  string anonymized = AnonymizeMACAddresses(input);
  anonymized = AnonymizeCustomPatterns(anonymized);
  anonymized = AnonymizeAndroidAppStoragePaths(anonymized);
  return anonymized;
}

string AnonymizerTool::AnonymizeMACAddresses(const string& input) {
  // This regular expression finds the next MAC address. It splits the data into
  // a section preceding the MAC address, an OUI (Organizationally Unique
  // Identifier) part and a NIC (Network Interface Controller) specific part.
  pcrecpp::RE mac_re("(.*?)("
                     "[0-9a-fA-F][0-9a-fA-F]:"
                     "[0-9a-fA-F][0-9a-fA-F]:"
                     "[0-9a-fA-F][0-9a-fA-F]):("
                     "[0-9a-fA-F][0-9a-fA-F]:"
                     "[0-9a-fA-F][0-9a-fA-F]:"
                     "[0-9a-fA-F][0-9a-fA-F])",
                     pcrecpp::RE_Options()
                     .set_multiline(true)
                     .set_dotall(true));

  string result;
  result.reserve(input.size());

  // Keep consuming, building up a result string as we go.
  pcrecpp::StringPiece text(input);
  string pre_mac, oui, nic;
  while (mac_re.Consume(&text, &pre_mac, &oui, &nic)) {
    // Look up the MAC address in the hash.
    oui = base::ToLowerASCII(oui);
    nic = base::ToLowerASCII(nic);
    string mac = oui + ":" + nic;
    string replacement_mac = mac_addresses_[mac];
    if (replacement_mac.empty()) {
      // If not found, anonymize the MAC address by printing out the OUI as-is
      // and then an anonymized integer for the NIC part.
      int mac_id = mac_addresses_.size() -
                   arraysize(kNonAnonymizedMacAddresses);
      replacement_mac =
          base::StringPrintf("[MAC OUI=%s IFACE=%d]", oui.c_str(), mac_id);
      mac_addresses_[mac] = replacement_mac;
    }

    result += pre_mac;
    result += replacement_mac;
  }

  result.append(text.data(), text.size());
  return result;
}

string AnonymizerTool::AnonymizeCustomPatterns(const string& input) {
  string anonymized = input;
  for (size_t i = 0; i < arraysize(kCustomPatterns); i++) {
    anonymized = AnonymizeCustomPattern(anonymized,
                                        kCustomPatterns[i],
                                        &custom_patterns_[i]);
  }
  return anonymized;
}

string AnonymizerTool::AnonymizeAndroidAppStoragePaths(const string& input) {
  // This is for anonymizing 'android_app_storage' output. When the path starts
  // either /home/root/<hash>/data/data/<package_name>/ or
  // /home/root/<hash>/data/user_de/<number>/<package_name>/, this function will
  // anonymize path components following <package_name>/.
  pcrecpp::RE path_re("(.*?)("
                      "\\t/home/root/[\\da-f]+/android-data/data/"
                      "(data|user_de/\\d+)/[^/\\n]+)("
                      "/[^\\n]+)",
                     pcrecpp::RE_Options()
                     .set_multiline(true)
                     .set_dotall(true));

  string result;
  result.reserve(input.size());

  // Keep consuming, building up a result string as we go.
  pcrecpp::StringPiece text(input);
  string pre_path, path_prefix, ignored, app_specific;
  while (path_re.Consume(
             &text, &pre_path, &path_prefix, &ignored, &app_specific)) {
    // We can record these parts as-is.
    result += pre_path;
    result += path_prefix;

    // |app_specific| has to be anonymized. First, convert it into components,
    // and then anonymize each component as follows:
    // - If the component has a non-ASCII character, change it to '*'.
    // - Otherwise, remove all the characters in the component but the first
    //   one.
    // - If the original component has 2 or more bytes, add '_'.
    const base::FilePath path(app_specific);
    std::vector<string> components;
    path.GetComponents(&components);
    DCHECK(!components.empty());

    auto it = components.begin() + 1;  // ignore the leading slash
    for (; it != components.end(); ++it) {
      const auto& component = *it;
      DCHECK(!component.empty());
      result += '/';
      result += (base::IsStringASCII(component) ? component[0] : '*');
      if (component.length() > 1)
        result += '_';
    }
  }

  result.append(text.data(), text.size());
  return result;
}

// static
string AnonymizerTool::AnonymizeCustomPattern(
    const string& input,
    const string& pattern,
    std::map<string, string>* identifier_space) {
  pcrecpp::RE re("(.*?)" + pattern,
                 pcrecpp::RE_Options()
                 .set_multiline(true)
                 .set_dotall(true));
  DCHECK_EQ(4, re.NumberOfCapturingGroups());

  string result;
  result.reserve(input.size());

  // Keep consuming, building up a result string as we go.
  pcrecpp::StringPiece text(input);
  string pre_match, pre_matched_id, matched_id, post_matched_id;
  while (re.Consume(&text, &pre_match,
                    &pre_matched_id, &matched_id, &post_matched_id)) {
    string replacement_id = (*identifier_space)[matched_id];
    if (replacement_id.empty()) {
      replacement_id = base::IntToString(identifier_space->size());
      (*identifier_space)[matched_id] = replacement_id;
    }

    result += pre_match;
    result += pre_matched_id;
    result += replacement_id;
    result += post_matched_id;
  }
  result.append(text.data(), text.size());
  return result;
}

}  // namespace debugd
