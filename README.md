# Louhi - The Distributed Compute Layer

[![CI](https://github.com/tomdionysus/louhi/actions/workflows/ci.yml/badge.svg)](https://github.com/tomdionysus/louhi/actions/workflows/ci.yml)
[![codecov](https://codecov.io/gh/tomdionysus/louhi/graph/badge.svg)](https://codecov.io/gh/tomdionysus/louhi)
[![CodeQL](https://github.com/tomdionysus/louhi/actions/workflows/codeql.yml/badge.svg)](https://github.com/tomdionysus/louhi/actions/workflows/codeql.yml)
[![License: GPL v3](https://img.shields.io/badge/License-GPLv3-blue.svg)](LICENSE)

Louhi is container compute for a cluster of machines you own, in the style of ECS and
Fargate. It is a C++20 library and a node binary: describe a task (its containers, images,
CPU and memory) and how many should run, never which machine, and Louhi places it, runs it,
restarts it by its policy, and reports what it sees. It runs on
[Dannan](https://github.com/tomdionysus/dannan), the network, keeps its records in
[Dagda](https://github.com/tomdionysus/dagda), the store, signs callers in with
[Lugh](https://github.com/tomdionysus/lugh), the directory, and announces what happens
on [Lir](https://github.com/tomdionysus/lir), the event system.

## Why it exists

The rest of the stack moves bytes, keeps state, signs people in and carries events; none of
it runs a program for someone. The usual answers to "run these containers somewhere in the
cluster" are Kubernetes or a cloud's own container service. The first brings its own
control plane, its own consistent store with a leader, its own scheduler and its own idea
of membership, beside the cluster that is already there; the second is someone else's
machines.

Louhi is the compute layer written once for this stack:

- **A caller describes a task, never a machine.** Placement is the system's. Callers name
  a task definition and a count; nodes are never addressed, which also leaves room for
  machines that are only partly trusted later.
- **No scheduler.** Each task's placement is to be settled by Dannan's per-key agreement
  (CASPaxos registers): any node may propose, agreement picks one, and no node's loss stops
  placement. Agreement arrived in Dannan 0.9.0; until Louhi is built on it, a task runs on
  the node that took it.
- **On the cluster that is already there.** Membership, node identity, reachability and
  placement functions are Dannan's; declared state is Dagda's; sign-in is Lugh's. Louhi adds
  only what makes it compute, and has no copy of anything below it.
- **Declared state and observed state apart.** What callers want and what nodes see are
  separate records with separate writers, and every node converges on what is declared,
  re-deriving what it does from the records and the runtime rather than remembering it.
  Containers keep running while a node's agent is down, and a restarted agent adopts them.
- **The task is the untrusted thing.** Every node is the operator's own; the workload is
  someone's code, and it is boxed in on the node that runs it.
- **Projects with quotas from the start.** One project cannot starve another.

## What it does

- **Records.** Projects, quotas, task definitions (immutable per revision), services,
  tasks, and the statuses nodes observe, each versioned from 1 with a bounded codec that
  refuses what it does not understand.
- **The runtime.** A driver interface that speaks of tasks, containers, images, limits and
  isolation; the first driver speaks the Docker Engine API over its local socket. Every
  container is hardened: not privileged, every capability dropped and eight ordinary ones
  (`CHOWN`, `KILL`, `SETUID` and the like) given back, no new privileges, an init process,
  a private IPC namespace, CPU, memory, swap and process limits always, a bound on what it
  writes (the task's ephemeral storage, shared equally), no host mounts (scratch volumes
  are size-limited, in memory and counted in the task's memory, or on disk), and no Docker
  networking. A node whose runtime cannot enforce memory limits, cannot bound storage
  (Docker needs overlay2 on xfs with project quotas) or lacks user namespaces launches
  nothing unless the operator allows it, and says why.
- **Images.** Pulled only from the registries the operator lists, signing in with the
  operator's credentials for each. A tag is resolved to its digest when a definition is
  registered, so every task of a revision runs the same bytes.
- **The agent.** On each node, passes that compare what should run with what the runtime
  has, and act on the difference: launch, start, restart with backoff, stop, remove. A
  service's task that keeps failing is restarted within a budget and then parked: stopped,
  reported, and left for an operator. A task run on its own is done when an essential
  container exits.
- **The API.** Louhi's own declarative HTTP and JSON, in AWS's vocabulary: put a project
  and its quota (`louhi:admin` only), register a task definition, run, list, describe and
  stop tasks, list nodes. Plain HTTP and HTTPS, each the operator's choice. RunTask is
  admitted against the project's quota (tasks, CPU, memory) and takes a client token so a
  retry does not run twice.
- **Sign-in.** Lugh access tokens only, verified offline against each trusted issuer's
  keys, with no anonymous mode. What a token may do is its own claims: `louhi` among its
  audiences, and scopes `louhi:<project>:read`, `register`, `run` or `*`, and `louhi:admin`,
  until it expires.
- **`louhi-ctl`.** The API from a terminal: device-flow sign-in to Lugh, tokens refreshed
  as they run out, one command per operation.
- **Events on Lir.** Each task's status as its node observes it, retained on
  `louhi/projects/<project>/tasks/<task>/nodes/<node>`; `task.run`, `task.started`,
  `task.stopped`, `task.parked` and `task.stop_requested` on `louhi/projects/<project>/events`;
  each node's status retained on `louhi/nodes/<node>`, cleared when it goes. A Lugh scope
  `lir:sub:louhi/projects/<project>/#` is one project's events. Notifications only: the
  records are the truth.

## The stack

Louhi is one of a stack of projects, each its own library and repository, written to the
same conventions. No code is duplicated between them: a piece needed by more than one lives
in exactly one.

```
   Lugh        Lir        Louhi        Macha, AthenaSIP, ...      hosted systems
     |          |           |                |
   Dagda: the store  (objects, claims, leases, heads)
     |
   Dannan: the network  (reachability, routes, channels, membership,
                         identity, placement, agreement, status)
```

- **[Dannan](https://github.com/tomdionysus/dannan)** is the network and the shared
  primitives. Louhi is a Dannan node: it advertises that it hosts Louhi, and links Dannan's
  listeners, connector, logging, settings reader, time sources and byte codec.
- **[Dagda](https://github.com/tomdionysus/dagda)** is the store. Louhi's records are to
  live in its head spaces, keyed so a project's records share a prefix.
- **[Lugh](https://github.com/tomdionysus/lugh)** is the directory and sign-on. To Louhi
  it is an identity provider: the Lugh plugin links Lugh's client library to verify tokens,
  and `louhi-ctl` signs in through it.
- **[Lir](https://github.com/tomdionysus/lir)** is the event system. Louhi publishes its
  events to it as any MQTT client does, through Lir's own `lir::client`, finding Lir's
  nodes in Dannan's roster.

Hosted systems never address one another: Lugh is to Louhi only an identity provider, and
Lir only an MQTT broker.

## How to use it

### Build

A C++20 compiler, CMake 3.24 or later, Boost 1.83 or later (Asio, Beast and JSON; Debian
13's is 1.83), OpenSSL 3, yaml-cpp (for Dannan) and GoogleTest for the tests. Dannan is
fetched from `../dannan` at the pinned release (0.8.3), and Lugh's and Lir's clients from
`../lugh` and `../lir` at pinned commits, so clone them beside this repository. Nodes are
Linux only; macOS builds the library and runs the unit tests against fakes.

```
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DLOUHI_BUILD_TESTING=ON
cmake --build build -j8
./build/louhi_tests
```

| Option | Default | Meaning |
|---|---|---|
| `LOUHI_BUILD_TESTING` | `OFF` | Build `louhi_tests` |
| `LOUHI_BUILD_LUGH` | `ON` | Build the Lugh plugin, the API's sign-in and `louhi-ctl`; off, the node serves no API |
| `LOUHI_BUILD_LIR` | `ON` | Build the events publisher on Lir's client |
| `LOUHI_BUILD_FUZZING` | `OFF` | Build the libFuzzer targets (clang) |
| `LOUHI_WARNINGS_AS_ERRORS` | `OFF` | Treat compiler warnings as errors |
| `LOUHI_DANNAN_REPOSITORY` | `../dannan` | Where Dannan is fetched from: a directory or a URL |
| `LOUHI_LUGH_REPOSITORY` | `../lugh` | Where Lugh is fetched from |
| `LOUHI_LIR_REPOSITORY` | `../lir` | Where Lir is fetched from |

Set `LOUHI_DOCKER_SOCKET=/var/run/docker.sock` to run the driver's tests against a real
Docker daemon; without it they are skipped. `tools/ci.sh` runs each CI step as hosted CI
does.

### Prepare a node

A Linux machine with Docker, and three things Louhi checks and reports at start:

- **User namespaces**, so root in a container is not root on the node: `"userns-remap":
  "default"` in `/etc/docker/daemon.json`, then restart Docker.
- **The memory controller**, so memory limits are enforced; on a Raspberry Pi,
  `cgroup_enable=memory` in `/boot/firmware/cmdline.txt` and a reboot.
- **Storage bounds**, so a task cannot fill the node's disk: Docker's data
  (`/var/lib/docker`) on xfs mounted with `pquota`, with the overlay2 driver.

### Run a node

`louhi-node` is one node: a Dannan node that advertises `louhi` and runs the tasks placed on
it. It reports what it does as one JSON object per line.

```
printf '%s' 'a long shared secret' > cluster.key
./build/louhi-node --key cluster.key --state ./a --advertise 10.0.0.10 \
  --api-http 0.0.0.0:7480 --lugh-issuer https://lugh.example
#   {"event":"listening","id":"...","port":7437}
#   {"event":"api","http":7480,"https":0}
#   {"event":"status","ready":true,"tasks":0,"passes":1,"conditions":0}
```

The API is served when at least one Lugh issuer (`--lugh-issuer`, repeated for each
trusted Lugh; `--lugh-ca PEM` for their roots) and at least one listener are given. For
HTTPS, `--api-https HOST:PORT --api-tls-chain chain.pem --api-tls-key key.pem`. Other
nodes join with `--bootstrap HOST:PORT`. `--allow-unenforced-memory`,
`--allow-without-user-namespaces` and `--allow-unbounded-storage` let a node launch tasks
where the runtime cannot box them in as it should; the node says so in its status.

Images come only from the registries the node lists: `--registry DOMAIN` for each
(`docker.io` for Docker Hub; none listed allows none), and `--registry-credential
DOMAIN=USER:PASSWORD` to sign in to one, kept in `secrets.yaml`.

A node that has left the cluster for good is retired, since Dannan never drops a member
for being absent and it would count against the majority for ever. On each remaining
node in turn, with that node stopped:

```
./build/louhi-node --key cluster.key --state ./a --retire <node id>
#   {"event":"retire","id":"...","retired":true}
```

`--lir` publishes events to the Lir nodes in the node's Dannan cluster (`--lir-tls`, and
`--lir-tls-trust PEM`, for TLS). The node signs in to Lir with a token of its own from the
first Lugh issuer, as a Lugh service principal: `--lir-client-id ID` with either
`--lir-client-key key.der --lir-client-kid KID` (a PKCS#8 DER key, `private_key_jwt`) or
`--lir-client-secret S` (kept in `secrets.yaml`). Without a client, it connects with no
password, for a Lir that allows anonymous clients.

Every flag is also a key in a YAML file and a `LOUHI_*` variable, read by the stack's one
settings reader (Dannan's): the file (`--config PATH`, `$LOUHI_CONFIG`, or
`/usr/local/etc/louhi/config.yaml` when it exists), then `secrets.yaml` beside it, then the
environment, then the flags. `--api-http` is `api: {http: ...}` and `LOUHI_API_HTTP`.
`tools/louhi_node.cpp`, `reader()`, lists every key.

### Use it

Sign in once; the tokens are kept in `~/.config/louhi/credentials.json`, readable only by
you:

```
./build/louhi-ctl login --issuer https://lugh.example --client-id louhi-ctl \
  --endpoint http://10.0.0.10:7480 --scope louhi:admin
```

An administrator makes a project and its quota; a project's members register definitions
and run tasks:

```
./build/louhi-ctl put-project web --description 'The public site'
echo '{"max_tasks":10,"max_cpu_millis":4000,"max_memory_mib":4096}' | ./build/louhi-ctl put-quota web -
./build/louhi-ctl register-task-definition web frontend.json
./build/louhi-ctl run-task web frontend --count 2 --client-token deploy-17
./build/louhi-ctl list-tasks web
./build/louhi-ctl stop-task web <task id> --reason 'done with it'
```

A task definition, as `frontend.json` holds it:

```json
{
  "family": "frontend",
  "cpu_millis": 500,
  "memory_mib": 256,
  "containers": [
    {"name": "app", "image": "alpine:3.20", "command": ["sleep", "3600"],
     "environment": [{"name": "MODE", "value": "production"}]}
  ]
}
```

The endpoints, their scopes and their errors are in the design's "API and sign-on"
(`TODO/2026-11-15-louhi-design.md`).

## Repository layout

```
louhi/model/          records, keys, names, image references and their codecs
louhi/runtime/        the driver contract; runtime/docker/ the Docker Engine driver
louhi/agent/          the node agent and its translation of records to containers
louhi/store/          the store contract and the in-memory store
louhi/node/           the node's status report
louhi/api/            the API: operations, request JSON, sign-in, HTTP server and client
louhi/auth/           the Lugh plugin and the node's own service token
louhi/ctl/            louhi-ctl's commands, credentials and sign-in
louhi/events/         the events and their publisher on Lir
louhi/test/           GoogleTest, one binary (louhi_tests); fixtures in louhi/test/fixtures/
louhi/fuzz/           libFuzzer targets for untrusted bytes
tools/                louhi-node, louhi-ctl, ci.sh
docs/                 the principles and laws shared with the stack
TODO/                 now and next (ACTIVE.md), the design, and what shipped (COMPLETED.md)
```

Conventions, shared with the stack: [CONTRIBUTING.md](CONTRIBUTING.md).

## Status

In development; no release is cut yet. Built and tested (277 tests, the Docker driver's
against a real daemon under user namespaces, with storage bounds kept on xfs): the records
and their codecs; the Docker driver with the hardened profile and limits; images resolved
to digests from allowed registries; ephemeral storage and scratch volume bounds; the agent
on one node, with restart, backoff and the parked state; `louhi-node`; the API with Lugh
sign-in, project scopes and quota admission; `louhi-ctl`; and the events. A node's status
has been published to a real Lir cluster. Not yet run end to end: `louhi-ctl` and the
node's own token against a real Lugh, task events against a real Lir, and a private
registry.

What it does not do yet, in the order it is to be built:

- **Records in Dagda.** The store is in memory until Louhi can pin a Dagda release, so a
  node holds only what was declared since it started, and nodes do not share it.
- **Placement across nodes**, by Dannan's agreement registers (Dannan 0.9; Louhi still
  builds on 0.8.3). Until then a task runs on the node that took the request, and quota is
  admitted on that node's view.
- **Services**: a desired count kept running, replacement, rolling deployment, health
  checks.
- **Task networking**: an address per task in a subnet of a VPC, route tables, security
  groups, service discovery, then gateways and load balancers, carried between nodes over
  Dannan or WireGuard.
- **Images kept in Dagda, then persistent volumes.**

Two limits are stated, not hidden. A principal disabled in Lugh keeps access until its
token expires (five minutes by default), until Lugh publishes principal changes over Lir.
And a reader that has just subscribed to a project's events could miss those of its first
moments while Lir spread the subscription across its nodes; Lir on Dannan 0.9 holds its
acknowledgement until the other nodes have the subscription, for up to a second, which
closes the gap except past that bound. The retained statuses were never affected.

## Licence

Copyright (C) 2026 Tom Cully. Licensed under the GNU GPLv3; see `LICENSE`.
