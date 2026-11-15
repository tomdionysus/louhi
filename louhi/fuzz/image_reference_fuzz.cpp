//
// Louhi - The Distributed Compute Layer
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <stdexcept>
#include <string>
#include <string_view>

#include "louhi/model/image.h"

// Any text a caller gives as an image: refused, or parsed into parts that
// print back as exactly that text and parse again to the same parts.
extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  using namespace louhi::model;
  const std::string_view text(reinterpret_cast<const char*>(data), size);
  try {
    const ImageReference reference = parse_image_reference(text);
    const std::string printed = to_string(reference);
    if (printed != text) std::abort();
    if (parse_image_reference(printed) != reference) std::abort();
  } catch (const std::invalid_argument&) {
  }
  return 0;
}
