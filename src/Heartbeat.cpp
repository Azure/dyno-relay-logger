/*
 * Copyright (c) Microsoft
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 */

#include "Heartbeat.h"

#include <glog/logging.h>

namespace dynorelaylogger {

Heartbeat::Heartbeat(std::shared_ptr<GatherSystemInfo> sysinfo,
                     std::shared_ptr<StatsCollector> stats,
                     MessageCallback callback,
                     std::chrono::seconds interval)
    : sysinfo_(std::move(sysinfo)),
      stats_(std::move(stats)),
      callback_(std::move(callback)),
      interval_(interval) {}

Heartbeat::~Heartbeat() {
  stop();
}

void Heartbeat::start() {
  running_.store(true);
  thread_ = std::make_unique<std::thread>(&Heartbeat::loop, this);
  LOG(INFO) << "Heartbeat started (interval: " << interval_.count() << "s)";
}

void Heartbeat::stop() {
  if (running_.load()) {
    running_.store(false);
    cv_.notify_all();
    if (thread_ && thread_->joinable()) {
      thread_->join();
    }
    LOG(INFO) << "Heartbeat stopped";
  }
}

nlohmann::json Heartbeat::buildDynologDaemonJson(const nlohmann::json& info) {
  std::string hostname, location, vmid, session_uuid;
  sysinfo_->getHostLocationId(hostname, location, vmid, session_uuid);
  auto snap = stats_->snapshot();

  nlohmann::json hb;
  hb["machine_id"] = info.value("machine_id", "");
  hb["hostname"] = hostname;
  hb["location"] = location;
  hb["vmid"] = vmid;
  hb["session_uuid"] = session_uuid;
  hb["t"] = std::time(nullptr);
  hb["rx_bytes_1m"] = snap.aggregate.rx.one_minute.bytes;
  hb["rx_bytes_5m"] = snap.aggregate.rx.five_minutes.bytes;
  hb["rx_bytes_1h"] = snap.aggregate.rx.one_hour.bytes;
  hb["tx_bytes_1m"] = snap.aggregate.tx.one_minute.bytes;
  hb["tx_bytes_5m"] = snap.aggregate.tx.five_minutes.bytes;
  hb["tx_bytes_1h"] = snap.aggregate.tx.one_hour.bytes;

  return hb;
}

nlohmann::json Heartbeat::buildDynologSystemInfoJson() {
  nlohmann::json info = sysinfo_->getAzureSystemInfo();
  info["t"] = std::time(nullptr);

  auto gpus = sysinfo_->getGpuInfo();
  info["nvidia_gpu_count"] = static_cast<int>(gpus.size());
  nlohmann::json gpu_array = nlohmann::json::array();
  for (int i = 0; i < static_cast<int>(gpus.size()); ++i) {
    const auto& gpu = gpus[i];
    gpu_array.push_back({
        {"number", i},
        {"model", gpu.model},
        {"uuid", gpu.uuid},
        {"irq", gpu.irq},
        {"bios", gpu.bios},
        {"bus_type", gpu.bus_type},
        {"bus_location", gpu.bus_location},
        {"device_minor", gpu.device_minor},
        {"firmware", gpu.firmware}
    });
  }
  info["nvidia_gpu_info"] = gpu_array;

  auto nics = sysinfo_->getNicInfo();
  nlohmann::json nic_array = nlohmann::json::array();
  for (int i = 0; i < static_cast<int>(nics.size()); ++i) {
    const auto& nic = nics[i];
    nic_array.push_back({
        {"number", i},
        {"interface", nic.interface},
        {"device_id", nic.device_id},
        {"firmware", nic.firmware},
        {"numa_node", nic.numa_node},
        {"speed", nic.speed},
        {"state", nic.state},
        {"sys_image_guid", nic.sys_image_guid}
    });
  }
  info["nic_info"] = nic_array;
  info["client_info"] = getClientIds();

  return info;
}

void Heartbeat::loop() {
  // Wait 60 seconds before first heartbeat
  {
    std::unique_lock<std::mutex> lock(mu_);
    cv_.wait_for(lock, std::chrono::seconds(60), [this] { return !running_.load(); });
    if (!running_.load()) return;
  }

  // Send immediately on startup, then every interval
  while (running_.load()) {
    try {
      nlohmann::json info = buildDynologSystemInfoJson();

      std::string msg = "::dynolog_system_info," + info.dump();
      callback_(msg);
      LOG(INFO) << "Heartbeat sent system info";

      // Send daemon stats heartbeat
      nlohmann::json hb = buildDynologDaemonJson(info);

      std::string hb_msg = "::dynolog_daemon," + hb.dump();
      callback_(hb_msg);
      LOG(INFO) << "Heartbeat sent daemon stats";
    } catch (const std::exception& e) {
      LOG(WARNING) << "Heartbeat failed: " << e.what();
    }

    // Sleep for the interval, but wake up early if stopped
    std::unique_lock<std::mutex> lock(mu_);
    cv_.wait_for(lock, interval_, [this] { return !running_.load(); });

    removeOldClientIds();
  }
}

std::shared_ptr<ClientId> Heartbeat::saveClientId(std::shared_ptr<ClientId> client_id) {
  std::lock_guard<std::mutex> lock(clientIds_mutex_);
  if (clientIds_.find(client_id->exe_sha256) == clientIds_.end()) {
    clientIds_[client_id->exe_sha256] = client_id;
  } else {
    clientIds_[client_id->exe_sha256]->last_seen = client_id->last_seen;
  }

  return clientIds_[client_id->exe_sha256];
}

nlohmann::json Heartbeat::getClientIds() {
  std::lock_guard<std::mutex> lock(clientIds_mutex_);
  nlohmann::json result = nlohmann::json::array();
  for (const auto& [key, client] : clientIds_) {
    result.push_back({
        {"pid", client->pid},
        {"uid", client->uid},
        {"gid", client->gid},
        {"process_name", client->process_name},
        {"exe_sha256", client->exe_sha256},
        {"last_seen", client->last_seen},
    });
  }
  return result;
}

void Heartbeat::removeOldClientIds() {
  std::lock_guard<std::mutex> lock(clientIds_mutex_);
  std::time_t now = std::time(nullptr);
  for (auto it = clientIds_.begin(); it != clientIds_.end(); ) {
    if (now - it->second->last_seen > 3600) {
      it = clientIds_.erase(it);
    } else {
      ++it;
    }
  }
}

}  // namespace dynorelaylogger
