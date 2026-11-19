# Louhi: Compute on Dannan

The design. Written on 2026-11-15, checked against the stack document and the
sibling code the same day, and rewritten to the maintainer's direction after that check.
What the maintainer decided is marked "(the maintainer)" and listed under "Decisions" in
`ACTIVE.md`; the rest follows from those decisions and is his to correct.

## What Louhi is

The compute part of the stack: Dannan networking, Dagda storage, Lugh auth,
Lir events, Louhi compute (the maintainer). Its focus is distributed container compute in
the style of ECS and Fargate, and the networking and storage substrate that
makes it possible (the maintainer): a caller describes a task and how many should run,
never which machine, and Louhi places it, runs it, keeps it running, gives it
an address in a private network and somewhere to keep its data.

**The cluster is ours (the maintainer).** Every node is a machine the operator owns and
trusts, which is the stack's own model: a cluster of trusted nodes, with a
misbehaving member outside the threat model. Contributed, partly trusted
machines (the community cloud) are a later direction, kept in "Later" below
so nothing built now closes it off.

What that leaves untrusted is the workload. A task is someone's code, and it
is boxed in on the node that runs it.

## Vocabulary

ECS's, so anyone who knows it knows this:

| Louhi | Meaning |
|---|---|
| Cluster | One Dannan cluster running Louhi |
| Node | A machine in it that runs tasks (ECS's container instance). Callers never name one |
| Task definition | A versioned, immutable description: containers, images, commands, CPU and memory, ports, volumes, health check |
| Task | One running copy of a task definition, placed on one node |
| Service | Keeps a number of tasks of one definition running, replaces failed ones, rolls out new revisions, registers them with a load balancer |
| Project | Who owns a set of definitions, services, networks and volumes |

A caller runs a task once (a job) or creates a service. Both are Fargate's
shape: sizes are declared per task, and capacity is the cluster's concern.

## What the stack provides

Checked against `../dagda/TODO/2026-11-03-stack.md` and the code where noted;
otherwise as the sibling sessions stated it on 2026-11-15.

**Dannan 0.8.3** (released, in use).
- `dannan::Context` is the whole contract (`dannan/context.h`, read): Nodes,
  Routes, Channels, Identity, Budget, Faults, Time, Telemetry, Status.
- Sessions between every pair of nodes through NATs, on two lanes, with
  frame classes; typed request and reply with deadlines; channels with
  notify, stream and bulk delivery. All over TCP: there is no node-to-node
  datagram path (searched).
- Membership and a roster with watches; per-node adverts, which carry a
  hosted system's own figures (capacity, roles); `directly_reachable`,
  `inbound_capable`, `path` and `latency` per peer.
- **Placement as pure functions over the roster**: rendezvous hashing with
  capacity and failure domains, the caller naming salts and the capacity
  advert (`Nodes::placement`). Every node computes the same answer from the
  same roster.
- The scope check, `Identity::verify(principal, prefix)`, answered by a
  `ScopeSource` that Lugh installs. The stack document names compute as a
  caller of it.
- A public listener layer (`dannan/listen/`: TCP and TLS streams, limits,
  certificates), which Lir and Lugh have moved their listeners onto.
- Message numbers: user types register freely from 512; a clash is refused.
- One process per node: a product binary links the stack, is a node, and
  advertises what it hosts.

**Dagda 0.2.0** (held, unpushed; no ref Louhi can pin yet).
- Head spaces: the owner's state as immutable commits, with the owner's
  codec and merge. `Heads::watch` notifies when a head moves, a peer's commit
  included; no polling.
- Immutable, content-identified objects with claims and garbage collection.
- "It gives no uniqueness, lock, transaction or linearizable write" (stack
  document). Uniqueness at creation is by its claimed collections.
- A lease is "a soft claim, not a lock: two holders can coexist for one
  absence window, and the epoch fence is what keeps the stale one from
  acting" (`dagda/lease/lease.h`, read). Today a lease is seen on one node
  only; cluster-wide leases wait on Dagda's heads step 4.
- Only every-node spaces exist today.

**Lugh 0.1.0** (develop carries OAuth 2, OIDC and `lugh_client`).
- Access tokens verified in-process by `lugh_client`. Groups grant scopes
  under a prefix per registered API. No tenancy and no per-workload
  identities.
- The stack settled that authentication is pluggable in every hosted system,
  with a Lugh plugin that depends on Lugh, and that HTTP stays each hosted
  system's own.

**Lir** (no release).
- Reached only as an MQTT broker over a socket. No client library exists;
  Lir is to export one (the maintainer).

## The shape

### The node

`louhi-node` is a Dannan node: it links the stack, hosts Louhi, and
advertises that it does (one process per node, as the stack decided). It
advertises its capacity (CPU, memory, what is free) and its failure domain
as Dannan adverts, so placement reads them from the roster. Nodes are Linux
only (the maintainer); macOS builds the library and runs the unit tests against fakes.

On each node the agent reconciles: it derives which tasks this node should
be running, compares that with what the container runtime reports, and acts
on the difference (discipline 1, re-derive rather than assert). A container
that dies is handled by its task's or service's policy, with backoff, a
budget, a visible parked state and an operator action (discipline 2). A
restarted agent recovers by resolving against the runtime and the store,
remembering nothing it cannot re-derive (discipline 3). Tasks keep running
while the agent is down or the node is cut off.

### The runtime

Behind an interface, `runtime::Driver`, which speaks of tasks, containers,
images, limits and isolation, never of one runtime's own terms. The first
driver speaks the Docker Engine API over its local socket (the maintainer). The stack
already uses Boost.Beast in Dannan's listener layer, so the HTTP client is
Beast over Asio's local stream socket, with Boost.JSON. The runtime's
replies are untrusted input with a fuzz target.

Tasks are boxed in (the maintainer: hardened containers now, microVMs later): no
privileged mode, no host mounts, dropped capabilities, a seccomp profile,
no new privileges, and CPU, memory, process and disk limits on every
container, with a default where none is declared. User namespaces are a
setting of the Docker daemon, not of a container, so they are part of
preparing a node, which `louhi-node` checks and reports at start. Docker's
own networking is not used: containers are created with no network, and
Louhi attaches the task's interface (below). The interface is shaped so a
microVM driver can follow.

### Records

Declared state lives in Dagda head spaces, keyed so a project's records
share a prefix. Every record is versioned from 1, with a codec written
against Dannan's byte codec.

- **Project** and its **Quota**; **TaskDefinition** (immutable per revision); **Service**
  (definition revision, desired count, deployment settings, load balancer
  attachment); **Task** (the definition revision it runs, and where the
  system decided it runs).
- **TaskStatus** and **NodeStatus**: what a node observes. Written only by
  that node.
- The network records (Vpc, Subnet, RouteTable, SecurityGroup, Gateway,
  LoadBalancer) and the storage records (Volume), below.

Desired state and observed state are separate records with separate
writers, so a merge never reconciles an operator's intent with a node's
report. Each kind's merge is written for what Dagda is: no uniqueness and
no linearizable write. Where two things must not be given the same value
(a task's address, a name), creation goes through Dagda's claimed
collections, and a duplicate that slips through a partition is resolved by
a fixed rule with the loser replaced, never left ambiguous.

### Placement and the service scheduler

A register per task in Dannan's agreement primitive (CASPaxos registers,
Dannan 0.9.0, `../dannan/TODO/2026-11-15-agreement-design.md`), with no
scheduler (the maintainer, 2026-11-15). This replaces the earlier "placement functions
now, a scheduler on a lease later": the maintainer ruled that anything that can use
agreement waits for it, with nothing standing in meanwhile.

- A task's placement is the value of its register. Any node may propose a
  node for it, choosing with Dannan's placement functions over the roster
  (capacity advert as weight, failure-domain spread, only nodes advertising
  Louhi); agreement settles one value, and a node runs exactly the tasks
  whose register names it.
- When a task's node is gone, any node proposes a replacement against the
  register's current value; agreement lets one proposal win. A running task
  is not moved merely because a better node appeared.
- A service's desired count is reconciled the same way: changes to its task
  set go through registers, so two nodes acting at once cannot both add the
  last task.
- A task that must not run twice (`run_once`) is handed its register's
  `changed_at`, the ballot of the last change to its placement: monotonic,
  never reused, comparable by a third party. Its side effects check it, so a
  task left running on a node cut off from the majority is refused by
  anything that has seen a later placement.
- The same primitive serves address allocation (step 9) and quota admission
  (step 5's cross-node half).

Nothing that places tasks across nodes (step 6) or allocates addresses
(step 9) is built before Dannan 0.9.0. A single node places on itself, which
needs no agreement. Placement sits behind an interface, `sched::Placer`, so
the agent and the records do not change with it.

### Projects and quotas

Projects are ours, and each has enforced limits from the start so one
cannot starve another (the maintainer: projects with quotas). A **Quota** record per
project bounds running tasks, CPU, memory, addresses, volumes and load
balancers.

Dagda gives no transaction, so a quota cannot be checked atomically: two
API nodes can each admit a create that together exceed it. So the quota is
enforced twice. Admission refuses what would exceed it on the admitting
node's view, with the reason. And the reconciler that owns a project's
key re-derives its usage from the records (never a stored counter,
discipline 1) and parks the newest tasks past the limit, visibly, until
usage falls or the quota rises. Over-admission is therefore brief and
bounded, and never silent.

### Networking

ECS's `awsvpc` model: every task gets its own interface and address in a
subnet of a VPC, and its own security groups. Layer 3 throughout; no
emulated Ethernet.

- **Vpc**: an isolated address space belonging to a project. Two VPCs may
  use the same range.
- **Subnet**: a range within a VPC, with a route table. Task addresses are
  claimed from it through Dagda's claimed collections.
- **RouteTable**: destinations to targets, evaluated on the sending node.
- **SecurityGroup**: stateful allow rules on a task's interface, applied on
  its node by nftables.
- **Gateway**: a node the operator marks as having public reach performs
  source NAT for a subnet's outbound traffic.
- **LoadBalancer**: layer 4 first, on nodes marked for ingress, spreading
  connections across a service's healthy tasks; HTTP-aware later.
- **Service discovery**: a name per service resolving to its healthy tasks'
  addresses, answered on each node from the records.

On a node, each task has a network namespace and a veth pair whose host
end sits in a per-VPC routing domain (a VRF). Because VPCs may overlap,
everything that keys on an address also keys on the VPC: a connection
tracking zone per VPC for the security groups, and a VPC identifier on
every packet carried between nodes.

**The carrier**, two paths per pair of nodes (the maintainer: kernel when direct,
Dannan otherwise), behind one interface, `net::Carrier`:

- **Dannan path.** A tun device per VPC hands packets to the agent, which
  sends them to the peer agent as notifications of a registered message
  type on the data lane, each carrying its VPC identifier. It works
  wherever Dannan reaches. It is a userspace path over a reliable, ordered
  session, so a lost segment delays every packet behind it and a TCP
  connection inside retransmits on top of the carrier's own. The queue into
  the carrier is small and drops when full, never blocks. Built first, and
  measured before it is relied on.
- **Kernel path.** Between nodes that can reach each other directly, an
  encrypted WireGuard tunnel per pair of nodes, with a VPC-tagged
  encapsulation inside it (the maintainer), since WireGuard alone carries bare IP and
  cannot tell two overlapping VPCs apart. Which encapsulation (VXLAN or
  Geneve) is settled when it is built. Louhi makes the keys per node and exchanges the
  public halves over Dannan. Dannan's `directly_reachable` is a statement
  about TCP, so the kernel path proves its own UDP path before it is used
  and falls back when the proof fails.

A pair of nodes moves between paths as reachability changes, without task
addresses changing.

### Storage

Three things a task needs, in the order they are built:

- **Images.** A task definition names an image by digest, or by a tag that
  Louhi resolves to a digest when the revision is registered, so every task
  of a revision runs the same bytes. Nodes pull from the registries the
  operator allows. An image store of Louhi's own, serving the registry
  protocol from Dagda's immutable objects (which are content-identified, as
  image layers are), is the natural later step, so a cluster does not
  depend on an outside registry to restart its tasks.
- **Ephemeral storage.** Each task gets scratch space with a size limit,
  gone when the task stops. Volumes shared between the containers of one
  task are the same thing.

#### Images in detail (step 13, first half)

- **When a tag becomes a digest.** At registration, on the node that serves
  the API call, before the revision is written. A reference that already
  carries a digest is kept as written and not looked up. A tag that cannot
  be resolved (registry unreachable, unknown, refused) fails the
  registration with the registry's reason; nothing is registered with an
  empty `image_digest` once this lands. The deadline is the API call's.
- **What the digest is of.** The manifest the tag names as served: for a
  multi-platform image the index, not one platform's manifest, so a revision
  pins the same bytes on arm64 and amd64 nodes and each node's runtime picks
  its own platform from the index at pull.
- **How it is asked.** Through the runtime interface, as `pull` already is:
  a `resolve(reference)` operation on the driver. The Docker driver answers
  it with the Engine API's `GET /distribution/{name}/json`, which asks the
  registry for the manifest's descriptor using the daemon's own registry
  handling (TLS, mirrors, the token exchange), so Louhi writes no registry
  client. The reply is untrusted input: bounded, the digest required to be
  `sha256:` and 64 hex digits, anything else refused, a fuzz target
  (`docker_reply` grows a case). A node that serves the API therefore needs
  a runtime, which every node has.
- **Which registries.** The operator lists them (`registries.allow` in the
  node's configuration, by domain; `docker.io` written out to allow Docker
  Hub, which is where a reference with no domain points). The API refuses
  to register a reference to a registry not listed, and the agent refuses to
  pull one (a node's list can be narrower than the API node's). No list
  allows none.
- **Private registries.** Credentials are the operator's, per registry, in
  `secrets.yaml` beside the configuration (user name and password, or an
  identity token), sent as the Engine API's `X-Registry-Auth` on resolve and
  on pull. A project's own registry credentials need a secret store, which
  is not designed yet, and wait for it.
- **Pull by digest.** The agent pulls and creates containers by
  `name@digest` (`agent/specs.cpp`, `image_of`), so a tag moved after
  registration changes nothing already registered.

#### Ephemeral storage in detail (step 13, first half)

Two kinds of writable space exist today, and only one is bounded:

- **Scratch volumes** are tmpfs, bounded by size. Their pages are charged to
  the memory cgroup of the container that writes them, so a container
  filling its scratch is killed for memory long before the volume is full
  if the volume is larger than the container's memory. The bound holds; the
  promise of the size does not.
- **The container's writable layer** (anything written outside a volume
  when the root is not read-only) is unbounded. Docker bounds it only with
  overlay2 on xfs mounted with project quotas (`StorageOpt` `size`); the
  Pis run overlay2 on ext4, where a size is refused at create.

