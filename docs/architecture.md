# Louhi's architecture

How Louhi is put together and why. Using it is the [README](../README.md); the
full design, with what is still to be built, is
[`TODO/2026-11-15-louhi-design.md`](../TODO/2026-11-15-louhi-design.md), and the
plan is [`TODO/ACTIVE.md`](../TODO/ACTIVE.md).

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
  placement. Until placement uses it, a task runs on the node that took it.
- **On the cluster that is already there.** Membership, node identity, reachability and
  placement functions are Dannan's; declared state is to be Dagda's; sign-in is Lugh's.
  Louhi adds only what makes it compute, and has no copy of anything below it.
- **Declared state and observed state apart.** What callers want and what nodes see are
  separate records with separate writers, and every node converges on what is declared,
  re-deriving what it does from the records and the runtime rather than remembering it.
  Containers keep running while a node's agent is down, and a restarted agent adopts them.
- **The task is the untrusted thing.** Every node is the operator's own; the workload is
  someone's code, and it is boxed in on the node that runs it.
- **Projects with quotas from the start.** One project cannot starve another.

The four laws and five disciplines that govern every change are
[`principles-and-laws.md`](principles-and-laws.md).

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

## The parts

- **Records.** Projects, quotas, task definitions (immutable per revision), services,
  tasks, the statuses nodes observe, and the network records, each versioned from 1 with a
  bounded codec that refuses what it does not understand.
- **The runtime.** A driver interface that speaks of tasks, containers, images, limits and
  isolation; the first driver speaks the Docker Engine API over its local socket. Every
  container is hardened: not privileged, every capability dropped and eight ordinary ones
  (`CHOWN`, `DAC_OVERRIDE`, `FOWNER`, `FSETID`, `KILL`, `NET_BIND_SERVICE`, `SETGID`,
  `SETUID`) given back, no new privileges, an init process, a private IPC namespace, CPU,
  memory, swap and process limits always, a bound on what it writes (the task's ephemeral
  storage, shared equally), no host mounts (scratch volumes are size-limited, in memory and
  counted in the task's memory, or on disk), and no Docker networking. A node whose runtime
  cannot enforce memory limits, cannot bound storage (Docker needs overlay2 on xfs with
  project quotas) or lacks user namespaces launches nothing unless the operator allows it,
  and says why.
- **Images.** Pulled only from the registries the operator lists, signing in with the
  operator's credentials for each. A tag is resolved to its digest when a definition is
  registered, so every task of a revision runs the same bytes.
- **The agent.** On each node, passes that compare what should run with what the runtime
  has, and act on the difference: launch, start, restart with backoff, stop, remove. A
  service's task that keeps failing is restarted within a budget and then parked: stopped,
  reported, and left for an operator. A task run on its own is done when an essential
  container exits.
- **The API.** Louhi's own declarative HTTP and JSON, in AWS's vocabulary, from one route
  table that the dispatch, the scope checks and the OpenAPI document are all read from.
  RunTask is admitted against the project's quota (tasks, CPU, memory) and takes a client
  token so a retry does not run twice.
- **Sign-in.** Lugh access tokens only, verified offline against each trusted issuer's
  keys, with no anonymous mode. What a token may do is its own claims: `louhi` among its
  audiences, and its scopes, until it expires.
- **Admission registers.** The revision and quota registers on Dannan's agreement
  (`louhi/admit/`), built and tested on Dannan's simulation harness; the API does not call
  them yet.
- **Networking groundwork.** The network records and security groups rendered as nftables
  rules, and a task's interface on its node (the command runner, the veth plan, `nft` as
  the firewall, `louhi/net/`), built and tested; `louhi-node` does not use them yet.
- **Events.** Notifications on Lir; the records are the truth.

## Repository layout

```
louhi/model/          records, keys, names, image references, network records and their codecs
louhi/runtime/        the driver contract; runtime/docker/ the Docker Engine driver
louhi/agent/          the node agent and its translation of records to containers
louhi/store/          the store contract and the in-memory store
louhi/admit/          revision and quota registers on Dannan's agreement
louhi/net/            a task's interface on its node: commands, host plan, firewall
louhi/node/           the node's status report
louhi/api/            the API: route table, operations, request JSON, sign-in, OpenAPI, HTTP server and client
louhi/auth/           the Lugh plugin and the node's own service token
louhi/ctl/            louhi-ctl's commands, configuration, credentials and sign-in
louhi/events/         the events and their publisher on Lir
louhi/test/           GoogleTest, one binary (louhi_tests); fixtures in louhi/test/fixtures/
louhi/fuzz/           libFuzzer targets for untrusted bytes
tools/                louhi-node, louhi-ctl, ci.sh
docs/                 this file, and the principles and laws shared with the stack
TODO/                 now and next (ACTIVE.md), the designs, and what shipped (COMPLETED.md)
```

## Limits stated, not hidden

- A principal disabled in Lugh keeps access until its token expires (five minutes by
  default), until Lugh publishes principal changes over Lir.
- A reader that has just subscribed to a project's events could miss those of its first
  moments while Lir spreads the subscription across its nodes. Lir on Dannan 0.9 holds its
  acknowledgement until the other nodes have the subscription, for up to a second, which
  closes the gap except past that bound. The retained statuses are not affected.
