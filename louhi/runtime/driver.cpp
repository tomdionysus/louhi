//
// Louhi - The Distributed Compute Layer
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#include "louhi/runtime/driver.h"

namespace louhi::runtime {

const char* kind_name(Error::Kind kind) noexcept {
  switch (kind) {
    case Error::Kind::unavailable:
      return "unavailable";
    case Error::Kind::refused:
      return "refused";
    case Error::Kind::not_found:
      return "not found";
    case Error::Kind::timed_out:
      return "timed out";
    case Error::Kind::malformed:
      return "malformed";
  }
  return "unknown";
}

}  // namespace louhi::runtime
