//
// Louhi - The Distributed Compute Layer
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

// The Publisher over Lir's client (events/lir.h; design, "Events"): it finds
// Lir in Dannan's roster, asks for the password at every CONNECT, leaves a
// retained empty will on the node's topic, and publishes at QoS 1 with the
// retain asked for. A publish Lir does not take (a refusal, a full queue
// while Lir is away, one still open at stop) is counted, the first of a run
// logged, and never retried. Run against a fake Lir on a real socket that
// speaks MQTT through Lir's own codec, the client's time stepped.

#include <gtest/gtest.h>
#include <poll.h>

#include <array>
#include <atomic>
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/write.hpp>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <future>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <variant>
#include <vector>

#include "dannan/test/fixtures/stepped_time.h"
#include "lir/contract/endpoints.h"
#include "lir/mqtt/packet.h"
#include "louhi/events/lir.h"

namespace {

namespace asio = boost::asio;
namespace mqtt = lir::mqtt;
using tcp = asio::ip::tcp;
using dannan::identity::NodeId;
using dannan::membership::NodeRecord;

constexpr auto wait_limit = std::chrono::seconds(10);

// Every line written, for the test to wait on.
class WatchedSink final : public dannan::status::LogSink {
 public:
  bool enabled(dannan::status::LogLevel) const noexcept override { return true; }
  void write(dannan::status::LogLevel, std::string_view, std::string_view message) override {
    {
      std::lock_guard<std::mutex> lock(_mutex);
      _lines.emplace_back(message);
    }
    _changed.notify_all();
  }
  // Whether `count` lines starting with `text` were written within the bound.
  bool saw(std::string_view text, size_t count = 1) {
    std::unique_lock<std::mutex> lock(_mutex);
    return _changed.wait_for(lock, wait_limit, [&] { return counted(text) >= count; });
  }
  size_t count(std::string_view text) {
    std::lock_guard<std::mutex> lock(_mutex);
    return counted(text);
  }

 private:
  size_t counted(std::string_view text) const {
    size_t n = 0;
    for (const auto& line : _lines)
      if (line.rfind(text, 0) == 0) ++n;
    return n;
  }
  std::mutex _mutex;
  std::condition_variable _changed;
  std::vector<std::string> _lines;
};

// Dannan's roster as the client reads it: the nodes given, all active.
class FakeNodes final : public dannan::Nodes {
 public:
  std::vector<NodeRecord> nodes;

