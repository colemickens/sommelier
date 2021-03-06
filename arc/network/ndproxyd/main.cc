// Copyright 2020 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <base/bind.h>
#include <base/command_line.h>
#include <base/files/scoped_file.h>
#include <base/macros.h>
#include <brillo/daemons/daemon.h>

#include "arc/network/datapath.h"
#include "arc/network/minijailed_process_runner.h"
#include "arc/network/ndproxy.h"

void OnSocketReadReady(arc_networkd::NDProxy* proxy, int fd) {
  proxy->ReadAndProcessOneFrame(fd);
}

void OnGuestIpDiscovery(arc_networkd::Datapath* datapath,
                        const std::string& ifname,
                        const std::string& ip6addr) {
  if (!datapath->AddIPv6HostRoute(ifname, ip6addr, 128)) {
    LOG(WARNING) << "Failed to setup the IPv6 route for interface " << ifname;
  }
}

// Stand-alone daemon to proxy ND frames between a pair of interfaces
// Usage: ndproxyd $physical_ifname $guest_ifname
int main(int argc, char* argv[]) {
  base::CommandLine::Init(argc, argv);
  base::CommandLine* cl = base::CommandLine::ForCurrentProcess();
  base::CommandLine::StringVector args = cl->GetArgs();
  if (args.size() < 2) {
    LOG(ERROR) << "Missing command line arguments; exiting";
    return EXIT_FAILURE;
  }

  brillo::Daemon daemon;

  arc_networkd::MinijailedProcessRunner runner;
  char accept_ra_sysctl_cmd[40] = {0};
  snprintf(accept_ra_sysctl_cmd, sizeof(accept_ra_sysctl_cmd),
           "net.ipv6.conf.%s.accept_ra", args[0].c_str());
  if (runner.sysctl_w(accept_ra_sysctl_cmd, "2") != 0) {
    LOG(ERROR) << "Failed to enable " << accept_ra_sysctl_cmd << ".";
    return EXIT_FAILURE;
  }
  if (runner.sysctl_w("net.ipv6.conf.all.forwarding", "1") != 0) {
    LOG(ERROR) << "Failed to enable net.ipv6.conf.all.forwarding.";
    return EXIT_FAILURE;
  }
  arc_networkd::Datapath datapath(&runner);

  arc_networkd::NDProxy proxy;
  if (!proxy.Init()) {
    PLOG(ERROR) << "Failed to initialize NDProxy internal state";
    return EXIT_FAILURE;
  }
  proxy.AddRouterInterfacePair(args[0], args[1]);
  proxy.RegisterOnGuestIpDiscoveryHandler(
      base::Bind(&OnGuestIpDiscovery, &datapath));

  base::ScopedFD fd = arc_networkd::NDProxy::PreparePacketSocket();
  if (!fd.is_valid()) {
    PLOG(ERROR) << "Failed to initialize data socket";
    return EXIT_FAILURE;
  }

  std::unique_ptr<base::FileDescriptorWatcher::Controller> watcher =
      base::FileDescriptorWatcher::WatchReadable(
          fd.get(), base::Bind(&OnSocketReadReady, &proxy, fd.get()));

  return daemon.Run();
}
