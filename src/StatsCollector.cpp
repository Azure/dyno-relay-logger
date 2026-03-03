/*
 * Copyright (c) Microsoft
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 */

#include "StatsCollector.h"

#include <algorithm>

namespace dynorelaylogger {

StatsCollector::StatsCollector() : start_time_(std::time(nullptr)) {}

int64_t StatsCollector::currentMinute() const {
  return static_cast<int64_t>(std::time(nullptr)) / 60;
}

void StatsCollector::BucketRing::advance(int64_t current_minute) {
  if (last_minute == 0) {
    last_minute = current_minute;
    return;
  }
  int64_t gap = current_minute - last_minute;
  if (gap <= 0) return;
  // Clear buckets that have been skipped over
  int to_clear = std::min(gap, static_cast<int64_t>(kNumBuckets));
  for (int64_t i = 1; i <= to_clear; ++i) {
    int idx = static_cast<int>((last_minute + i) % kNumBuckets);
    buckets[idx] = {0, 0};
  }
  last_minute = current_minute;
}

void StatsCollector::BucketRing::add(int64_t current_minute, int64_t bytes) {
  advance(current_minute);
  int idx = static_cast<int>(current_minute % kNumBuckets);
  buckets[idx].lines += 1;
  buckets[idx].bytes += bytes;
}

StatsCollector::WindowResult StatsCollector::BucketRing::sum(
    int64_t current_minute, int num_minutes) const {
  WindowResult result;
  for (int i = 0; i < num_minutes; ++i) {
    int64_t minute = current_minute - i;
    // Only include buckets that are still valid (not older than last_minute - kNumBuckets)
    if (last_minute > 0 && minute < last_minute - kNumBuckets + 1) continue;
    int idx = static_cast<int>(((minute % kNumBuckets) + kNumBuckets) % kNumBuckets);
    result.lines += buckets[idx].lines;
    result.bytes += buckets[idx].bytes;
  }
  return result;
}

void StatsCollector::recordRx(const std::string& entity, int64_t bytes) {
  std::lock_guard<std::mutex> lock(mu_);
  int64_t minute = currentMinute();
  aggregate_rx_.add(minute, bytes);
  per_entity_rx_[entity].add(minute, bytes);
}

void StatsCollector::recordTx(const std::string& entity, int64_t bytes) {
  std::lock_guard<std::mutex> lock(mu_);
  int64_t minute = currentMinute();
  aggregate_tx_.add(minute, bytes);
  per_entity_tx_[entity].add(minute, bytes);
}

void StatsCollector::recordDrop(const std::string& entity, int64_t bytes) {
  std::lock_guard<std::mutex> lock(mu_);
  int64_t minute = currentMinute();
  aggregate_drops_.add(minute, bytes);
  per_entity_drops_[entity].add(minute, bytes);
}

StatsCollector::Snapshot StatsCollector::snapshot() const {
  std::lock_guard<std::mutex> lock(mu_);
  int64_t minute = currentMinute();

  Snapshot snap;
  snap.uptime_seconds = std::time(nullptr) - start_time_;

  auto fill = [&](const BucketRing& ring) -> EntityResult {
    EntityResult r;
    r.one_minute = ring.sum(minute, 1);
    r.five_minutes = ring.sum(minute, 5);
    r.one_hour = ring.sum(minute, 60);
    return r;
  };

  snap.aggregate.entity = "_aggregate";
  snap.aggregate.rx = fill(aggregate_rx_);
  snap.aggregate.tx = fill(aggregate_tx_);
  snap.aggregate.drops = fill(aggregate_drops_);

  // Collect all entity names from rx, tx, and drops maps
  std::map<std::string, bool> all_entities;
  for (const auto& [name, _] : per_entity_rx_) all_entities[name] = true;
  for (const auto& [name, _] : per_entity_tx_) all_entities[name] = true;
  for (const auto& [name, _] : per_entity_drops_) all_entities[name] = true;

  for (const auto& [name, _] : all_entities) {
    EntityRxTxResult er;
    er.entity = name;
    auto rx_it = per_entity_rx_.find(name);
    if (rx_it != per_entity_rx_.end()) er.rx = fill(rx_it->second);
    auto tx_it = per_entity_tx_.find(name);
    if (tx_it != per_entity_tx_.end()) er.tx = fill(tx_it->second);
    auto drop_it = per_entity_drops_.find(name);
    if (drop_it != per_entity_drops_.end()) er.drops = fill(drop_it->second);
    snap.entities.push_back(std::move(er));
  }

  return snap;
}

}  // namespace dynorelaylogger
