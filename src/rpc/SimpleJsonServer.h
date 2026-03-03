/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <atomic>
#include <map>
#include <memory>
#include <string>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>


namespace dynolog {

// This is a simple service built using UNIX Sockets
// with remote procedure calls implemented via JSON string.

class SimpleJsonServerBase {
 public:
  explicit SimpleJsonServerBase(int port);

  virtual ~SimpleJsonServerBase();

  int getPort() const {
    return port_;
  }

  bool initSuccessful() const {
    return initSuccess_;
  }
  // spin up a new thread to process requests
  void run();

  void stop() {
    run_ = false;
    // Close the listen socket to unblock accept()
    if (sock_fd_ >= 0) {
      ::shutdown(sock_fd_, SHUT_RDWR);
      ::close(sock_fd_);
      sock_fd_ = -1;
    }
    if (thread_ && thread_->joinable()) {
      thread_->join();
    }
  }

  // synchronously processes a request
  void processOne() noexcept;

 protected:
  void initSocket();

  // process requests in a loop
  void loop() noexcept;

  // implement processing of request using the handler
  virtual std::string processOneImpl(const std::string& request_str) {
    return "";
  }

  int port_;
  int sock_fd_{-1};
  bool initSuccess_{false};

  std::atomic<bool> run_{true};
  std::unique_ptr<std::thread> thread_;
};

} // namespace dynolog
