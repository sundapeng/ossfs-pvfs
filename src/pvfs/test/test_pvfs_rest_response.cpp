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

#include <gtest/gtest.h>
#include <photon/net/socket.h>
#include <photon/photon.h>
#include <photon/thread/thread.h>

#include <functional>
#include <sstream>
#include <string>
#include <vector>

#include "common/macros.h"
#include "pvfs/paimon_rest_client.h"

using PvfsFileSystem::PaimonRESTClient;
using PvfsFileSystem::PaimonRESTClientOptions;

namespace {

// Raw HTTP framing lets a reply end without the final chunk marker.
class PvfsRestResponseTest : public ::testing::Test {
 protected:
  using Stream = photon::net::ISocketStream;
  photon::net::ISocketServer *server_ = nullptr;
  std::function<int(Stream *)> reply_;
  int active_ = 0;

  static bool write(Stream *stream, const std::string &data) {
    return stream->write(data.data(), data.size()) ==
           static_cast<ssize_t>(data.size());
  }

  static bool chunk(Stream *stream, const std::string &data) {
    std::ostringstream size;
    size << std::hex << data.size() << "\r\n";
    return write(stream, size.str()) && write(stream, data) &&
           write(stream, "\r\n");
  }

  int serve(Stream *stream) {
    active_++;
    DEFER(active_--);
    stream->timeout(5 * 1000 * 1000);
    std::string request;
    char buffer[4096];
    while (request.find("\r\n\r\n") == std::string::npos) {
      auto n = stream->recv(buffer, sizeof(buffer));
      if (n <= 0) return -1;
      request.append(buffer, n);
    }
    if (request.find("GET /v1/config?") == 0) {
      const std::string body = R"({"defaults":{"prefix":"pfx"}})";
      return write(stream,
                   "HTTP/1.1 200 OK\r\nConnection: close\r\n"
                   "Content-Length: " +
                       std::to_string(body.size()) + "\r\n\r\n" + body)
                 ? 0
                 : -1;
    }
    if (!write(stream,
               "HTTP/1.1 200 OK\r\nConnection: close\r\n"
               "Transfer-Encoding: chunked\r\n"
               "Content-Type: application/json\r\n\r\n"))
      return -1;
    if (reply_) return reply_(stream);
    return chunk(stream, R"({"databases":[]})") && write(stream, "0\r\n\r\n")
               ? 0
               : -1;
  }

  void start() {
    server_ = photon::net::new_tcp_socket_server();
    ASSERT_NE(server_, nullptr);
    ASSERT_EQ(server_->bind_v4localhost(0), 0);
    ASSERT_EQ(server_->listen(), 0);
    server_->set_handler({this, &PvfsRestResponseTest::serve});
    ASSERT_EQ(server_->start_loop(false), 0);
  }

  void stop() {
    if (!server_) return;
    server_->terminate();
    while (active_ > 0) photon::thread_usleep(1000);
    delete server_;
    server_ = nullptr;
  }

  PaimonRESTClientOptions opts() {
    PaimonRESTClientOptions options;
    options.endpoint =
        "http://127.0.0.1:" + std::to_string(server_->getsockname().port);
    options.region = "cn-test";
    options.catalog = "cat";
    options.access_key_id = "ak";
    options.access_key_secret = "sk";
    options.signing_algorithm = "default";
    options.timeout_ms = 5000;
    return options;
  }
};

TEST_F(PvfsRestResponseTest, ChunkedBodyOverLimitIsRejected) {
  INIT_PHOTON();
  start();
  DEFER(stop());
  PaimonRESTClient client(opts());
  ASSERT_EQ(client.init(), 0);
  reply_ = [](Stream *stream) {
    const std::string padding(1UL << 20, ' ');
    for (int i = 0; i < 64; i++) {
      if (!chunk(stream, padding)) return -1;
    }
    return chunk(stream, "{}") && write(stream, "0\r\n\r\n") ? 0 : -1;
  };
  EXPECT_EQ(client.get_database("db"), -EMSGSIZE);
}

TEST_F(PvfsRestResponseTest, InterruptedChunkedBodyIsRejected) {
  INIT_PHOTON();
  start();
  DEFER(stop());
  PaimonRESTClient client(opts());
  ASSERT_EQ(client.init(), 0);
  reply_ = [](Stream *stream) {
    // Leave valid JSON in the client's first read, but close before the
    // terminal chunk. JSON parsing must not hide the transport failure.
    std::string body = R"({"databases":["db"]})";
    body.resize(8192, ' ');
    return chunk(stream, body) ? 0 : -1;
  };
  std::vector<std::string> databases;
  EXPECT_EQ(client.list_databases(databases), -EIO);
  EXPECT_TRUE(databases.empty());
}

TEST_F(PvfsRestResponseTest, ChunkedJsonIsDecoded) {
  INIT_PHOTON();
  start();
  DEFER(stop());
  PaimonRESTClient client(opts());
  ASSERT_EQ(client.init(), 0);
  reply_ = [](Stream *stream) {
    return chunk(stream, R"({"data)") && chunk(stream, R"(bases":["first",)") &&
                   chunk(stream, R"("second"]})") && write(stream, "0\r\n\r\n")
               ? 0
               : -1;
  };
  std::vector<std::string> databases;
  ASSERT_EQ(client.list_databases(databases), 0);
  EXPECT_EQ(databases, (std::vector<std::string>{"first", "second"}));
}

}  // namespace
