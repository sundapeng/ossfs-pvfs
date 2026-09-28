/*
 * Copyright 2025 The Ossfs Authors
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#pragma once

#include <gtest/gtest.h>
#include <photon/net/http/server.h>
#include <photon/net/socket.h>
#include <photon/photon.h>
#include <photon/thread/thread.h>

#include <atomic>
#include <chrono>
#include <functional>
#include <string>
#include <vector>

#include "common/macros.h"

// In-process HTTP server standing in for the DLF REST catalog.
class RestStub : public photon::net::http::HTTPHandler {
 public:
  using Route = std::function<std::pair<int, std::string>(
      const std::string& target, const std::string& query)>;
  Route route;
  std::vector<std::string> requests;  // "target?query" in arrival order
  std::string last_security_token;    // x-oss-security-token of the last request
  std::vector<std::string> user_agents;  // User-Agent header, per request
  std::atomic<bool> stopping{false};
  std::atomic<int> active{0};

  // Delay a reply from inside a route without outliving the server.
  void hold_ms(int ms) {
    for (int i = 0; i < ms / 10 && !stopping.load(); i++) {
      photon::thread_usleep(10 * 1000);
    }
  }

  int handle_request(photon::net::http::Request& req,
                     photon::net::http::Response& resp,
                     std::string_view) override {
    active++;
    DEFER(active--);
    // target() carries the query string; keep path and query apart.
    std::string target(req.target());
    std::string query;
    auto qpos = target.find('?');
    if (qpos != std::string::npos) {
      query = target.substr(qpos + 1);
      target = target.substr(0, qpos);
    }
    requests.push_back(query.empty() ? target : target + "?" + query);
    last_security_token = std::string(req.headers["x-oss-security-token"]);
    user_agents.emplace_back(req.headers["User-Agent"]);
    auto [code, body] = route(target, query);
    resp.set_result(code);
    resp.headers.insert("Content-Type", "application/json");
    resp.headers.insert("Content-Length", std::to_string(body.size()));
    resp.keep_alive(true);
    if (body.empty()) return 0;
    return resp.write(body.data(), body.size()) == (ssize_t)body.size() ? 0
                                                                        : -1;
  }
};

// Starts the stub on a loopback port of the current Photon vCPU; every
// request is served on that vCPU, so the test thread must stay Photon-aware
// (blocking in a Photon call is fine, a bare std::thread::join is not).
class RestStubFixture : public ::testing::Test {
 protected:
  RestStub stub_;
  photon::net::ISocketServer* tcp_ = nullptr;
  photon::net::http::HTTPServer* http_ = nullptr;
  uint16_t port_ = 0;

  void start() {
    tcp_ = photon::net::new_tcp_socket_server();
    ASSERT_NE(tcp_, nullptr);
    ASSERT_EQ(tcp_->bind_v4localhost(0), 0);
    ASSERT_EQ(tcp_->listen(), 0);
    photon::net::EndPoint ep;
    ASSERT_EQ(tcp_->getsockname(ep), 0);
    port_ = ep.port;
    http_ = photon::net::http::new_http_server();
    ASSERT_NE(http_, nullptr);
    http_->add_handler(&stub_);
    tcp_->set_handler(http_->get_connection_handler());
    ASSERT_EQ(tcp_->start_loop(false), 0);
  }

  void stop() {
    if (!tcp_) return;
    stub_.stopping = true;
    for (int i = 0; i < 1000 && stub_.active.load() > 0; i++) {
      photon::thread_usleep(10 * 1000);
    }
    tcp_->terminate();
    delete http_;
    delete tcp_;
    http_ = nullptr;
    tcp_ = nullptr;
  }

  std::string endpoint() const {
    return "http://127.0.0.1:" + std::to_string(port_);
  }

  static std::pair<int, std::string> json(int code, const std::string& body) {
    return {code, body};
  }

  static int64_t ms_since(std::chrono::steady_clock::time_point t0) {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now() - t0)
        .count();
  }
};