  void refresh_public_connectivity(bool, bool, asio::any_io_executor, std::function<void(std::optional<dannan::reach::PublicConnectivityStatus>)>) override {}
  uint64_t watch(asio::any_io_executor, std::function<void(dannan::RosterChange)>) override { return 0; }
  void unwatch(uint64_t) override {}
  uint64_t watch_advert(const std::string&, std::chrono::milliseconds, asio::any_io_executor, std::function<void(dannan::AdvertChange)>) override { return 0; }
  void unwatch_advert(uint64_t) override {}
  NodeRecord self() const override { return {}; }
  std::vector<NodeRecord> roster() const override { return nodes; }
  std::vector<NodeRecord> active() const override { return nodes; }
  dannan::RosterSnapshot snapshot() const override { return {nodes, nodes}; }
  std::optional<dannan::contract::TimePoint> absent_since(const NodeId&) const override { return std::nullopt; }
  std::optional<dannan::identity::Certificate> certificate(const NodeId&) const override { return std::nullopt; }
  bool directly_reachable(const NodeId&) const override { return true; }
  bool all_known_reachable() const override { return true; }
  bool inbound_capable(const NodeId&) const override { return true; }
  std::optional<dannan::net::Purpose> path(const NodeId&, dannan::net::Lane) const override { return std::nullopt; }
  std::optional<std::chrono::milliseconds> latency(const NodeId&) const override { return std::nullopt; }
  std::map<NodeId, std::chrono::milliseconds> latencies() const override { return {}; }
  std::vector<dannan::membership::IdentityReset> identity_resets() const override { return {}; }
  bool apply_identity_reset(const dannan::membership::IdentityReset&) override { return false; }
  void propagate_identity_reset(const dannan::membership::IdentityReset&, std::chrono::milliseconds) override {}
  std::vector<NodeRecord> placement(std::span<const uint8_t>, size_t, const dannan::placement::Scheme&, const std::string&) const override { return {}; }
  void advertise(const std::string&, dannan::net::Bytes) override {}
  void withdraw(const std::string&) override {}
  void advertise(const std::string&, std::optional<dannan::net::Bytes>, std::chrono::milliseconds, asio::any_io_executor,
                 std::function<void(dannan::membership::Pushed)>) override {}
  dannan::membership::Partition partition() const override { return {}; }
  uint64_t watch_partition(asio::any_io_executor, std::function<void(dannan::membership::Partition)>) override { return 0; }
  void unwatch_partition(uint64_t) override {}
  std::set<NodeId> members() const override { return {}; }
  bool retire(const NodeId&) override { return false; }
  std::optional<std::chrono::milliseconds> clock_offset(const NodeId&) const override { return std::nullopt; }
  bool clock_in_bounds() const override { return true; }
};

// A node of Lir as the roster shows one: the "lir" advert, and where it
// serves plain TCP.
NodeRecord lir_node(uint16_t port) {
  NodeRecord node;
  node.host = "127.0.0.1";
  node.adverts[std::string(lir::contract::lir_advert)] = {};
  const auto endpoints = lir::contract::endpoints_advert(lir::contract::Endpoints{{"tcp", "127.0.0.1:" + std::to_string(port)}});
  node.adverts[std::string(lir::contract::endpoints_advert_key)] = dannan::net::Bytes(endpoints.begin(), endpoints.end());
  return node;
}

// One Lir node, one client at a time, driven by the test: every wait is
// bounded, so a test that waits wrongly fails rather than hangs.
class FakeLir {
 public:
  FakeLir() : _acceptor(_io, tcp::endpoint(asio::ip::make_address("127.0.0.1"), 0)) {}

  uint16_t port() const { return _acceptor.local_endpoint().port(); }

  bool accept() {
    if (!ready(_acceptor.native_handle())) return false;
    _socket = std::make_unique<tcp::socket>(_io);
    _acceptor.accept(*_socket);
    _buffer.clear();
    return true;
  }
  void send(const mqtt::Packet& packet) { asio::write(*_socket, asio::buffer(mqtt::encode(packet, mqtt::Version::v5))); }
  void close() {
    if (!_socket) return;
    boost::system::error_code ignored;
    _socket->close(ignored);
  }

  // The next packet, or nothing on close or past the bound.
  std::optional<mqtt::Packet> receive() {
    for (;;) {
      if (const auto frame = mqtt::read_frame(_buffer, UINT32_MAX); frame && _buffer.size() >= frame->size()) {
        auto packet = mqtt::decode(std::span<const uint8_t>(_buffer).first(frame->size()), mqtt::Version::v5);
        _buffer.erase(_buffer.begin(), _buffer.begin() + static_cast<std::ptrdiff_t>(frame->size()));
        return packet;
      }
      if (!ready(_socket->native_handle())) return std::nullopt;
      std::array<uint8_t, 65536> chunk{};
      boost::system::error_code error;
      const size_t got = _socket->read_some(asio::buffer(chunk), error);
      if (error) return std::nullopt;
      _buffer.insert(_buffer.end(), chunk.begin(), chunk.begin() + static_cast<std::ptrdiff_t>(got));
    }
  }
  template <typename T>
  std::optional<T> receive_a() {
    auto packet = receive();
    if (!packet || !std::holds_alternative<T>(*packet)) return std::nullopt;
    return std::get<T>(std::move(*packet));
  }

 private:
  static bool ready(int fd) {
    pollfd want{fd, POLLIN, 0};
    return ::poll(&want, 1, static_cast<int>(std::chrono::milliseconds(wait_limit).count())) > 0;
  }

  asio::io_context _io;
  tcp::acceptor _acceptor;
  std::unique_ptr<tcp::socket> _socket;
  mqtt::Bytes _buffer;
};

// The publisher's own io, run on a thread for the test's life.
class Io {
 public:
  Io() : _work(asio::make_work_guard(_io)), _thread([this] { _io.run(); }) {}
  ~Io() {
    _work.reset();
    _io.stop();
    _thread.join();
  }
  asio::any_io_executor executor() { return _io.get_executor(); }

