//
// Louhi - The Distributed Compute Layer
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#include "louhi/events/lir.h"

namespace louhi::events {

namespace {

lir::client::ClientConfig client_config(LirConfig config, std::string will_topic) {
  lir::client::ClientConfig out;
  out.nodes = config.nodes;
  out.tls = std::move(config.tls);
  out.client_id = std::move(config.client_id);
  out.password = std::move(config.password);
  out.version = lir::mqtt::Version::v5;
  // An empty retained message clears the node's status when it goes unseen.
  lir::client::Message will;
  will.topic = std::move(will_topic);
  will.qos = lir::mqtt::QoS::at_least_once;
  will.retain = true;
  out.will = std::move(will);
  return out;
}

}  // namespace

LirPublisher::LirPublisher(boost::asio::any_io_executor executor, LirConfig config, std::string will_topic, const dannan::contract::TimeSource& time,
                           dannan::status::ScopedLogger logger)
    : _logger(logger), _client(std::move(executor), client_config(std::move(config), std::move(will_topic)), time, std::move(logger)) {
  _client.on_state([connected = _connected, logger = _logger](const lir::client::StateChange& change) {
    const bool now = change.state == lir::client::ConnectionState::connected;
    if (connected->exchange(now) != now) {
      if (now)
        logger.info("connected to Lir");
      else
        logger.warn("not connected to Lir: " + change.why);
    }
  });
}

LirPublisher::~LirPublisher() = default;

void LirPublisher::start() { _client.start(); }

void LirPublisher::stop(std::function<void()> done) { _client.stop(std::move(done)); }

void LirPublisher::publish(std::string topic, std::string payload, bool retain) {
  lir::client::Message message;
  message.topic = std::move(topic);
  message.payload = lir::mqtt::Bytes(payload.begin(), payload.end());
  message.qos = lir::mqtt::QoS::at_least_once;
  message.retain = retain;
  _client.publish(std::move(message), [dropped = _dropped, logger = _logger](const lir::client::Outcome& outcome) {
    if (outcome.ok) return;
    // The first of a run is logged; the count says the rest.
    if (dropped->fetch_add(1) == 0) logger.warn("Lir did not take an event: " + outcome.why);
  });
}

}  // namespace louhi::events
