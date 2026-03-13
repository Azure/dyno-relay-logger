// Copyright (c) Microsoft
//
// Licensed under the Apache License, Version 2.0
//
// Example: reads /proc/stat every N seconds, computes CPU usage deltas in
// milliseconds, and emits JSON to stdout or to dyno-relay-logger via its
// Unix domain socket.  The wire protocol matches dynolog's UdsRelayLogger:
//
//   ::category_name,{json}\n
//
// Usage:
//   ./cpu_logger my_cpu_monitor                        # stdout (default)
//   ./cpu_logger --output=relay my_cpu_monitor         # relay to logger
//   ./cpu_logger --output=relay --socket=/tmp/test.sock --interval=5 my_cpu

#include <gflags/gflags.h>
#include <nlohmann/json.hpp>

#include <cerrno>
#include <csignal>
#include <cstring>
#include <ctime>
#include <atomic>
#include <chrono>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <thread>
#include <array>

#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

DEFINE_string(output, "stdout", "Output destination: 'stdout' or 'relay'");
DEFINE_string(socket, "/var/run/dyno-relay-logger.sock",
              "Unix domain socket path for relay mode");
DEFINE_int32(interval, 10, "Polling interval in seconds");

static std::atomic<bool> g_running{true};

static void signalHandler(int /*sig*/) {
  g_running.store(false);
}

// ── /proc/stat fields ───────────────────────────────────────────────────────

struct CpuSample {
  int64_t user = 0;
  int64_t nice = 0;
  int64_t system = 0;
  int64_t idle = 0;
  int64_t iowait = 0;
  int64_t irq = 0;
  int64_t softirq = 0;
  int64_t steal = 0;
};

static bool readCpuSample(CpuSample& out) {
  std::ifstream proc("/proc/stat");
  if (!proc.is_open()) {
    return false;
  }
  std::string line;
  if (!std::getline(proc, line)) {
    return false;
  }
  // First line: "cpu  user nice system idle iowait irq softirq steal ..."
  std::istringstream iss(line);
  std::string label;
  iss >> label >> out.user >> out.nice >> out.system >> out.idle >> out.iowait
      >> out.irq >> out.softirq >> out.steal;
  return iss.good() || iss.eof();
}

static int64_t jiffiesToMs(int64_t jiffies, long ticksPerSec) {
  return jiffies * 1000 / ticksPerSec;
}

// ── Socket helper (modeled on dynolog UdsRelayLogger) ───────────────────────

class RelaySocket {
 public:
  explicit RelaySocket(const std::string& path) : path_(path) {}

  ~RelaySocket() { close(); }

  // Send a formatted relay message.  Returns true on success.
  bool send(const std::string& category, const std::string& json) {
    if (!ensureConnected()) {
      return false;
    }
    std::string msg = "::" + category + "," + json + "\n";
    ssize_t n =
        ::send(fd_, msg.data(), msg.size(), MSG_NOSIGNAL);
    if (n < 0 || static_cast<size_t>(n) != msg.size()) {
      std::cerr << "cpu_logger: send failed: " << std::strerror(errno)
                << std::endl;
      close();
      return false;
    }
    return true;
  }

  void close() {
    if (fd_ >= 0) {
      ::close(fd_);
      fd_ = -1;
    }
  }

 private:
  static constexpr int kReconnectIntervalSec = 120;

  bool ensureConnected() {
    if (fd_ >= 0) {
      return true;
    }

    auto now = std::time(nullptr);
    if (now - lastAttempt_ < kReconnectIntervalSec && lastAttempt_ != 0) {
      return false;
    }
    lastAttempt_ = now;

    fd_ = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd_ < 0) {
      std::cerr << "cpu_logger: socket() failed: " << std::strerror(errno)
                << std::endl;
      return false;
    }

    struct sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    std::strncpy(addr.sun_path, path_.c_str(), sizeof(addr.sun_path) - 1);

    if (::connect(fd_, reinterpret_cast<struct sockaddr*>(&addr),
                  sizeof(addr)) < 0) {
      std::cerr << "cpu_logger: connect(" << path_
                << ") failed: " << std::strerror(errno) << std::endl;
      ::close(fd_);
      fd_ = -1;
      return false;
    }

    std::cerr << "cpu_logger: connected to " << path_ << std::endl;
    return true;
  }

  std::string path_;
  int fd_ = -1;
  std::time_t lastAttempt_ = 0;
};

