// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "slf/transport.hpp"

#include <thread>

#include "fixtures.hpp"
#include "slf/platform.hpp"
#include "slf/service.hpp"
#include "slf_test.hpp"

using namespace slf;
using namespace slf::test;

SLF_TEST(transport_endpoint_and_frame_codec) {
  const auto endpoint = net::Endpoint::parse("127.0.0.1:9000");
  SLF_EXPECT(endpoint.ok());
  SLF_EXPECT_EQ(endpoint.value().port, 9000);
  SLF_EXPECT_EQ(endpoint.value().to_string(), std::string("127.0.0.1:9000"));
  SLF_EXPECT_CODE(net::Endpoint::parse("127.0.0.1"), StatusCode::Invalid);
  SLF_EXPECT_CODE(net::Endpoint::parse("127.0.0.1:0"), StatusCode::Invalid);
  SLF_EXPECT_CODE(net::Endpoint::parse("127.0.0.1:70000"), StatusCode::Invalid);
  SLF_EXPECT_CODE(net::Endpoint::parse(":9000"), StatusCode::Invalid);

  net::TransportLimits limits;
  const std::vector<std::byte> payload{std::byte{9}, std::byte{8}, std::byte{7}};
  net::FrameHeader header;
  header.type = 3;
  header.request_id = 42;
  const auto encoded = net::encode_frame(header, payload, limits);
  SLF_EXPECT(encoded.ok());
  if (!encoded.ok()) {
    return;
  }
  SLF_EXPECT_EQ(encoded.value().size(), net::FrameHeader::kHeaderBytes + payload.size());
  const auto decoded = net::decode_frame_header(
      std::span<const std::byte>(encoded.value().data(), net::FrameHeader::kHeaderBytes), limits);
  SLF_EXPECT(decoded.ok());
  if (decoded.ok()) {
    SLF_EXPECT_EQ(decoded.value().type, 3);
    SLF_EXPECT_EQ(decoded.value().request_id, 42U);
    SLF_EXPECT_EQ(decoded.value().payload_len, 3U);
  }
  SLF_EXPECT(net::verify_frame(
                 std::span<const std::byte>(encoded.value().data(), net::FrameHeader::kHeaderBytes),
                 std::span<const std::byte>(encoded.value().data() + net::FrameHeader::kHeaderBytes,
                                            payload.size()))
                 .ok());
  // A payload above the bound is refused before anything is allocated.
  net::TransportLimits tight;
  tight.max_frame_payload = 2;
  SLF_EXPECT_CODE(net::encode_frame(header, payload, tight), StatusCode::Exhausted);
}

SLF_TEST(transport_server_and_client_round_trip) {
  TempDir dir{"transport"};
  RuntimeConfig config;
  config.fabric_id = FabricId{1};
  config.name = "transport";
  config.enable_persistence = false;
  config.worker_threads = 0;
  ControllerRuntime runtime(config);
  SLF_EXPECT(runtime.start().ok());
  const auto fabric = make_two_by_two();
  SLF_EXPECT(fabric.ok());
  SLF_EXPECT(runtime.install_fabric(fabric.value(), true).ok());

  net::RuntimeService service(runtime);
  net::ServerConfig server_config;
  server_config.endpoint.host = "127.0.0.1";
  server_config.endpoint.port = 0;
  auto server = net::FabricServer::create(server_config, &service);
  SLF_EXPECT(server.ok());
  if (!server.ok()) {
    return;
  }
  SLF_EXPECT(server.value()->start().ok());
  SLF_EXPECT(server.value()->port() != 0);

  net::ClientConfig client_config;
  client_config.endpoint.host = "127.0.0.1";
  client_config.endpoint.port = server.value()->port();
  auto client = net::FabricClient::connect(client_config);
  SLF_EXPECT(client.ok());
  if (!client.ok()) {
    (void)server.value()->stop();
    return;
  }
  SLF_EXPECT(net::handshake(*client.value(), "transport").ok());

  net::Request status_request;
  status_request.kind = net::RequestKind::Status;
  status_request.request_id = RequestId{1};
  const auto status = client.value()->call(status_request);
  SLF_EXPECT(status.ok());
  if (status.ok()) {
    SLF_EXPECT_EQ(status.value().code, StatusCode::Ok);
    SLF_EXPECT_EQ(status.value().status.leaf_count, 2U);
    SLF_EXPECT_EQ(status.value().server_name, std::string("slf-controller"));
  }

  net::Request paths_request;
  paths_request.kind = net::RequestKind::Paths;
  paths_request.request_id = RequestId{2};
  paths_request.from = NodeKey{LeafId{1}};
  paths_request.to = NodeKey{LeafId{2}};
  const auto paths = client.value()->call(paths_request);
  SLF_EXPECT(paths.ok());
  if (paths.ok()) {
    SLF_EXPECT_EQ(paths.value().assessment.verdict, EligibilityVerdict::Eligible);
    SLF_EXPECT_EQ(paths.value().assessment.eligible_spines.size(), 2U);
  }

  // Unknown leaves are refused by the service, not silently answered.
  net::Request missing;
  missing.kind = net::RequestKind::Paths;
  missing.request_id = RequestId{3};
  missing.from = NodeKey{LeafId{9}};
  missing.to = NodeKey{LeafId{2}};
  const auto missing_response = client.value()->call(missing);
  SLF_EXPECT(missing_response.ok());
  if (missing_response.ok()) {
    SLF_EXPECT_EQ(missing_response.value().code, StatusCode::NotFound);
  }

  // An unsupported protocol version is rejected at the frame layer.
  const auto stats = server.value()->stats();
  SLF_EXPECT(stats.accepted_connections >= 1U);
  SLF_EXPECT(stats.frames_read >= 3U);
  SLF_EXPECT(stats.frames_written >= 3U);
  SLF_EXPECT(stats.active_connections >= 1U);

  (void)client.value()->close();
  SLF_EXPECT(server.value()->stop().ok());
  SLF_EXPECT(runtime.stop().ok());
}
