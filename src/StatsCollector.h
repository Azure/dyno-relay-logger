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

#include <array>
#include <chrono>
#include <ctime>
#include <map>
#include <mutex>
#include <string>
#include <vector>

namespace dynorelaylogger {

// Thread-safe statistics collector using rolling 1-minute buckets.
// Maintains 60 buckets (1 hour) per entity and in aggregate.
// Tracks both rx (received) and tx (forwarded) separately.
class StatsCollector {
 public:
  StatsCollector();

  // Record a received metric (thread-safe)
  void recordRx(const std::string& entity, int64_t bytes);

  // Record a forwarded metric (thread-safe)
  void recordTx(const std::string& entity, int64_t bytes);

  // Record a dropped metric (thread-safe)
  void recordDrop(const std::string& entity, int64_t bytes);

  struct WindowResult {
    int64_t lines = 0;
    int64_t bytes = 0;
  };

  struct EntityResult {
    std::string entity;
    WindowResult one_minute;
    WindowResult five_minutes;
    WindowResult one_hour;
  };

  struct EntityRxTxResult {
    std::string entity;
    EntityResult rx;
    EntityResult tx;
    EntityResult drops;
  };

  struct Snapshot {
    EntityRxTxResult aggregate;
    std::vector<EntityRxTxResult> entities;
    int64_t uptime_seconds;
  };

  // Get a consistent snapshot of all stats (thread-safe)
  Snapshot snapshot() const;

 private:
  static constexpr int kNumBuckets = 60;

  struct Bucket {
    int64_t lines = 0;
    int64_t bytes = 0;
  };

  struct BucketRing {
    std::array<Bucket, kNumBuckets> buckets{};
    int64_t last_minute = 0;  // minute index of most recent write

    void advance(int64_t current_minute);
    void add(int64_t current_minute, int64_t bytes);
    WindowResult sum(int64_t current_minute, int num_minutes) const;
  };

  int64_t currentMinute() const;

  mutable std::mutex mu_;
  BucketRing aggregate_rx_;
  BucketRing aggregate_tx_;
  BucketRing aggregate_drops_;
  std::map<std::string, BucketRing> per_entity_rx_;
  std::map<std::string, BucketRing> per_entity_tx_;
  std::map<std::string, BucketRing> per_entity_drops_;
  std::time_t start_time_;
};

}  // namespace dynorelaylogger