What bounds each (the maintainer, 2026-11-16):

- **The writable layer** is bounded by a task's `ephemeral_storage_mib`
  (1024 when a definition names none), shared equally by its containers,
  and enforced by the runtime
  (`StorageOpt` `size`, so overlay2 on xfs with project quotas). The
  runtime reports whether it can bound it; a node whose runtime cannot
  refuses tasks unless the operator allows unbounded storage
  (`--allow-unbounded-storage`), as with memory and user namespaces.
- **Scratch comes in two kinds,** the definition choosing per volume:
  `memory` (tmpfs, the kind built so far; the sum of a task's memory
  scratch must fit in its memory, since that is where its pages are
  charged) and `disk` (bounded on disk by the same quota mechanism as the
  writable layer: the local driver's `size` option). Where the operator
  allows a runtime that cannot bound storage, neither the writable layer
  nor a disk volume carries a bound, since the runtime would refuse one.
- **Persistent volumes.** A Volume record names storage that outlives a
  task and follows it to another node. What it is made of is open: Dagda
  stores immutable objects and merged heads, not a block device or a POSIX
  filesystem, so a volume is either a filesystem presented from Dagda (as
  Macha presents media), a block device replicated between nodes, or at
  first a directory pinned to one node with the task pinned beside it. the maintainer
  decides this later, once stateless tasks, services and networking work.

### Events

Louhi is an MQTT client of Lir, through the client library Lir is to export
(the maintainer). Lifecycle events (task placed, started, stopped and why; service
deployment progress; node draining) are QoS 1 publishes under a `louhi/`
root, with the current state of each task and service as retained
messages. Node joins and leaves are already on `$dannan/roster`. Lir
carries notifications only: the truth is in Dagda, and a consumer that was
away recovers from there.

**The topics (step 8).** A project's topics sit under its own prefix, so a
Lugh scope `lir:sub:louhi/projects/<project>/#` is exactly one project's
events. Every payload is JSON as the API shows the record; ids as the API
writes them. All QoS 1.

| Topic | Retained | Publisher | Payload |
|---|---|---|---|
| `louhi/projects/{project}/tasks/{task}/nodes/{node}` | yes | the node running it | its TaskStatus, the last one kept (stopped) until the task record is removed, which nothing does yet |
| `louhi/projects/{project}/events` | no | the node where it happened | `{"event", "task", "node", "at", "reason"}` |
| `louhi/nodes/{node}` | yes | that node | its NodeStatus; cleared by its will when it goes |

- Events: `task.run` and `task.stop_requested` from the API node that took
  the request; `task.started`, `task.stopped` and `task.parked` from the
  running node, on the change in its status. `service.*` come with step 7.
- One writer per retained topic: the node is in every task status topic,
  so a task that runs on two nodes for a moment (a placement that moved)
  has two, and neither overwrites the other.
- A node restarted republishes what it observes; an event may come twice,
  and its `at` and the status's `sequence` tell a consumer which is newer.
- A limit of Lir's, stated (Lir, 2026-11-15): a subscription made on one
  Lir node reaches the others at Dannan's heartbeat pace (up to about 5
  s), and a message not retained, published on another node in that
  window, is lost to the new subscriber even at QoS 1. The retained topics
  are unaffected (the current value comes at SUBSCRIBE); a reader of a
  project's events may miss those of its first few seconds, and recovers
  from the retained statuses or the store. It closes with Dannan 0.9.0,
  when Lir holds SUBACK until the other nodes hold the subscription.
