// Copyright (c) 2013 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "client/clock_interface.h"
#include "client/peer.h"
#include "client/peer_selector.h"
#include "common/constants.h"

#include <map>
#include <vector>

#include <base/bind.h>
#include <base/command_line.h>
#include <base/logging.h>
#include <base/rand_util.h>

using std::map;
using std::string;
using std::vector;

namespace p2p {

namespace client {

PeerSelector::PeerSelector(ServiceFinder* finder, ClockInterface* clock)
    : finder_(finder), clock_(clock) {
}

// Type used for std::sort()
struct SortPeerBySize {
  explicit SortPeerBySize(const std::string& id) : id_(id) {}

  bool operator() (const Peer *a, const Peer *b) {
    map<string, size_t>::const_iterator iter_a = a->files.find(id_);
    map<string, size_t>::const_iterator iter_b = b->files.find(id_);
    if (iter_a == a->files.end())
      return false;
    // Put all the peers without the id_ file at the end of the ordering.
    if (iter_b == b->files.end())
      return true;

    return iter_a->second > iter_b->second;
  }

  string id_;
};

string PeerSelector::PickUrlForId(const string& id, size_t minimum_size) {
  vector<string> files = finder_->AvailableFiles();

  for (auto const& file_name : files) {
    if (file_name == id) {
      vector<const Peer*> peers = finder_->GetPeersForFile(id);

      if (peers.size() > 0) {
        // Sort according to size (largest file size first)
        std::sort(peers.begin(), peers.end(), SortPeerBySize(file_name));

        // Don't consider peers with file size lower than minimum_size.
        int big_enough_files = 0;
        for (auto const& peer : peers) {
          map<string, size_t>::const_iterator file_size_it =
              peer->files.find(file_name);
          if (file_size_it != peer->files.end() &&
              file_size_it->second >= minimum_size)
            ++big_enough_files;
        }
        peers.resize(big_enough_files);

        // If we have any files left, pick randomly from the top 33%
        if (peers.size() > 0) {
          int victim_number = 0;
          int num_possible_victims = peers.size()/3 - 1;
          if (num_possible_victims > 1)
            victim_number = base::RandInt(0, num_possible_victims - 1);
          const Peer* victim = peers[victim_number];
          string address = victim->address;
          if (victim->is_ipv6)
            address = "[" + address + "]";
          return string("http://") + address + ":" +
            std::to_string(victim->port) + "/" + id;
        }
      }
    }
  }

  return "";
}

string PeerSelector::GetUrlAndWait(const string& id, size_t minimum_size) {
  LOG(INFO) << "Requesting URL in the LAN for ID " << id
            << " (minimum_size=" << minimum_size << ")";

  string url = PickUrlForId(id, minimum_size);
  int num_retries = 0;

  do {
    // If we didn't find a peer, fail
    if (url.size() == 0) {
      LOG(INFO) << "Returning error - no peer for the given ID.";
      return "";
    }

    // Only return the peer if the number of connections in the LAN
    // is below the threshold
    int num_total_conn = finder_->NumTotalConnections();
    if (num_total_conn < constants::kMaxSimultaneousDownloads) {
      LOG(INFO) << "Returning URL " << url << " after " << num_retries
                << " retries.";
      return url;
    }

    LOG(INFO) << "Found peer for the given ID but there are already "
              << num_total_conn << " download(s) in the LAN which exceeds "
              << "the threshold of "
              << constants::kMaxSimultaneousDownloads << " download(s). "
              << "Sleeping "
              << constants::kMaxSimultaneousDownloadsPollTimeSeconds
              << " seconds until retrying.";

    clock_->Sleep(constants::kMaxSimultaneousDownloadsPollTimeSeconds);

    // OK, now that we've slept for a while, the URL may not be
    // valid anymore... so we do the lookup again
    finder_->Lookup();
    url = PickUrlForId(id, minimum_size);
    num_retries++;
  } while (true);
}

}  // namespace client

}  // namespace p2p
