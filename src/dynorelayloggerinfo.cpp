/*
 * Copyright (c) Microsoft
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 */

#include <nlohmann/json.hpp>

#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cstdio>
#include <cstdint>
#include <iostream>
#include <sstream>
#include <string>

using json = nlohmann::json;

static std::string formatBytes(int64_t bytes) {
  if (bytes >= 1024 * 1024) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.1f MB", bytes / (1024.0 * 1024.0));
    return buf;
  } else if (bytes >= 1024) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.1f KB", bytes / 1024.0);
    return buf;
  }
  return std::to_string(bytes) + " B";
}

static std::string formatUptime(int64_t seconds) {
  int64_t days = seconds / 86400;
  int64_t hours = (seconds % 86400) / 3600;
  int64_t mins = (seconds % 3600) / 60;
  int64_t secs = seconds % 60;
  std::ostringstream os;
  if (days > 0) os << days << "d ";
  if (hours > 0 || days > 0) os << hours << "h ";
  if (mins > 0 || hours > 0 || days > 0) os << mins << "m ";
  os << secs << "s";
  return os.str();
}

static void printRow(const std::string& name, const json& s) {
  std::printf("  %-35s %8lld %10s  %8lld %10s  %8lld %10s\n",
              name.c_str(),
              (long long)s["one_minute"]["lines"].get<int64_t>(),
              formatBytes(s["one_minute"]["bytes"].get<int64_t>()).c_str(),
              (long long)s["five_minutes"]["lines"].get<int64_t>(),
              formatBytes(s["five_minutes"]["bytes"].get<int64_t>()).c_str(),
              (long long)s["one_hour"]["lines"].get<int64_t>(),
              formatBytes(s["one_hour"]["bytes"].get<int64_t>()).c_str());
}

static void printHeader(const char* label) {
  std::printf("\n  %s\n", label);
  std::printf("  %-35s %8s %10s  %8s %10s  %8s %10s\n",
              "", "── 1 min ─", "", "── 5 min ─", "", "── 1 hour ─", "");
  std::printf("  %-35s %8s %10s  %8s %10s  %8s %10s\n",
              "Entity", "lines", "bytes", "lines", "bytes", "lines", "bytes");
  std::printf("  %-35s %8s %10s  %8s %10s  %8s %10s\n",
              "-----------------------------------",
              "--------", "----------",
              "--------", "----------",
              "--------", "----------");
}

// Send a length-prefixed message and read a length-prefixed response
static std::string rpcCall(const std::string& host, int port,
                           const std::string& request) {
  int sock = ::socket(AF_INET6, SOCK_STREAM, 0);
  if (sock < 0) {
    std::perror("socket()");
    return "";
  }

  struct sockaddr_in6 addr{};
  addr.sin6_family = AF_INET6;
  addr.sin6_port = htons(port);

  // Try IPv6 first, fall back to IPv4-mapped IPv6
  if (::inet_pton(AF_INET6, host.c_str(), &addr.sin6_addr) <= 0) {
    std::string mapped = "::ffff:" + host;
    if (::inet_pton(AF_INET6, mapped.c_str(), &addr.sin6_addr) <= 0) {
      std::fprintf(stderr, "Invalid address: %s\n", host.c_str());
      ::close(sock);
      return "";
    }
  }

  if (::connect(sock, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
    std::perror("connect()");
    ::close(sock);
    return "";
  }

  // Send length-prefixed request
  int32_t req_size = static_cast<int32_t>(request.size());
  if (::write(sock, &req_size, sizeof(req_size)) != sizeof(req_size) ||
      ::write(sock, request.c_str(), req_size) != req_size) {
    std::fprintf(stderr, "Failed to send request\n");
    ::close(sock);
    return "";
  }

  // Read length-prefixed response
  int32_t resp_size = 0;
  if (::read(sock, &resp_size, sizeof(resp_size)) != sizeof(resp_size) ||
      resp_size <= 0) {
    std::fprintf(stderr, "Failed to read response size\n");
    ::close(sock);
    return "";
  }

  std::string response;
  response.resize(resp_size);
  int received = 0;
  while (received < resp_size) {
    int n = ::read(sock, &response[received], resp_size - received);
    if (n <= 0) break;
    received += n;
  }

  ::close(sock);

  if (received != resp_size) {
    std::fprintf(stderr, "Incomplete response: got %d of %d bytes\n",
                 received, resp_size);
    return "";
  }

  return response;
}

int main(int argc, char* argv[]) {
  std::string host = "127.0.0.1";
  int port = 1779;

  // Simple arg parsing: dynorelayloggerinfo [host:port]
  if (argc > 1) {
    std::string addr = argv[1];
    auto colon = addr.rfind(':');
    if (colon != std::string::npos) {
      host = addr.substr(0, colon);
      port = std::stoi(addr.substr(colon + 1));
    } else {
      host = addr;
    }
  }

  json request = {{"fn", "getStats"}};
  std::string response_str = rpcCall(host, port, request.dump());
  if (response_str.empty()) {
    std::fprintf(stderr, "Error: failed to get stats from %s:%d\n",
                 host.c_str(), port);
    return 1;
  }

  json stats;
  try {
    stats = json::parse(response_str);
  } catch (const json::parse_error& e) {
    std::fprintf(stderr, "Error: invalid response: %s\n", e.what());
    return 1;
  }

  // Header
  std::printf("DynoRelayLogger Stats (uptime: %s)\n",
              formatUptime(stats["uptime_seconds"].get<int64_t>()).c_str());

  std::string sinks_str;
  auto& active_sinks = stats["active_sinks"];
  if (active_sinks.empty()) {
    sinks_str = "none (drop mode)";
  } else {
    for (size_t i = 0; i < active_sinks.size(); ++i) {
      if (i > 0) sinks_str += ", ";
      sinks_str += active_sinks[i].get<std::string>();
    }
  }
  std::printf("  Active sinks: %s\n", sinks_str.c_str());

  // RX stats
  printHeader("RX (received from clients)");
  printRow("TOTAL", stats["aggregate"]["rx"]);
  for (const auto& e : stats["entities"]) {
    printRow(e["entity"].get<std::string>(), e["rx"]);
  }

  // TX stats
  printHeader("TX (forwarded to sinks)");
  printRow("TOTAL", stats["aggregate"]["tx"]);
  for (const auto& e : stats["entities"]) {
    printRow(e["entity"].get<std::string>(), e["tx"]);
  }

  std::printf("\n");

  return 0;
}
