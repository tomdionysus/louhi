#!/bin/sh
#
# Louhi - The Distributed Compute Layer
#
# Copyright (C) 2026 Tom Cully <mail@tomcully.com>
# Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
#
# One CI step on Debian 13, as the nodes have it, in GitHub's runner, a local
# container or a Pi; .github/workflows/ci.yml runs each as its own job.
#
#   tools/ci.sh check      the header block on every source, and clang-format
#   tools/ci.sh release    g++ release build, warnings as errors, and the tests
#   tools/ci.sh clang      the same with clang++ (and -Wthread-safety)
#   tools/ci.sh asan       AddressSanitizer and UndefinedBehaviorSanitizer
#   tools/ci.sh tsan       ThreadSanitizer, with Dannan's suppressions
#   tools/ci.sh coverage   a coverage build's tests; lcov report build-ci/coverage.info
#   tools/ci.sh tidy       clang-tidy's use-after-move over Louhi's sources
#   tools/ci.sh fuzz       the four fuzz targets, LOUHI_FUZZ_SECONDS (60) each
#   tools/ci.sh events     the release step with the events publisher on Lir's
#                          client (LOUHI_BUILD_LIR, off in every other step:
#                          no Lir release ships a client yet)
#
# Builds go under build-ci/ (LOUHI_CI_OUT to move it), one job per CPU
# (LOUHI_CI_JOBS to set it: 2 on the shared Pis). Dannan is fetched at the
# pinned tag from the sibling checkout when there is one, else from its public
# repository (LOUHI_DANNAN_REPOSITORY to name another), and Lir likewise
# (LOUHI_LIR_REPOSITORY), Dagda (LOUHI_DAGDA_REPOSITORY) and Lugh
# (LOUHI_LUGH_REPOSITORY). Needs g++ cmake ninja-build libboost-dev libssl-dev libyaml-cpp-dev libgtest-dev, and for
# its step clang, clang-format or lcov.

set -eu

top=$(cd "$(dirname "$0")/.." && pwd)
out=${LOUHI_CI_OUT:-$top/build-ci}
jobs=${LOUHI_CI_JOBS:-$(nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 4)}
step=${1:-}
if [ -z "${LOUHI_DANNAN_REPOSITORY:-}" ]; then
  if [ -d "$top/../dannan/.git" ]; then
    LOUHI_DANNAN_REPOSITORY=$top/../dannan
  else
    LOUHI_DANNAN_REPOSITORY=https://github.com/tomdionysus/dannan.git
  fi
fi
if [ -z "${LOUHI_LIR_REPOSITORY:-}" ]; then
  if [ -d "$top/../lir/.git" ]; then
    LOUHI_LIR_REPOSITORY=$top/../lir
  else
    LOUHI_LIR_REPOSITORY=https://github.com/tomdionysus/lir.git
  fi
fi
# Lugh, public since 2026-11-20, likewise, so every step builds the Lugh
# plugin and the API.
if [ -z "${LOUHI_LUGH_REPOSITORY:-}" ]; then
  if [ -d "$top/../lugh/.git" ]; then
    LOUHI_LUGH_REPOSITORY=$top/../lugh
  else
    LOUHI_LUGH_REPOSITORY=https://github.com/tomdionysus/lugh.git
  fi
fi
siblings="-DLOUHI_BUILD_LUGH=ON -DLOUHI_LUGH_REPOSITORY=$LOUHI_LUGH_REPOSITORY"
# Dagda, public since 2026-11-20: from LOUHI_DAGDA_REPOSITORY, else the
# sibling checkout, else its public repository, as Dannan.
if [ -z "${LOUHI_DAGDA_REPOSITORY:-}" ]; then
  if [ -d "$top/../dagda/.git" ]; then
    LOUHI_DAGDA_REPOSITORY=$top/../dagda
  else
    LOUHI_DAGDA_REPOSITORY=https://github.com/tomdionysus/dagda.git
  fi
fi
siblings="$siblings -DLOUHI_BUILD_DAGDA=ON -DLOUHI_DAGDA_REPOSITORY=$LOUHI_DAGDA_REPOSITORY"

# Warnings are errors in every build but the sanitizers': instrumented, g++
# warns inside libstdc++ and Asio, not in Louhi. The release and clang steps
# hold the warning gate.
configure() {
  dir="$1"
  shift
  cmake -S "$top" -B "$out/$dir" -G Ninja -DLOUHI_BUILD_TESTING=ON -DLOUHI_WARNINGS_AS_ERRORS=ON -DLOUHI_DANNAN_REPOSITORY="$LOUHI_DANNAN_REPOSITORY" -DLOUHI_LIR_REPOSITORY="$LOUHI_LIR_REPOSITORY" $siblings "$@"
  cmake --build "$out/$dir" -j "$jobs"
}

