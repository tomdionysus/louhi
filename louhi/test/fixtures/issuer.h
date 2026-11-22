//
// Louhi - The Distributed Compute Layer
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#pragma once

#include <boost/json.hpp>
#include <map>
#include <string>
#include <string_view>

#include "dannan/encoding/percent.h"

// What a test of signing in to a Lugh issuer says and reads back, beside
// lugh_client's testing::FakeHttps, which answers as the issuer.
namespace louhi::test {

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

}  // namespace louhi::test
