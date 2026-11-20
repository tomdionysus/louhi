//
// Louhi - The Distributed Compute Layer
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#pragma once

#include <boost/asio/any_io_executor.hpp>
#include <boost/asio/strand.hpp>
#include <chrono>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "dannan/contract/thread_safety.h"
#include "dannan/contract/time_source.h"
#include "dannan/sched/retry.h"
#include "dannan/status/log.h"
#include "lugh/client/https.h"
#include "lugh/client/oauth_client.h"
#include "lugh/client/verifier.h"

// This node's own access token from Lugh (design, "Events": louhi-node signs
// in to Lir as a Lugh service principal), by the client-credentials grant,
// proving itself by private_key_jwt or client_secret_basic (the maintainer, 2026-11-15:
// both). The issuer's discovery is read once; the token is fetched at start
// and again when two thirds of its life has passed (never sooner than ten
// seconds), so current() always answers at once with the newest one; a
// failure keeps the one held and tries again after a backoff of 5 seconds
// doubling to 5 minutes. Built only with LOUHI_BUILD_LUGH.
namespace louhi::auth {

struct ServiceTokenConfig {
  // The Lugh that issues it, as its tokens name it.
  std::string issuer;
  lugh::client::ClientIdentity identity;
  // Empty: every scope the client is granted.
  std::vector<std::string> scopes;
};

// Calls `fn` after `delay`, on any thread. Waits on nothing.
using After = std::function<void(std::chrono::milliseconds, std::function<void()>)>;

class ServiceToken : public std::enable_shared_from_this<ServiceToken> {
 public:
  // `https` reaches the issuer; it and the wall clock outlive this. Throws
  // std::invalid_argument for an identity that cannot sign in by
  // private_key_jwt or client_secret_basic.
  static std::shared_ptr<ServiceToken> make(lugh::client::Https& https, boost::asio::any_io_executor executor, After after, ServiceTokenConfig config,
                                            const dannan::contract::WallClock& wall, dannan::status::ScopedLogger logger);

  // Each waits on nothing; thread-safe. Nothing is asked of the issuer after
  // stop(), and an answer that comes after it is dropped.
  void start();
  void stop();
  // The newest token, or nothing before the first came. Never waits;
  // thread-safe.
  std::optional<std::string> current() const;

 private:
  ServiceToken(lugh::client::Https& https, boost::asio::any_io_executor executor, After after, ServiceTokenConfig config,
               const dannan::contract::WallClock& wall, dannan::status::ScopedLogger logger);
  void fetch();
  void retry();
  void later(std::chrono::milliseconds delay);

  ServiceTokenConfig _config;
  const dannan::contract::WallClock& _wall;
  dannan::status::ScopedLogger _logger;
  lugh::client::Https& _https;
  lugh::client::Verifier _verifier;
  // Every step and stop() run on this strand, one at a time.
  boost::asio::strand<boost::asio::any_io_executor> _strand;
  After _after;
  std::unique_ptr<lugh::client::OAuthClient> _oauth;
  // Dannan's retry discipline, with no budget: a node never gives up on its
  // token, as nothing but a token signs it in.
  dannan::sched::RetryState _retries;
  bool _stopped = false;

  mutable dannan::contract::Mutex _mutex;
  std::optional<std::string> _token DANNAN_GUARDED_BY(_mutex);
};

}  // namespace louhi::auth
