//
// Louhi - The Distributed Compute Layer
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#pragma once

#include <atomic>
#include <boost/asio/any_io_executor.hpp>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>

#include "dannan/context.h"
#include "dannan/contract/time_source.h"
#include "dannan/status/log.h"
#include "lir/client/client.h"
#include "louhi/events/events.h"

// The Publisher over Lir's own client (design, "Events"; the maintainer: Lir's exported
// client, Louhi writes none). Lir's nodes are found in Dannan's roster; the
// password is asked at every CONNECT, so a token can be fresh each time. A
// publish Lir does not take (a full queue while Lir is away, a refusal) is
// counted and logged, never retried here: the store holds the truth. Built
// only with LOUHI_BUILD_LIR.
namespace louhi::events {

struct LirConfig {
  // Each attempt goes to a node advertising Lir; outlives the publisher.
  const dannan::Nodes* nodes = nullptr;
  std::optional<lir::client::Tls> tls;
  std::string client_id;
  // Asked at each CONNECT; nothing: no password. Must not block.
  std::function<std::optional<std::string>()> password;
};

class LirPublisher final : public Publisher {
 public:
  // The executor's context and the clock outlive this. `will` is published
  // retained by Lir when this node goes without stopping: `topic` emptied.
  LirPublisher(boost::asio::any_io_executor executor, LirConfig config, std::string will_topic, const dannan::contract::TimeSource& time,
               dannan::status::ScopedLogger logger);
  ~LirPublisher() override;

  void start();
  // Stops; `done` once every publish still open has failed or finished.
  void stop(std::function<void()> done);
  void publish(std::string topic, std::string payload, bool retain) override;

  // Publishes Lir did not take since start.
  uint64_t dropped() const noexcept { return _dropped->load(); }
  bool connected() const noexcept { return _connected->load(); }

 private:
  dannan::status::ScopedLogger _logger;
  std::shared_ptr<std::atomic<uint64_t>> _dropped = std::make_shared<std::atomic<uint64_t>>(0);
  std::shared_ptr<std::atomic<bool>> _connected = std::make_shared<std::atomic<bool>>(false);
  lir::client::Client _client;
};

}  // namespace louhi::events
