# Louhi - The Distributed Compute Layer

[![CI](https://github.com/tomdionysus/louhi/actions/workflows/ci.yml/badge.svg)](https://github.com/tomdionysus/louhi/actions/workflows/ci.yml)
[![codecov](https://codecov.io/gh/tomdionysus/louhi/graph/badge.svg)](https://codecov.io/gh/tomdionysus/louhi)
[![CodeQL](https://github.com/tomdionysus/louhi/actions/workflows/codeql.yml/badge.svg)](https://github.com/tomdionysus/louhi/actions/workflows/codeql.yml)
[![License: GPL v3](https://img.shields.io/badge/License-GPLv3-blue.svg)](LICENSE)

Louhi is distributed container compute for a cluster of your own machines, in
the style of ECS and Fargate. You describe a task and how many should run,
never which machine; Louhi places the containers, keeps them running, and
gives each task an address in a private network (subnets, route tables,
security groups, gateways and load balancers) and somewhere to keep its data.
It runs on
[Dannan](https://github.com/tomdionysus/dannan), the network, persists in
[Dagda](https://github.com/tomdionysus/dagda), the store, signs callers in
with Lugh, the directory, and announces what happens on
[Lir](https://github.com/tomdionysus/lir), the event system.

## Status

A skeleton: the library, its version and the test binary. Nothing runs a
container yet.

## Building

Needs a C++20 compiler, CMake 3.24, Boost 1.83 (headers), OpenSSL 3, yaml-cpp
and GoogleTest, and a Dannan checkout beside this one.

```
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DLOUHI_BUILD_TESTING=ON -DLOUHI_WARNINGS_AS_ERRORS=ON
cmake --build build -j8
./build/louhi_tests
```

| Option | Default | Meaning |
|---|---|---|
| `LOUHI_BUILD_TESTING` | `OFF` | Build `louhi_tests` |
| `LOUHI_BUILD_FUZZING` | `OFF` | Build the fuzz targets |
| `LOUHI_WARNINGS_AS_ERRORS` | `OFF` | Treat compiler warnings as errors |

## Licence

GPLv3; see [LICENSE](LICENSE).