 private:
  asio::io_context _io;
  asio::executor_work_guard<asio::io_context::executor_type> _work;
  std::thread _thread;
};

std::string text(const mqtt::Bytes& bytes) { return std::string(bytes.begin(), bytes.end()); }

const std::string will_topic = "louhi/nodes/n1";

struct LirPublisherTest : ::testing::Test {
  Io io;
  std::shared_ptr<WatchedSink> sink = std::make_shared<WatchedSink>();
  dannan::test::SteppedTime stepped;
  FakeNodes nodes;
  FakeLir lir;
  std::atomic<int> passwords{0};
  std::unique_ptr<louhi::events::LirPublisher> publisher;

  void make() {
    louhi::events::LirConfig config;
    config.nodes = &nodes;
    config.client_id = "louhi-n1";
    config.password = [this]() -> std::optional<std::string> { return "token-" + std::to_string(++passwords); };
    publisher =
        std::make_unique<louhi::events::LirPublisher>(io.executor(), std::move(config), will_topic, stepped, dannan::status::ScopedLogger(sink, "events"));
  }

  // Stopped, the client touches neither the roster nor the clock again; Lir
  // closes first, so a stop left to here never waits on its side.
  ~LirPublisherTest() override {
    lir.close();
    if (publisher) stop();
  }

  // The promise is the callback's too: set_value readies the future before it
  // is done with the promise, so one on the waiter's stack could die under it.
  void stop() {
    auto stopped = std::make_shared<std::promise<void>>();
    auto done = stopped->get_future();
    publisher->stop([stopped] { stopped->set_value(); });
    EXPECT_EQ(done.wait_for(wait_limit), std::future_status::ready);
  }
};

}  // namespace

TEST_F(LirPublisherTest, ConnectsToLirInTheRosterWithAFreshPasswordAndAWillThatClearsTheNode) {
  NodeRecord plain;
  plain.host = "127.0.0.1";
  plain.port = 1;
  nodes.nodes = {plain, lir_node(lir.port())};
  make();
  publisher->start();

  ASSERT_TRUE(lir.accept());
  const auto connect = lir.receive_a<mqtt::Connect>();
  ASSERT_TRUE(connect);
  EXPECT_EQ(connect->version, mqtt::Version::v5);
  EXPECT_EQ(connect->client_id, "louhi-n1");
  ASSERT_TRUE(connect->password);
  EXPECT_EQ(text(*connect->password), "token-1");
  ASSERT_TRUE(connect->will);
  EXPECT_EQ(connect->will->topic, will_topic);
  EXPECT_TRUE(connect->will->payload.empty()) << "an empty retained message clears the node's status";
  EXPECT_TRUE(connect->will->retain);
  EXPECT_EQ(connect->will->qos, mqtt::QoS::at_least_once);
  EXPECT_FALSE(publisher->connected()) << "not before Lir accepts";

  lir.send(mqtt::Connack{});
  ASSERT_TRUE(sink->saw("connected to Lir"));
  EXPECT_TRUE(publisher->connected());

  // Lir goes away and comes back: the password is asked for again.
  lir.send(mqtt::Disconnect{mqtt::reason::server_shutting_down, {}});
  lir.close();
  ASSERT_TRUE(sink->saw("not connected to Lir"));
  EXPECT_FALSE(publisher->connected());
  stepped.advance(std::chrono::minutes(1));
  ASSERT_TRUE(lir.accept());
  const auto again = lir.receive_a<mqtt::Connect>();
  ASSERT_TRUE(again && again->password);
  EXPECT_EQ(text(*again->password), "token-2");
  lir.send(mqtt::Connack{});
  ASSERT_TRUE(sink->saw("connected to Lir", 2));
  EXPECT_TRUE(publisher->connected());
  EXPECT_EQ(sink->count("not connected to Lir"), 1u);

  // A clean stop says DISCONNECT, normal, so Lir does not publish the will.
  auto stopped = std::make_shared<std::promise<void>>();
  auto stopped_future = stopped->get_future();
  publisher->stop([stopped] { stopped->set_value(); });
  const auto bye = lir.receive_a<mqtt::Disconnect>();
  ASSERT_TRUE(bye);
  EXPECT_EQ(bye->reason, mqtt::reason::normal_disconnection);
  lir.close();
  EXPECT_EQ(stopped_future.wait_for(wait_limit), std::future_status::ready);
}