- Lir being away never holds up the agent: publishes are queued by
  `lir::client` up to its bound, then dropped and counted.
- `louhi-node` signs in to Lir with a Lugh access token of its own, fresh
  at each CONNECT, granted `lir:pub:louhi/#` and `lir:retain:louhi/#`. It
  gets the token by the client-credentials grant as a Lugh service
  principal, proving itself by `private_key_jwt` (a PKCS#8 key on the node)
  or `client_secret_basic` (a secret in `secrets.yaml`), the operator's
  choice (the maintainer, 2026-11-15: both). With neither, it connects with no
  password, for a Lir that allows anonymous clients.

### API and sign-on

Louhi's own declarative HTTP API on any node (the maintainer: own API, AWS's
vocabulary, no Kubernetes or EC2 compatibility), served on Dannan's
listener layer with Louhi's own HTTP on top, as the stack settled.
Register a task definition, run a task, create or update a service, read
and list, watch.

Sign-in is by the Lugh plugin only, with no anonymous mode (the maintainer,
2026-11-15): `lugh_client` verifies access tokens in process, offline,
against keys it fetches from each trusted issuer. Authorisation is from the
verified token itself, as Lir's is (the maintainer, 2026-11-15): "louhi" among its
audiences, and its scope claim granting `louhi:<project>:<action>` or
`louhi:admin`, until its exp. Dannan's `Identity::verify` is not used: it
answers only from a Lugh scope source installed in the same process, and
`louhi-node` hosts no Lugh. Projects and their quotas are created and set
through the API by `louhi:admin` only. A command-line tool, `louhi-ctl`,
signs in by the device flow. Tasks get no Lugh identity for now (the maintainer:
deferred).

