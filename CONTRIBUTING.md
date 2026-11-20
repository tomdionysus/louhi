# Contributing to Louhi

Everyone taking part follows the [code of conduct](CODE_OF_CONDUCT.md).

## Read first

`TODO/ACTIVE.md` is the plan and `TODO/COMPLETED.md` is the record. Louhi is
the compute layer, a hosted system on a stack with one set of conventions:
Dannan the network layer (`../dannan`), Dagda the store (`../dagda`), Lugh
the directory (`../lugh`) and Lir the event system (`../lir`).
`../dagda/TODO/2026-11-03-stack.md` says what each owns. Hosted systems use
Dannan as their network layer and Dagda for what they persist, and no code
is duplicated between the projects. Louhi runs containers on a cluster's
nodes from declared state and gives them virtual private clouds, written
new from the design, `TODO/2026-11-15-louhi-design.md`.

`../dannan/CONTRIBUTING.md` is the reference text for the conventions the
projects share; this file is kept identical to it in everything but the
project's name, its build line and its "Read first".

The four laws and five disciplines (`docs/principles-and-laws.md`, a verbatim copy
of Macha's) govern every change here, cited by number. A change that touches
scheduling, admission, priority, retry or recovery says which law it serves and
which discipline it follows.

## Branches

- `main` is the last stable state. Nothing is committed to it directly; stable
  states reach it by merging `develop`.
- `develop` is where work happens.
- Work that needs isolation branches off `develop` as `feature/<short-name>` and
  merges back into `develop`.

## Tags and releases

- Tags are exactly `x.y.z` semver: annotated, no `v` prefix, no suffix, cut from
  `main`.
- The tag, the CMake `project(... VERSION ...)` and `--version` are the same
  number.

## Commits

- Subjects are short imperative prose. No emoji, no ticket prefixes.
- Keep unrelated changes in separate commits: a move, a rename, a fix and a feature
  are four commits. A commit that moves files does not change their content.
- A commit message ends with its last line of prose. No tool attribution trailers.

## Building and testing

Louhi builds against Dannan at a pinned tag, fetched from the sibling checkout
into the build tree.

```
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DLOUHI_BUILD_TESTING=ON \
  -DLOUHI_WARNINGS_AS_ERRORS=ON -DCMAKE_PREFIX_PATH="/opt/local/libexec/boost/1.88;/opt/local"
cmake --build build -j8
./build/louhi_tests
```

Run the test binary rather than `ctest`. `tools/ci.sh <step>` runs one step of
the hosted CI the same way anywhere: `check`, `release`, `clang`, `asan`,
`tsan`, `coverage`. Sanitizers run before a release, or to hunt a bug that
cannot be pinned any other way. What starts real containers or changes a
host's network configuration cannot run in hosted CI: those gates are run by
hand on real nodes, as are multi-node beds (`test/compose/`).

Write tests from the design and the laws, not from the current behaviour.

## Dependencies

Dannan, Dagda, Boost (Asio) and OpenSSL, plus GoogleTest for the test build.
Prefer writing something by hand over adding a library, and say so if a
dependency really is the right call.

## Layout

One layout for all the projects, as `../dannan/CONTRIBUTING.md` "Layout"
gives it, with `<p>` = `louhi`: the library target `louhi` in `louhi/`, public
headers in `louhi/include/louhi/`, subsystems in `louhi/<subsystem>/`,
interfaces in `louhi/contract/`, tests in `louhi/test/` (one binary
`louhi_tests`), fuzz targets in `louhi/fuzz/`, tools in `tools/`, beds in
`test/compose/`. No presets.

## Shared code

What more than one project needs lives once, in the lowest project that
needs it, and the others link it. Louhi links Dannan's crypto primitives,
durable files, byte codec, thread-safety contract, time sources, logging (a
logger from `Context::logger`), Status surface and test fixtures, and
Dagda's heads and objects, and writes none of its own. When Louhi finds
itself writing something another project has, or needs, it asks that
project's session instead.

## Errors

An operation that could not happen throws (`std::invalid_argument` for a
caller's mistake, `std::runtime_error` or a named subclass for bytes or a
system that refused). An operation that happened returns its result, and one
that happened on more than one node returns its evidence (who answered, what
failed and why), never a bare boolean. A read that finds nothing succeeds
with an empty value.

## Style

- Formatting is `.clang-format` (Google base, 160 columns): run `clang-format -i`
  on the files you changed. Two-space indent, `#pragma once`, braces on the same
  line, headers `.h`.
- `snake_case` for functions, methods, variables and files; `PascalCase` for
  types; private and protected members prefixed with `_`. Namespaces are
  `<project>::<subsystem>`.
- Every source file starts with the Louhi header block (`CLAUDE.md` has it).
- No emoji anywhere.
- Dependency injection over globals; the only process global is the io context.
- Each component logs through its own scoped logger.
- Thread-safety annotations on every guarded member; contracts declare their waits
  and thread safety beside each operation.
- Comments are sparse and explain intent, not mechanics.

## Plan

`TODO/ACTIVE.md` is the plan and `TODO/COMPLETED.md` is the record. Move items
across as they land.