TEST_F(LirPublisherTest, PublishesAtLeastOnceAndCountsWhatLirRefusesWithoutRetrying) {
  nodes.nodes = {lir_node(lir.port())};
  make();
  publisher->start();
  ASSERT_TRUE(lir.accept());
  ASSERT_TRUE(lir.receive_a<mqtt::Connect>());
  lir.send(mqtt::Connack{});

  publisher->publish("louhi/nodes/n1", R"({"ready":true})", true);
  const auto status = lir.receive_a<mqtt::Publish>();
  ASSERT_TRUE(status);
  EXPECT_EQ(status->topic, "louhi/nodes/n1");
  EXPECT_EQ(text(status->payload), R"({"ready":true})");
  EXPECT_EQ(status->qos, mqtt::QoS::at_least_once);
  EXPECT_TRUE(status->retain);
  lir.send(mqtt::Ack{mqtt::PacketType::puback, status->packet_id, mqtt::reason::success, {}});

  publisher->publish("louhi/projects/web/events", R"({"event":"task.run"})", false);
  const auto refused = lir.receive_a<mqtt::Publish>();
  ASSERT_TRUE(refused);
  EXPECT_EQ(refused->qos, mqtt::QoS::at_least_once);
  EXPECT_FALSE(refused->retain);
  mqtt::Properties why;
  why.reason_string = "not this topic";
  lir.send(mqtt::Ack{mqtt::PacketType::puback, refused->packet_id, mqtt::reason::not_authorized, why});

  publisher->publish("louhi/projects/web/events", R"({"event":"task.stop_requested"})", false);
  const auto over = lir.receive_a<mqtt::Publish>();
  ASSERT_TRUE(over);
  lir.send(mqtt::Ack{mqtt::PacketType::puback, over->packet_id, mqtt::reason::quota_exceeded, {}});

  // Lir's packets are taken in order, so both refusals are counted by the
  // time the disconnect after them is told.
  lir.send(mqtt::Disconnect{mqtt::reason::server_shutting_down, {}});
  lir.close();
  ASSERT_TRUE(sink->saw("not connected to Lir"));
  EXPECT_EQ(publisher->dropped(), 2u);
  EXPECT_EQ(sink->count("Lir did not take an event"), 1u) << "the first of a run is logged; the count says the rest";

  // Back again: nothing refused is sent a second time.
  stepped.advance(std::chrono::minutes(1));
  ASSERT_TRUE(lir.accept());
  ASSERT_TRUE(lir.receive_a<mqtt::Connect>());
  lir.send(mqtt::Connack{});
  // Connected again before it is stopped: a stop before the CONNACK is read
  // closes with no DISCONNECT, which is right, but not what this asks.
  ASSERT_TRUE(sink->saw("connected to Lir", 2));
  // Shared with the callback: set_value readies the future before it is done
  // with the promise, so a promise on this stack could die under it.
  auto stopped = std::make_shared<std::promise<void>>();
  auto stopped_future = stopped->get_future();
  publisher->stop([stopped] { stopped->set_value(); });
  const auto next = lir.receive();
  ASSERT_TRUE(next);
  EXPECT_TRUE(std::holds_alternative<mqtt::Disconnect>(*next)) << "a refused publish was sent again";
  lir.close();
  EXPECT_EQ(stopped_future.wait_for(wait_limit), std::future_status::ready);
  EXPECT_EQ(publisher->dropped(), 2u);
}

TEST_F(LirPublisherTest, WhileLirIsAwayAFullQueueAndAnUnsentPublishAreCounted) {
  // No node in the roster serves Lir: the publisher never connects.
  make();
  publisher->start();
  publisher->publish("louhi/nodes/n1", "{}", true);
  publisher->publish("louhi/projects/web/events", std::string(5u << 20, 'x'), false);
  stop();
  EXPECT_FALSE(publisher->connected());
  EXPECT_EQ(publisher->dropped(), 2u) << "the one past the queue at once, the one held at stop";
  EXPECT_EQ(sink->count("Lir did not take an event"), 1u);
}