// ── Hostname helper ─────────────────────────────────────────────────────────

static std::string getHostname() {
  std::array<char, 256> buf{};
  if (::gethostname(buf.data(), buf.size()) == 0) {
    return std::string(buf.data());
  }
  return "unknown";
}

// ── Main ────────────────────────────────────────────────────────────────────

int main(int argc, char* argv[]) {
  gflags::SetUsageMessage(
      "CPU metrics logger example.\n"
      "Usage: cpu_logger [flags] <category_name>\n"
      "  category_name   Required logger category (entity name).");
  gflags::ParseCommandLineFlags(&argc, &argv, true);

  if (argc < 2) {
    std::cerr << "Error: category name is required.\n"
              << "Usage: " << argv[0] << " [flags] <category_name>"
              << std::endl;
    return 1;
  }
  const std::string category = argv[1];

  if (FLAGS_output != "stdout" && FLAGS_output != "relay") {
    std::cerr << "Error: --output must be 'stdout' or 'relay'" << std::endl;
    return 1;
  }

  std::signal(SIGINT, signalHandler);
  std::signal(SIGTERM, signalHandler);

  const std::string hostname = getHostname();
  const long ticksPerSec = sysconf(_SC_CLK_TCK);

  std::unique_ptr<RelaySocket> relay;
  if (FLAGS_output == "relay") {
    relay = std::make_unique<RelaySocket>(FLAGS_socket);
  }

  // Take initial baseline sample.
  CpuSample prev{};
  if (!readCpuSample(prev)) {
    std::cerr << "Error: cannot read /proc/stat" << std::endl;
    return 1;
  }

  std::cerr << "cpu_logger: category=" << category
            << " output=" << FLAGS_output
            << " interval=" << FLAGS_interval << "s" << std::endl;

  while (g_running.load()) {
    for (int i = 0; i < FLAGS_interval * 10 && g_running.load(); ++i) {
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    if (!g_running.load()) {
      break;
    }

    CpuSample cur{};
    if (!readCpuSample(cur)) {
      std::cerr << "Warning: failed to read /proc/stat" << std::endl;
      continue;
    }

    auto delta = [&](int64_t CpuSample::*field) {
      return cur.*field - prev.*field;
    };

    int64_t d_user = delta(&CpuSample::user);
    int64_t d_nice = delta(&CpuSample::nice);
    int64_t d_system = delta(&CpuSample::system);
    int64_t d_idle = delta(&CpuSample::idle);
    int64_t d_iowait = delta(&CpuSample::iowait);
    int64_t d_irq = delta(&CpuSample::irq);
    int64_t d_softirq = delta(&CpuSample::softirq);
    int64_t d_steal = delta(&CpuSample::steal);

    int64_t busy = d_user + d_nice + d_system + d_irq + d_softirq + d_steal;
    int64_t total = busy + d_idle + d_iowait;

    nlohmann::json j;
    j["hostname"] = hostname;
    j["t"] = static_cast<int64_t>(std::time(nullptr));
    j["interval_s"] = FLAGS_interval;
    j["cpu_user_ms"] = jiffiesToMs(d_user, ticksPerSec);
    j["cpu_nice_ms"] = jiffiesToMs(d_nice, ticksPerSec);
    j["cpu_system_ms"] = jiffiesToMs(d_system, ticksPerSec);
    j["cpu_idle_ms"] = jiffiesToMs(d_idle, ticksPerSec);
    j["cpu_iowait_ms"] = jiffiesToMs(d_iowait, ticksPerSec);
    j["cpu_irq_ms"] = jiffiesToMs(d_irq, ticksPerSec);
    j["cpu_softirq_ms"] = jiffiesToMs(d_softirq, ticksPerSec);
    j["cpu_steal_ms"] = jiffiesToMs(d_steal, ticksPerSec);
    j["cpu_busy_ms"] = jiffiesToMs(busy, ticksPerSec);
    j["cpu_total_ms"] = jiffiesToMs(total, ticksPerSec);

    std::string payload = j.dump();

    if (FLAGS_output == "stdout") {
      std::cout << "::" << category << "," << payload << std::endl;
    } else {
      if (!relay->send(category, payload)) {
        std::cerr << "Warning: relay send failed, will retry" << std::endl;
      }
    }

    prev = cur;
  }

  std::cerr << "cpu_logger: shutting down" << std::endl;
  return 0;
}