case "$step" in
  check)
    cd "$top"
    missing=0
    for f in $(git ls-files '*.h' '*.cpp' '*.h.in'); do
      if ! head -3 "$f" | grep -qx '// Louhi - The Distributed Compute Layer'; then
        echo "no header block: $f"
        missing=1
      fi
    done
    [ "$missing" = 0 ]
    git ls-files '*.h' '*.cpp' | xargs clang-format --dry-run -Werror
    ;;
  release)
    configure release -DCMAKE_BUILD_TYPE=Release
    "$out/release/louhi_tests"
    "$out/release/louhi-node" --version
    ;;
  events)
    configure events -DCMAKE_BUILD_TYPE=Release -DLOUHI_BUILD_LIR=ON
    "$out/events/louhi_tests"
    ;;
  clang)
    CXX=clang++ configure clang -DCMAKE_BUILD_TYPE=Release
    "$out/clang/louhi_tests"
    ;;
  asan)
    flags="-fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=undefined"
    configure asan -DCMAKE_BUILD_TYPE=RelWithDebInfo -DLOUHI_WARNINGS_AS_ERRORS=OFF -DCMAKE_CXX_FLAGS="$flags" -DCMAKE_EXE_LINKER_FLAGS="$flags" -DCMAKE_SHARED_LINKER_FLAGS="$flags"
    ASAN_OPTIONS=detect_stack_use_after_return=1 UBSAN_OPTIONS=print_stacktrace=1 "$out/asan/louhi_tests"
    ;;
  tsan)
    flags="-fsanitize=thread -fno-omit-frame-pointer"
    configure tsan -DCMAKE_BUILD_TYPE=RelWithDebInfo -DLOUHI_WARNINGS_AS_ERRORS=OFF -DCMAKE_CXX_FLAGS="$flags" -DCMAKE_EXE_LINKER_FLAGS="$flags" -DCMAKE_SHARED_LINKER_FLAGS="$flags"
    # Dannan's suppressions, from the Dannan this build fetched: the Asio
    # internals TSan cannot model (its executor count's fence, its kqueue
    # reactor), which Louhi meets on the same Asio. Nothing of Louhi's is
    # suppressed.
    TSAN_OPTIONS="halt_on_error=1 second_deadlock_stack=1 suppressions=$out/tsan/_deps/dannan-src/dannan/test/tsan-suppressions.txt" "$out/tsan/louhi_tests"
    ;;
  coverage)
    configure coverage -DCMAKE_BUILD_TYPE=Debug -DCMAKE_CXX_FLAGS=--coverage -DCMAKE_EXE_LINKER_FLAGS=--coverage -DCMAKE_SHARED_LINKER_FLAGS=--coverage
    find "$out/coverage" -name '*.gcda' -delete
    (cd "$out/coverage" && ./louhi_tests)
    ignore=inconsistent,inconsistent,mismatch,negative,unused
    lcov --quiet --capture --directory "$out/coverage" --ignore-errors "$ignore" --output-file "$out/all.info"
    lcov --quiet --extract "$out/all.info" "$top/louhi/*" --ignore-errors "$ignore" --output-file "$out/library.info"
    lcov --quiet --remove "$out/library.info" "$top/louhi/test/*" "$top/louhi/fuzz/*" --ignore-errors "$ignore" --output-file "$out/coverage.info"
    rm "$out/all.info" "$out/library.info"
    lcov --summary "$out/coverage.info" --ignore-errors "$ignore"
    ;;
  tidy)
    # clang-tidy's use-after-move over Louhi's own sources, warnings as
    # errors: GCC evaluates arguments right to left on x86-64, so a value used
    # beside its own move in one call is empty on one platform and not on
    # another (found in Lugh, 2026-11-20).
    CXX=clang++ cmake -S "$top" -B "$out/tidy" -G Ninja -DLOUHI_BUILD_TESTING=ON -DCMAKE_EXPORT_COMPILE_COMMANDS=ON -DLOUHI_DANNAN_REPOSITORY="$LOUHI_DANNAN_REPOSITORY" \
      -DLOUHI_LIR_REPOSITORY="$LOUHI_LIR_REPOSITORY" $siblings
    # Configuring writes every generated header; nothing is compiled.
    files=$(cd "$top" && git ls-files 'louhi/*.cpp' 'tools/*.cpp' | sed "s|^|$top/|")
    clang-tidy -p "$out/tidy" --quiet -checks='-*,bugprone-use-after-move' -warnings-as-errors='*' $files
    ;;
  fuzz)
    # Everything instrumented for libFuzzer, so the parsers guide it, not only
    # the harnesses; each target run LOUHI_FUZZ_SECONDS (60) from its corpus.
    # Needs clang with its runtime (libclang-rt-19-dev on Debian 13).
    flags="-fsanitize=fuzzer-no-link,address,undefined -fno-omit-frame-pointer"
    CXX=clang++ configure fuzz -DCMAKE_BUILD_TYPE=RelWithDebInfo -DLOUHI_WARNINGS_AS_ERRORS=OFF -DLOUHI_BUILD_FUZZING=ON -DCMAKE_CXX_FLAGS="$flags" \
      -DCMAKE_EXE_LINKER_FLAGS="-fsanitize=address,undefined"
    for target in record image_reference docker_reply api_body; do
      mkdir -p "$out/fuzz/corpus/$target"
      "$out/fuzz/${target}_fuzz" -max_total_time="${LOUHI_FUZZ_SECONDS:-60}" -print_final_stats=1 "$out/fuzz/corpus/$target"
    done
    ;;
  *)
    echo "usage: tools/ci.sh check|release|clang|asan|tsan|coverage|tidy|fuzz" >&2
    exit 2
    ;;
esac
