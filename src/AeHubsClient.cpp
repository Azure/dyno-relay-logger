/*
 * Copyright (c) Microsoft
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 */

#include "AeHubsClient.h"

#include <glog/logging.h>

#include <algorithm>
#include <random>

namespace dynorelaylogger {

// ─── AeHubsClient ──────────────────────────────────────────────────────────

AeHubsClient::AeHubsClient(std::shared_ptr<GatherSystemInfo> sysinfo,
                             std::shared_ptr<StatsCollector> stats)
    : stats_(std::move(stats)) {
  dcgm_available_ = sysinfo->isDcgmAvailable();

  // Use namespace from Azure tags if available, otherwise derive from location
  eh_namespace_ = sysinfo->getEhNamespace();

  // Use user-assigned managed identity for RBAC authentication
  credential_ = std::make_shared<Azure::Identity::ManagedIdentityCredential>(
      sysinfo->getClientId());

  entities_ = {"dynolog_daemon", "dynolog_cpu_monitor", "dynolog_system_info"};
  if (dcgm_available_) {
    entities_.push_back("dynolog_dcgm_gpu_monitor");
  }
}

AeHubsClient::~AeHubsClient() {
  stop();
}

void AeHubsClient::start() {
  std::string fqns = eh_namespace_ + ".servicebus.windows.net";
  LOG(INFO) << "AeHubs: connecting to " << fqns;

  for (const auto& entity : entities_) {
    auto producer = std::make_unique<
        Azure::Messaging::EventHubs::ProducerClient>(
        fqns, entity, credential_);
    producers_[entity] = std::move(producer);
    LOG(INFO) << "AeHubs: created producer for entity: " << entity;
  }

  running_.store(true);
  sender_thread_ = std::make_unique<std::thread>(&AeHubsClient::senderLoop, this);

  LOG(INFO) << "AeHubs client started (batch flush every "
            << kFlushIntervalSeconds << "s, max queue depth "
            << kMaxQueueDepth << ")";
}

void AeHubsClient::stop() {
  if (!running_.exchange(false)) return;
  cv_.notify_all();
  if (sender_thread_ && sender_thread_->joinable()) {
    sender_thread_->join();
  }
  LOG(INFO) << "AeHubs client stopped";
}

bool AeHubsClient::forwardMetrics(const std::string& json_data,
                                   const std::string& entity) {
  std::lock_guard<std::mutex> lock(queue_mu_);
  auto& queue = queues_[entity];
  if (queue.size() >= kMaxQueueDepth) {
    if (stats_) {
      stats_->recordDrop(entity, json_data.size());
    }
    VLOG(1) << "AeHubs: queue full for " << entity << ", dropping message";
    return false;
  }
  queue.push_back(json_data);
  return true;
}

void AeHubsClient::senderLoop() {
  // Random initial delay to avoid thundering herd across fleet restarts
  {
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<int> dist(0, kFlushIntervalSeconds - 1);
    int jitter = dist(gen);
    LOG(INFO) << "AeHubs: initial jitter delay " << jitter << "s";
    std::unique_lock<std::mutex> lock(cv_mu_);
    cv_.wait_for(lock, std::chrono::seconds(jitter),
                 [this] { return !running_.load(); });
  }

  while (running_.load()) {
    {
      std::unique_lock<std::mutex> lock(cv_mu_);
      cv_.wait_for(lock, std::chrono::seconds(kFlushIntervalSeconds),
                   [this] { return !running_.load(); });
    }

    // Drain queues under lock, then send outside of lock
    std::map<std::string, std::deque<std::string>> snapshot;
    {
      std::lock_guard<std::mutex> lock(queue_mu_);
      snapshot.swap(queues_);
    }

    for (auto& [entity, queue] : snapshot) {
      if (queue.empty()) continue;
      flushEntity(entity, queue);
    }
  }

  // Final flush on shutdown
  std::map<std::string, std::deque<std::string>> remaining;
  {
    std::lock_guard<std::mutex> lock(queue_mu_);
    remaining.swap(queues_);
  }
  for (auto& [entity, queue] : remaining) {
    if (queue.empty()) continue;
    flushEntity(entity, queue);
  }
}

void AeHubsClient::flushEntity(const std::string& entity,
                                std::deque<std::string>& queue) {
  auto it = producers_.find(entity);
  if (it == producers_.end()) {
    LOG(ERROR) << "AeHubs: no producer for entity: " << entity;
    return;
  }

  try {
    Azure::Messaging::EventHubs::EventDataBatchOptions batchOptions;
    auto batch = it->second->CreateBatch(batchOptions);
    size_t batch_bytes = 0;
    size_t batch_count = 0;

    while (!queue.empty()) {
      auto& msg = queue.front();
      Azure::Messaging::EventHubs::Models::EventData event;
      event.Body = std::vector<uint8_t>(msg.begin(), msg.end());

      if (!batch.TryAdd(event)) {
        // Batch is full — send what we have, start a new one
        it->second->Send(batch);
        bytes_sent_.fetch_add(batch_bytes, std::memory_order_relaxed);
        VLOG(1) << "AeHubs: sent batch of " << batch_count
                << " events to " << entity;
        batch = it->second->CreateBatch(batchOptions);
        batch_bytes = 0;
        batch_count = 0;

        // Retry adding the current event to the fresh batch
        if (!batch.TryAdd(event)) {
          LOG(WARNING) << "AeHubs: single event too large for " << entity
                       << ", dropping (" << msg.size() << " bytes)";
          if (stats_) {
            stats_->recordDrop(entity, msg.size());
          }
          queue.pop_front();
          continue;
        }
      }

      batch_bytes += msg.size();
      batch_count++;
      queue.pop_front();
    }

    // Send remaining events in the batch
    if (batch_count > 0) {
      it->second->Send(batch);
      bytes_sent_.fetch_add(batch_bytes, std::memory_order_relaxed);
      VLOG(1) << "AeHubs: sent batch of " << batch_count
              << " events to " << entity;
    }
  } catch (const std::exception& e) {
    LOG(ERROR) << "AeHubs: failed to flush " << entity << ": " << e.what();
  }
}

}  // namespace dynorelaylogger