**The endpoints (step 5).** JSON over HTTP/1.1, one version prefix, a
project in every path a project owns, AWS's names for the operations. Every
request carries `Authorization: Bearer <access token>`; none is answered
without one.

| Method and path | AWS's name | Scope |
|---|---|---|
| `PUT /v1/projects/{project}` | (none) | `louhi:admin` |
| `GET /v1/projects` | (none) | any; lists the projects the token holds a scope in, all for admin |
| `GET /v1/projects/{project}` | (none) | `louhi:{project}:read` |
| `PUT /v1/projects/{project}/quota` | (none) | `louhi:admin` |
| `GET /v1/projects/{project}/quota` | (none) | `louhi:{project}:read` |
| `POST /v1/projects/{project}/task-definitions` | RegisterTaskDefinition | `louhi:{project}:register` |
| `GET /v1/projects/{project}/task-definitions` | ListTaskDefinitionFamilies | `louhi:{project}:read` |
| `GET /v1/projects/{project}/task-definitions/{family}` | ListTaskDefinitions | `louhi:{project}:read` |
| `GET /v1/projects/{project}/task-definitions/{family}/{revision}` | DescribeTaskDefinition | `louhi:{project}:read` |
| `POST /v1/projects/{project}/tasks` | RunTask | `louhi:{project}:run` |
| `GET /v1/projects/{project}/tasks` | ListTasks | `louhi:{project}:read` |
| `GET /v1/projects/{project}/tasks/{task}` | DescribeTasks | `louhi:{project}:read` |
| `POST /v1/projects/{project}/tasks/{task}/stop` | StopTask | `louhi:{project}:run` |
| `GET /v1/nodes` | (none) | `louhi:admin` |

