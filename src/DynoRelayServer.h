/*
 * Copyright (c) Microsoft
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 */

#pragma once

#include "MetricSink.h"
#include "StatsCollector.h"
#include "GatherSystemInfo.h"
#include "Heartbeat.h"
#include "rpc/SimpleJsonServer.h"

#include <atomic>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace dynorelaylogger {

// JSON RPC server for the info service (GetStats, port 1779)
class InfoJsonServer : public dynolog::SimpleJsonServerBase {
 public:
  InfoJsonServer(int port,
                 std::vector<std::shared_ptr<MetricSink>> sinks,
                 std::shared_ptr<StatsCollector> stats);

 protected:
  std::string processOneImpl(const std::string& request_str) override;

 private:
  std::vector<std::shared_ptr<MetricSink>> sinks_;
  std::shared_ptr<StatsCollector> stats_;
};

// Manages the Unix domain socket logger listener and JSON RPC info server
class DynoRelayServer {
 public:
  DynoRelayServer(std::vector<std::shared_ptr<MetricSink>> sinks,
                  std::shared_ptr<GatherSystemInfo> sysinfo,
                  const std::string& logger_socket_path = "/var/run/dyno-relay-logger.sock",
                  int info_port = 1779,
                  std::shared_ptr<StatsCollector> stats = nullptr);

  // Starts both servers (blocks until shutdown)
  void run();

  // Shuts down both servers
  void shutdown();

  // Get SHA256 hash of the executable for a given PID via /proc/[pid]/exe.
  // Pass pid=0 to hash /proc/self/exe.
  static std::string getExeSha256FromPid(pid_t pid);

 private:
  // Get peer credentials (PID, UID, GID) for a connected Unix socket client
  ClientId getClientId(int client_fd);

  // Handle a single client connection
  void handleClient(int client_fd);

  // Process received JSON data: detect entity, record stats, dispatch to sinks
  void processMessage(const std::string& json_data);

  // Run the Unix domain socket accept loop
  void runSocketLogger();

  std::vector<std::shared_ptr<MetricSink>> sinks_;
  std::shared_ptr<StatsCollector> stats_;
  std::string socket_path_;
  int info_port_;
  std::atomic<bool> running_{false};
  int listen_fd_ = -1;
  std::unique_ptr<InfoJsonServer> info_server_;
  std::shared_ptr<GatherSystemInfo> sysinfo_;
  std::unique_ptr<Heartbeat> heartbeat_;
  std::string hostname_;
  std::string location_;
  std::string vmid_;
  std::string session_uuid_;
  std::vector<GpuProcInfo> GPUs_;
};

}  // namespace dynorelaylogger
