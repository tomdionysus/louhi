//
// Louhi - The Distributed Compute Layer
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

// A fuzz binary's entry, as Dannan's harness has it (dannan/fuzz/fuzz.h): the
// targets of the one target file it is built from, each run on every input.
// With libFuzzer (LOUHI_LIBFUZZER), the fuzzer's own entry point; without it,
// a replay driver over files, or the seeds when given none.

#include <cstdio>
#include <span>

#include "dannan/fuzz/fuzz.h"

#if defined(LOUHI_LIBFUZZER)
extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  dannan::fuzz::run_all(dannan::fuzz::targets(), std::span<const uint8_t>(data, size));
  return 0;
}
#else
int main(int argc, char** argv) {
  const auto runs = dannan::fuzz::replay(dannan::fuzz::targets(), argc, argv);
  std::printf("%zu inputs through %zu targets, no finding\n", runs, dannan::fuzz::targets().size());
  return 0;
}
#endif