- The actions are `read`, `register` and `run` (the maintainer, 2026-11-15);
  `louhi:admin` grants everything and `louhi:{project}:*` every action in
  one project. A scope that does not parse is skipped, as Lir skips one.
- Plain HTTP, and HTTPS on its own port with the operator's certificate,
  each the operator's choice, as Lir serves (the maintainer, 2026-11-15).
- A token bound to a key or a certificate (`cnf`) is refused until its
  binding can be checked, as Lir refuses one it cannot check.
- RegisterTaskDefinition takes the family and the definition, and answers
  the revision it was given: one more than the family's highest. Revisions
  are allocated on the node that takes the request; two nodes allocating
  at once is agreement's to settle (step 6), as quota admission is.
- RunTask takes a family, a revision (the latest when left out), a count
  (1 to 10, as ECS's) and an optional client token: a retry carrying the
  same token within an hour answers the tasks the first made rather than
  making more, on the node that took the first. Admission checks the
  project's quota and refuses, with the limit that would be passed, what
  would exceed it.
- DescribeTasks answers the task and every node's status of it. Lists take
  `limit` (at most 100) and `next`, the key to start after.
- Errors are `{"error": code, "message": text}`: 400 `invalid` (the field at
  fault named, never its value), 401 `unauthenticated` (with the
  verifier's reason), 403 `forbidden` (the scope wanted), 404 `not_found`,
  409 `quota` (the limit passed), 413 `too_large`, 503 `not_ready`. A body
  is at most 1 MiB; a request not read whole in 30 seconds is closed.
- Watching is left for step 8: Lir carries the events.

A principal disabled in Lugh keeps access until its token expires (five
minutes by default). Lugh has designed, not built, principal-change events
published over Lir (`../lugh/TODO/2026-11-15-principal-events.md`):
subscribers recheck anything authorised under an older version and fail
closed when Lugh's heartbeat stops. When that exists, Louhi's Lugh plugin
subscribes; until then the token lifetime is the bound.

## Later

Kept in view, built after the above works, and not designed here:

- **Trusted hosts and contributed machines** (the maintainer: "I like the idea of
  trusted hosts, for later"). The check found that the stack lets any
  member write any shared space, so contributed machines cannot simply be
  members. The likely shape is two tiers: the operator's cluster as it is
  here, and contributed workers that run only an agent, hold no shared
  state, and are told only their own tasks, with trust labels deciding what
  may be placed on them. What this design does now to leave room: callers
  never name a node, placement takes constraints, and observed state is
  kept apart from declared state.
- **Tenants beyond our own projects**: outside users, usage accounting,
  suspension.
- **A microVM driver.**
- **Running the same task on several nodes and comparing results.**
- **Computing on data a node cannot read**, as a small coprocessor.

## Order of work

Each step is a session, ends with its gate, and leaves the tree working.

0. Skeleton: the library, version, test binary, CI set. (Done 2026-11-15.)
1. Records and codecs: Project, Quota, TaskDefinition, Service, Task,
   TaskStatus, NodeStatus; versioned, fuzzed. No I/O.
2. Runtime driver: the interface, a fake, and the Docker Engine driver with
   the hardened profile and limits.
3. `louhi-node` and the agent on one node: run a task from an in-memory
   store; stop, restart, backoff, parked state. First real container on a
   Pi.
4. Store: the records in Dagda head spaces, merges, watches, claimed
   collections. Needs a Dagda ref Louhi can pin.
5. API and `louhi-ctl`: register a definition, run a task, read its status;
   pluggable authentication, the scope check, quota at admission.
6. Placement across nodes through Dannan's agreement registers, and node
   loss. Needs Dannan 0.9.0.
7. Services: desired count, replacement, rolling deployment, health checks;
   quota enforced by the reconciler.
8. Events on Lir. Needs Lir's client library.
9. Task networking: namespaces, addresses, the Dannan carrier, measured.
10. Subnets, route tables, security groups, service discovery.
11. The kernel carrier and moving between the two.
12. Gateways and load balancers.
13. Images by digest and ephemeral storage limits; then the image store on
    Dagda.
14. Persistent volumes, once the maintainer has chosen what they are made of.

Steps 1 to 3 need nothing from Dagda, Lugh or Lir. Steps 4, 6, 8 and 9 each
wait on another project; the order can bend around whichever arrives first.

## What waits on others

As of 2026-11-19 (`ACTIVE.md` has the current state):

- **Dagda:** a ref Louhi can pin, for step 4. Waits on the maintainer's word.
- **Dannan:** the agreement registers, for steps 6 and 9. Released in
  0.9.0; Louhi's pin has still to move to it.
- **Lir:** the exported MQTT client library for step 8. Done.
