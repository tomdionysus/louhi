//
// Louhi - The Distributed Compute Layer
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#pragma once

#include <boost/asio/post.hpp>
#include <boost/json.hpp>
#include <deque>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "dannan/encoding/percent.h"
#include "lugh/client/https.h"

// A Lugh issuer as Lugh's HTTPS client sees one, for the tests of what signs
// in through it. Every request is held until the test answers it, in the
// order sent; each answer is posted to the caller's executor, never given
// inline. Thread-safe.
namespace louhi::test {

class FakeHttps final : public lugh::client::Https {
 public:
  struct Sent {
    lugh::client::HttpsRequest request;
    lugh::Deadline deadline{0};
  };

  // Answers every request at once, as `script` says, instead of holding it:
  // for a caller that blocks the test's thread while it waits.
  using Script = std::function<lugh::Outcome<lugh::client::HttpsResponse>(const lugh::client::HttpsRequest&)>;
  void answer_by(Script script) {
    std::lock_guard<std::mutex> lock(_mutex);
    _script = std::move(script);
  }

  void send(lugh::client::HttpsRequest request, lugh::Deadline deadline, lugh::Executor executor,
            lugh::Done<lugh::Outcome<lugh::client::HttpsResponse>> done) override {
    Script script;
    {
      std::lock_guard<std::mutex> lock(_mutex);
      _sent.push_back({request, deadline});
      if (!_script) {
        _pending.push_back({std::move(request), std::move(executor), std::move(done)});
        return;
      }
      script = _script;
    }
    boost::asio::post(executor, [done = std::move(done), outcome = script(request)]() mutable { done(std::move(outcome)); });
  }

  // Every request sent so far, answered or not.
  std::vector<Sent> sent() const {
    std::lock_guard<std::mutex> lock(_mutex);
    return _sent;
  }
  size_t pending() const {
    std::lock_guard<std::mutex> lock(_mutex);
    return _pending.size();
  }
  // The oldest request not yet answered; nothing when none waits.
  std::optional<lugh::client::HttpsRequest> next() const {
    std::lock_guard<std::mutex> lock(_mutex);
    if (_pending.empty()) return std::nullopt;
    return _pending.front().request;
  }

  // Answers the oldest request waiting. False when none waits.
  bool answer(unsigned status, std::string body, std::string content_type = "application/json") {
    lugh::client::HttpsResponse response;
    response.status = status;
    response.body = std::move(body);
    response.content_type = std::move(content_type);
    return reply(lugh::Outcome<lugh::client::HttpsResponse>::success(std::move(response)));
  }
  bool answer_json(unsigned status, const boost::json::object& body) { return answer(status, boost::json::serialize(body)); }
  // The oldest request waiting fails: no answer came.
  bool fail(std::string why) { return reply(lugh::Outcome<lugh::client::HttpsResponse>::failure(lugh::Fault::failed, std::move(why))); }

 private:
  struct Pending {
    lugh::client::HttpsRequest request;
    lugh::Executor executor;
    lugh::Done<lugh::Outcome<lugh::client::HttpsResponse>> done;
  };

  bool reply(lugh::Outcome<lugh::client::HttpsResponse> outcome) {
    Pending first;
    {
      std::lock_guard<std::mutex> lock(_mutex);
      if (_pending.empty()) return false;
      first = std::move(_pending.front());
      _pending.pop_front();
    }
    boost::asio::post(first.executor, [done = std::move(first.done), outcome = std::move(outcome)]() mutable { done(std::move(outcome)); });
    return true;
  }

  mutable std::mutex _mutex;
  Script _script;
  std::vector<Sent> _sent;
  std::deque<Pending> _pending;
};

// An answer for a script.
inline lugh::Outcome<lugh::client::HttpsResponse> json_answer(unsigned status, const boost::json::object& body) {
  lugh::client::HttpsResponse response;
  response.status = status;
  response.content_type = "application/json";
  response.body = boost::json::serialize(body);
  return lugh::Outcome<lugh::client::HttpsResponse>::success(std::move(response));
}

// A discovery document for `issuer`, with its endpoints under it.
inline boost::json::object discovery(const std::string& issuer) {
  return {{"issuer", issuer},
          {"authorization_endpoint", issuer + "/authorize"},
          {"token_endpoint", issuer + "/token"},
          {"device_authorization_endpoint", issuer + "/device"}};
}

// The fields of an application/x-www-form-urlencoded body, one value each.
inline std::map<std::string, std::string> form_fields(std::string_view body) {
  std::map<std::string, std::string> out;
  while (!body.empty()) {
    const auto amp = body.find('&');
    const auto pair = body.substr(0, amp);
    const auto equals = pair.find('=');
    const auto value = equals == std::string_view::npos ? std::string_view() : pair.substr(equals + 1);
    out[dannan::encoding::percent_decode(pair.substr(0, equals), dannan::encoding::Decoding::form)] =
        dannan::encoding::percent_decode(value, dannan::encoding::Decoding::form);
    if (amp == std::string_view::npos) break;
    body.remove_prefix(amp + 1);
  }
  return out;
}

// The value of `name` among a request's headers; empty when absent.
inline std::string header(const lugh::client::HttpsRequest& request, const std::string& name) {
  for (const auto& [key, value] : request.headers)
    if (key == name) return value;
  return {};
}

}  // namespace louhi::test
