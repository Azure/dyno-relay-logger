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

#include "GatherSystemInfo.h"
#include "StatsCollector.h"

#include <azure/core/credentials/credentials.hpp>
#include <azure/identity/managed_identity_credential.hpp>
#include <azure/messaging/eventhubs/producer_client.hpp>

#include <atomic>
#include <condition_variable>
#include <deque>
#include <list>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

namespace dynorelaylogger {

// Azure Event Hubs client using the Azure SDK for C++.
// Authenticates via user-assigned managed identity (RBAC).
// Queues events per entity and sends accumulated batches every 30 seconds.
class AeHubsClient {
 public:
  static constexpr size_t kMaxQueueDepth = 100;
  static constexpr int kFlushIntervalSeconds = 30;

  AeHubsClient(std::shared_ptr<GatherSystemInfo> sysinfo,
               std::shared_ptr<StatsCollector> stats);
  ~AeHubsClient();

  // Start the client (creates producers and sender thread)
  void start();

  // Stop the sender thread and flush remaining events
  void stop();

  // Enqueue a JSON string for the named Event Hub entity.
  // Returns false if the queue is full (message dropped).
  bool forwardMetrics(const std::string& json_data,
                      const std::string& entity);

 private:
  // Background sender loop — flushes all entity queues every 30 seconds
  void senderLoop();

  // Flush queued events for a single entity
  void flushEntity(const std::string& entity,
                   std::deque<std::string>& queue);

  std::string eh_namespace_;
  std::list<std::string> entities_;
  std::shared_ptr<Azure::Identity::ManagedIdentityCredential> credential_;
  std::shared_ptr<StatsCollector> stats_;
  std::map<std::string, std::unique_ptr<
      Azure::Messaging::EventHubs::ProducerClient>> producers_;

  // Per-entity send queue
  std::mutex queue_mu_;
  std::map<std::string, std::deque<std::string>> queues_;

  // Sender thread
  std::unique_ptr<std::thread> sender_thread_;
  std::mutex cv_mu_;
  std::condition_variable cv_;
  std::atomic<bool> running_{false};

  std::atomic<uint64_t> bytes_sent_{0};
  bool dcgm_available_ = false;
};

}  // namespace dynorelaylogger
