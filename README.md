# Louhi - The Distributed Compute Layer

Louhi is container compute for a cluster of machines you own, in the style of ECS and
Fargate. Describe a task (its containers, images, CPU and memory) and how many should run,
never which machine, and Louhi places it, runs it in a hardened container, restarts it by
its policy, and reports what it sees. Each node runs `louhi-node`; people and scripts use
the HTTP API or `louhi-ctl`.

It runs on the rest of the stack: [Dannan](https://github.com/tomdionysus/dannan) is its
network (membership, node identity, agreement), [Dagda](https://github.com/tomdionysus/dagda)
is to keep its records, [Lugh](https://github.com/tomdionysus/lugh) signs callers in, and
[Lir](https://github.com/tomdionysus/lir) carries its events.

[![Release](https://img.shields.io/github/v/tag/tomdionysus/louhi?sort=semver&label=release)](https://github.com/tomdionysus/louhi/tags)
[![CI](https://github.com/tomdionysus/louhi/actions/workflows/ci.yml/badge.svg)](https://github.com/tomdionysus/louhi/actions/workflows/ci.yml)
[![codecov](https://codecov.io/gh/tomdionysus/louhi/graph/badge.svg)](https://codecov.io/gh/tomdionysus/louhi)
[![CodeQL](https://github.com/tomdionysus/louhi/actions/workflows/codeql.yml/badge.svg)](https://github.com/tomdionysus/louhi/actions/workflows/codeql.yml)
[![License: GPL v3](https://img.shields.io/badge/License-GPLv3-blue.svg)](LICENSE)
[![C++20](https://img.shields.io/badge/C%2B%2B-20-blue.svg)](https://en.cppreference.com/w/cpp/20)
[![Platform: Linux](https://img.shields.io/badge/platform-Linux-lightgrey.svg)](#)
[![Last commit](https://img.shields.io/github/last-commit/tomdionysus/louhi/develop.svg)](https://github.com/tomdionysus/louhi/commits/develop)

## Installing

### From the Debian package

> **Not settled yet.** The package layout is being settled across the stack (Dannan,
> Dagda, Lugh, Lir and Louhi alike), so there is no Louhi package to install yet. When
> there is, it installs two binaries, `louhi-node` and `louhi-ctl`, and this section will
> say where the node's configuration lives. Until then, build from source.

### From source

On Debian 13, the nodes' platform:

```
sudo apt-get install g++ cmake ninja-build git ca-certificates \
  libboost-dev libssl-dev libyaml-cpp-dev libgtest-dev
```

That is a C++20 compiler, CMake 3.24 or later, Boost 1.83 or later, OpenSSL 3 and
yaml-cpp; GoogleTest only for the tests. The build fetches the sibling projects at pinned
versions into the build tree: Dannan `0.9.2`, Dagda `0.3.2`, and Lugh's and Lir's clients
at pinned commits.

The simplest build is `tools/ci.sh`, which takes each sibling from a checkout beside this
one (`../dannan`, `../dagda`, `../lugh`, `../lir`) when there is one and from its public
GitHub repository otherwise:

```
git clone https://github.com/tomdionysus/louhi.git
cd louhi
tools/ci.sh release
# build-ci/release/louhi-node, build-ci/release/louhi-ctl, and the tests run
```

`LOUHI_CI_JOBS` sets the parallelism and `LOUHI_CI_OUT` the output directory. With plain
CMake, the siblings default to the checkouts beside this one; name a URL or another
directory with the `LOUHI_*_REPOSITORY` options:

```
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
./build/louhi-node --version
```

| Option | Default | Meaning |
|---|---|---|
| `LOUHI_BUILD_LUGH` | `ON` | The Lugh plugin, the API's sign-in and `louhi-ctl`; off, the node serves no API |
| `LOUHI_BUILD_LIR` | `ON` | The events publisher on Lir's client; off, no events |
| `LOUHI_BUILD_DAGDA` | `ON` | Fetch and build against Dagda |
| `LOUHI_BUILD_TESTING` | `OFF` | Build `louhi_tests` |
| `LOUHI_BUILD_FUZZING` | `OFF` | Build the libFuzzer targets (clang) |
| `LOUHI_WARNINGS_AS_ERRORS` | `OFF` | Treat compiler warnings as errors |
| `LOUHI_DANNAN_REPOSITORY` | `../dannan` | Where Dannan is fetched from: a directory or a URL |
| `LOUHI_DAGDA_REPOSITORY` | `../dagda` | Where Dagda is fetched from |
| `LOUHI_LUGH_REPOSITORY` | `../lugh` | Where Lugh is fetched from |
| `LOUHI_LIR_REPOSITORY` | `../lir` | Where Lir is fetched from |

Nodes are Linux only. The build has no install step yet: copy `louhi-node` and `louhi-ctl`
where you want them.

## Preparing a node

A Linux machine with Docker. Louhi checks three things at start and reports each one that
is missing in the node's status; a node missing any of them launches no task:

- **User namespaces**, so root in a container is not root on the node:
  `"userns-remap": "default"` in `/etc/docker/daemon.json`, then restart Docker.
- **The memory controller**, so memory limits are enforced. On a Raspberry Pi,
  `cgroup_enable=memory` in `/boot/firmware/cmdline.txt` and a reboot.
- **Storage bounds**, so a task cannot fill the node's disk: Docker's data
  (`/var/lib/docker`) on xfs mounted with `pquota`, with the overlay2 storage driver.

Where one cannot be had, the operator may let the node launch tasks anyway, and the node
says so in its status: `--allow-without-user-namespaces`,
`--allow-unenforced-memory`, `--allow-unbounded-storage`.

## Running louhi-node

`louhi-node` is one node: a Dannan node that advertises `louhi` and runs the tasks placed
on it. It needs the cluster key (a file holding a long shared secret, the same on every
node) and a state directory of its own.

Every setting is a flag, a key in `config.yaml`, and a `LOUHI_*` variable (the key in
capitals, dots as underscores: `api.http` is `--api-http` and `LOUHI_API_HTTP`). The layers
are read in order, each overriding the one before: the file (`--config PATH`,
`$LOUHI_CONFIG`, or `/usr/local/etc/louhi/config.yaml` when it exists), `secrets.yaml`
beside it, the environment, then the flags. A key no setting reads is warned about.

A first node, serving the API over plain HTTP:

```yaml
# /usr/local/etc/louhi/config.yaml
cluster:
  key: /usr/local/etc/louhi/cluster.key
node:
  state: /var/lib/louhi
  advertise: 10.0.0.10        # the address other nodes reach this one at
  port: 7437                  # Dannan's port (the default)
registries:
  allow: [docker.io]          # where images may come from; none listed allows none
api:
  http: 0.0.0.0:7480
lugh:
  issuer: [https://lugh.example]
```

```yaml
# /usr/local/etc/louhi/secrets.yaml, readable by the node's user alone
registries:
  credentials:
    - ghcr.io=someone:a-personal-access-token   # DOMAIN=USER:PASSWORD, or DOMAIN=:IDENTITY_TOKEN
```

```
louhi-node
#   {"event":"listening","id":"...","port":7437}
#   {"event":"api","http":7480,"https":0}
#   {"event":"status","ready":true,"tasks":0,"passes":1,"conditions":0}
```

It reports what it does as one JSON object per line on stdout, a status line every
`--status-ms` (10 seconds by default), and logs through `--log-level` and
`--log-format text|json`. It runs until SIGTERM or SIGINT (or for `--seconds`). Stopping it
leaves the tasks running; started again, it adopts them.

The settings, by key and flag:

| Key | Flag | Meaning |
|---|---|---|
| `cluster.key` | `--key` | The cluster key file (required) |
| `cluster.bootstrap` | `--bootstrap` | `HOST:PORT` of a node to join by; a list |
| `node.state` | `--state` | The state directory (required) |
| `node.listen`, `node.port`, `node.advertise` | `--listen`, `--port`, `--advertise` | Dannan's listener (default `0.0.0.0:7437`) and the host others reach |
| `node.name`, `node.domain` | `--name`, `--domain` | A name, and a failure domain |
| `timing.heartbeat_ms`, `timing.status_ms`, `timing.seconds` | `--heartbeat-ms`, `--status-ms`, `--seconds` | Heartbeat (5000), status interval (10000), seconds to run (0, until stopped) |
| `log.level`, `log.format` | `--log-level`, `--log-format` | Logging |
| `runtime.docker_socket` | `--docker-socket` | Docker's socket (default `/var/run/docker.sock`) |
| `runtime.allow_*` | `--allow-*` | The three overrides above |
| `registries.allow` | `--registry` | A registry images may come from, by domain (`docker.io` for Docker Hub) |
| `registries.credentials` | `--registry-credential` | `DOMAIN=USER:PASSWORD` or `DOMAIN=:IDENTITY_TOKEN`; keep in `secrets.yaml` |
| `api.http`, `api.https` | `--api-http`, `--api-https` | `HOST:PORT` for the API over HTTP and over HTTPS |
| `api.tls_chain`, `api.tls_key` | `--api-tls-chain`, `--api-tls-key` | The HTTPS certificate chain (PEM, leaf first) and key |
| `lugh.issuer` | `--lugh-issuer` | A trusted Lugh, as its tokens name it; a list |
| `lugh.ca` | `--lugh-ca` | The roots the issuers' HTTPS chains to; the system's when absent |
| `lir.enabled` | `--lir` | Publish events to Lir |
| `lir.tls`, `lir.tls_trust` | `--lir-tls`, `--lir-tls-trust` | Reach Lir over TLS, and the roots its certificate chains to |
| `lir.client_id` | `--lir-client-id` | This node's Lugh client, for its token to Lir |
| `lir.client_key`, `lir.client_kid` | `--lir-client-key`, `--lir-client-kid` | Its signing key (PKCS#8 DER, `private_key_jwt`) and the key's kid |
| `lir.client_secret` | `--lir-client-secret` | Or its secret (`client_secret_basic`); keep in `secrets.yaml` |

### Joining a cluster

Every node shares the cluster key. A new node joins by naming one or more running nodes:

```yaml
cluster:
  key: /usr/local/etc/louhi/cluster.key
  bootstrap: [10.0.0.10:7437]
```

Any node may serve the API; give each its own listeners and the same issuers. Until
records are shared (see Status), a task runs on the node whose API took it.

### The API's listeners and Lugh issuers

The API is served when at least one Lugh issuer and at least one listener are given; there
is no anonymous sign-in, so a listener without an issuer is refused. For HTTPS, set
`api.https` with `api.tls_chain` and `api.tls_key`; HTTP and HTTPS may both be served. A
token is accepted from any issuer listed in `lugh.issuer`, when `louhi` is among its
audiences.

### Events on Lir

With `lir.enabled`, the node publishes to the Lir nodes it finds in its Dannan cluster.
It signs in with a token of its own from the first Lugh issuer, as a Lugh service
principal: `lir.client_id` with either `lir.client_key` and `lir.client_kid`, or
`lir.client_secret`. Without a client it connects with no password, for a Lir that allows
anonymous clients.

| Topic | What |
|---|---|
| `louhi/projects/<project>/tasks/<task>/nodes/<node>` | A task's status as that node observes it, retained |
| `louhi/projects/<project>/events` | `task.run`, `task.started`, `task.stopped`, `task.parked`, `task.stop_requested` |
| `louhi/nodes/<node>` | The node's status, retained, cleared when it stops |

A Lugh scope `lir:sub:louhi/projects/<project>/#` is one project's events. They are
notifications; the API's records are the truth.

### Retiring a node

Dannan never drops a member for being absent, so a node that has left for good counts
against the majority until it is retired. On each remaining node in turn, with that node
stopped (the command opens its state and is refused while it runs):

```
louhi-node --key cluster.key --state /var/lib/louhi --retire <node id>
#   {"event":"retire","id":"...","retired":true}
```

`retired` is false when the node was not a member. A retired node that is still running
becomes a member again when it is next seen.

## Using louhi-ctl

`louhi-ctl` is the API from a terminal. Sign in once, by Lugh's device flow:

```
louhi-ctl login --issuer https://lugh.example --client-id louhi-ctl \
  --endpoint http://10.0.0.10:7480 --endpoint http://10.0.0.11:7480 --scope louhi:admin
# To sign in, visit https://lugh.example/... and enter the code ...
```

Login writes the issuer, client id, endpoints and scopes (and `--ca`, when given) to
`~/.config/louhi/config.yaml`, replacing it, and the tokens to
`~/.config/louhi/credentials.yaml`, readable only by you. The access token is refreshed
when less than a minute of it is left. `louhi-ctl logout` removes the tokens.

The configuration's keys are `issuer`, `client_id`, `endpoints` and `scopes` (lists), `ca`
(the roots an https endpoint and the issuer chain to) and `credentials` (the credentials
file). It is read from `--config PATH`, `$LOUHI_CONFIG`, or `~/.config/louhi/config.yaml`;
then `$LOUHI_ENDPOINT` (a list, separated by commas or spaces) and `$LOUHI_CREDENTIALS`;
then the flags `--endpoint`, `--ca` and `--credentials`, which any command takes.
Credentials kept by an older `louhi-ctl` in `~/.config/louhi/credentials.json` are moved to
the YAML files at the first call.

Each command prints the answer's JSON and exits 0 for a 2xx answer, 1 for any other answer
or failure, 2 for a usage error.

| Command | Example |
|---|---|
| `list-projects` | `louhi-ctl list-projects` |
| `put-project` | `louhi-ctl put-project web --description 'The public site'` |
| `describe-project` | `louhi-ctl describe-project web` |
| `put-quota` | `louhi-ctl put-quota web quota.json` (`-` reads stdin) |
| `describe-quota` | `louhi-ctl describe-quota web` |
| `register-task-definition` | `louhi-ctl register-task-definition web frontend.json` |
| `list-task-definition-families` | `louhi-ctl list-task-definition-families web` |
| `list-task-definitions` | `louhi-ctl list-task-definitions web frontend` |
| `describe-task-definition` | `louhi-ctl describe-task-definition web frontend 3` |
| `run-task` | `louhi-ctl run-task web frontend --revision 3 --count 2 --client-token deploy-17` |
| `list-tasks` | `louhi-ctl list-tasks web --limit 50 --next <task id>` |
| `describe-task` | `louhi-ctl describe-task web <task id>` |
| `stop-task` | `louhi-ctl stop-task web <task id> --reason 'done with it'` |
| `list-nodes` | `louhi-ctl list-nodes` |

A quota, and a task definition:

```
echo '{"max_tasks":10,"max_cpu_millis":4000,"max_memory_mib":4096}' | louhi-ctl put-quota web -
```

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

**Endpoint failover.** Any node serves the API, so each call goes to the endpoints in
turn until one answers: past one that cannot be connected to, and for a read past one that
gives no answer as well. A write that may have reached a node is never sent to another;
`louhi-ctl` says it may have happened. Any HTTP answer, an error included, is the answer.
Until records are shared between nodes (see Status), each node answers from what was
declared to it.

## The API in brief

JSON over HTTP/1.1. Every request carries `Authorization: Bearer <Lugh access token>`, with
`louhi` among the token's audiences. A token's scopes are `louhi:admin` (everything) and
`louhi:<project>:<action>`, the action `read`, `register`, `run`, or `*` for all three.

| Method and path | Operation | Scope |
|---|---|---|
| `GET /v1/status` | GetStatus | any token; the detail for `louhi:admin` |
| `GET /v1/openapi.json` | GetOpenApi | any token |
| `GET /v1/nodes` | ListNodes | `louhi:admin` |
| `GET /v1/projects` | ListProjects | any token; the projects it holds a scope in, all for admin |
| `PUT /v1/projects/{project}` | PutProject | `louhi:admin` |
| `GET /v1/projects/{project}` | GetProject | `louhi:{project}:read` |
| `PUT /v1/projects/{project}/quota` | PutQuota | `louhi:admin` |
| `GET /v1/projects/{project}/quota` | GetQuota | `louhi:{project}:read` |
| `POST /v1/projects/{project}/task-definitions` | RegisterTaskDefinition | `louhi:{project}:register` |
| `GET /v1/projects/{project}/task-definitions` | ListTaskDefinitionFamilies | `louhi:{project}:read` |
| `GET /v1/projects/{project}/task-definitions/{family}` | ListTaskDefinitions | `louhi:{project}:read` |
| `GET /v1/projects/{project}/task-definitions/{family}/{revision}` | DescribeTaskDefinition | `louhi:{project}:read` |
| `POST /v1/projects/{project}/tasks` | RunTask | `louhi:{project}:run` |
| `GET /v1/projects/{project}/tasks` | ListTasks | `louhi:{project}:read` |
| `GET /v1/projects/{project}/tasks/{task}` | DescribeTasks | `louhi:{project}:read` |
| `POST /v1/projects/{project}/tasks/{task}/stop` | StopTask | `louhi:{project}:run` |

`/v1/openapi.json` is the OpenAPI document, written from the same route table the API
dispatches from. `/v1/status` is the node as it sees itself: its id, version, readiness
and its store's, and for `louhi:admin` its task count and full report. Errors are
`{"error": code, "message": text}`.

## Status

This is Louhi's first release. What works today, on one node: the records and their
codecs; the Docker driver with the hardened profile, limits and storage bounds; images by
digest from allowed registries; the agent, with restart, backoff and the parked state;
`louhi-node`; the API with Lugh sign-in, project scopes and quota admission; `louhi-ctl`;
and events on Lir.

What it does not do yet:

- **Placement across nodes.** A task runs on the node that took the request, and quota is
  admitted on that node's view, until Dannan's agreement registers have a durable store
  (Dagda's acceptor store).
- **Shared, durable records.** Records are kept in memory until Dagda's register map
  ships: a node holds only what was declared to it since it started, and nodes do not share
  them.
- **Task networking.** The groundwork (network records, security groups as nftables, a
  task's interface on its node) is built and tested but not wired into `louhi-node`; tasks
  have no Docker networking.
- **Services**, persistent volumes and images kept in Dagda.

The plan, in order, is [`TODO/ACTIVE.md`](TODO/ACTIVE.md).

## More

- [docs/architecture.md](docs/architecture.md): why Louhi is built as it is, its parts,
  the stack and the repository's layout.
- [docs/principles-and-laws.md](docs/principles-and-laws.md): the laws and disciplines
  every change follows.
- [CONTRIBUTING.md](CONTRIBUTING.md): conventions, branches, tests.
- [TODO/2026-11-15-louhi-design.md](TODO/2026-11-15-louhi-design.md): the design.

## Code of conduct

Everyone taking part follows the [code of conduct](CODE_OF_CONDUCT.md).

## Licence

Copyright (C) 2026 Tom Cully. Licensed under the GNU GPLv3; see `LICENSE`.
