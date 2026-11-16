# Louhi - Completed Work

The record, newest first. `ACTIVE.md` is the plan.

## Step 13, first half: images by digest, ephemeral storage bounds (2026-11-16)

- The design's detail written first ("Images in detail", "Ephemeral
  storage in detail"); the maintainer's answers: resolve through the runtime, an
  operator allow-list with per-registry credentials, xfs project quotas
  with a refusal otherwise, and scratch in two kinds.
- `runtime/registries.h`: the allow-list by domain (`docker.io` for a
  reference with no domain; no list allows none) and one credential per
  allowed registry, a user name with a password or an identity token.
- `Driver::resolve`; the Docker driver asks `GET /distribution/<ref>/json`
  with `X-Registry-Auth` (base64url, by hand), refuses an unlisted registry
  before asking, and pulls with the same header. `parse_distribution` is in
  the `docker_reply` fuzz target.
- The API resolves every container's image before a revision is admitted;
  a digest the caller sends is ignored. Refusals: `image_refused` and
  `image_not_found` (400), `image_unresolved` (503); no revision is spent.
- `TaskDefinition::ephemeral_storage_mib` (64 to 1 TiB, 1024 by default in
  the API), shared equally by its containers as `StorageOpt` `size`.
  `VolumeKind` is `memory` (tmpfs; the sum must fit in the task's memory)
  or `disk` (the local driver's `size` option). Record format 2: version 1
  was never kept.
- `RuntimeInfo::limits_storage` (overlay2 on xfs); the agent and the node's
  status refuse a node without it unless `--allow-unbounded-storage`, and
  then send no bound. `louhi-node --registry`, `--registry-credential`.
- Found on the Pis: Docker answers a storage bound on ext4 with a 500
  ("--storage-opt is supported only for overlay over xfs with 'pquota'"),
  so the driver classes it `unavailable` and the agent's launch backs off
  with that reason. Only reachable where xfs lacks `pquota`.

Gate on node-d (Pi 5, g++, Release, warnings as errors, Docker with
user namespaces, ext4): **237 passed** (207 before, 30 new). Not yet run:
any storage bound kept by a real runtime, since no Pi has xfs.

## Step 8: events on Lir (2026-11-15)

- The topics written into the design's "Events" first; the maintainer's answer on
  how the node signs in to Lir: `private_key_jwt` and `client_secret_basic`,
  both.
- `louhi/events/events.h`: a task's status retained on
  `louhi/projects/<p>/tasks/<t>/nodes/<n>`; `task.started`, `task.stopped`,
  `task.parked` on a change of state, `task.run` and `task.stop_requested`
  from the API once their writes land, on `louhi/projects/<p>/events`; a
  node's status retained on `louhi/nodes/<n>`, cleared by its will or on a
  clean stop. `EventReports` passes the agent's reports to the store first.
- `louhi/events/lir.h`: the publisher over `lir::client` at Lir
  `4443c16` (its pushed develop, with discovery in Dannan's roster),
  fetched with `LIR_BUILD_SERVER=OFF`; publishes Lir does not take are
  counted. `LOUHI_BUILD_LIR` (on); Lir is public, so hosted CI builds it.
- `louhi/auth/service_token.h`: the node's own token by the
  client-credentials grant, refreshed at two thirds of its life, with
  backoff; Lir's password callback reads it without waiting.
- `louhi-node --lir [--lir-tls] [--lir-client-id ...]`. A signing key is
  read as PKCS#8 DER, Dannan's only key format, rather than Louhi parsing
  PEM itself.
- Hosted CI's first public run failed TSan on Asio's executor count (a
  release decrement then an acquire fence, which g++'s TSan cannot model),
  reached through Dannan's connector; `tools/ci.sh tsan` now uses the
  fetched Dannan's own suppressions, as Lir does, and CI is green.

Gate on node-c (Pi 5, g++, Release, warnings as errors, never
throttled, Docker with user namespaces): **207 passed** (200 before, 7
new). Not yet run: a live publish to Lir, and `ServiceToken` against a
real Lugh (`ACTIVE.md`, "Owed, end to end").

## Step 5: the API and louhi-ctl (2026-11-15)

Merged from `feature/api` (`5c1fdce`, merge `f309ecb`).

- `louhi/api/api.h`: the design's endpoints over the store, apart from
  HTTP. Projects and quotas by `louhi:admin` only; every other operation
  behind `louhi:<project>:<read|register|run>`, checked before anything
  about the project is said. Revisions from 1; RunTask on this node, epoch
  1, admitted against the quota (tasks, CPU, memory) counting this node's
  admitted-but-unwritten tasks; a client token remembered an hour; a
  stopped task stops counting. Answers posted, never inline.
- `louhi/api/json.h`: strict, bounded bodies; unknown members refused; the
  field named, never the value; what the API shows registers back as is.
- `louhi/api/auth.h`: Bearer only, audience "louhi", unbound, before exp;
  unparsable scopes skipped.
- `louhi/api/http.h`: HTTP/1.1 on Dannan's listeners, plain and HTTPS each
  optional; body and header limits, a read deadline, one request per
  connection. `louhi/api/client.h`: the client over Dannan's connector.
- `louhi/auth/lugh.h`: the Lugh plugin, `lugh::client` at `a34770f`. With
  it, Louhi's own Boost.JSON definitions are dropped (the double-definition
  question settled); without it (`LOUHI_BUILD_LUGH=OFF`, hosted CI) there
  is no API and no `louhi-ctl`.
- `louhi-node --api-http/--api-https --lugh-issuer`; it now joins its io
  threads before the Api and store go out of scope.
- `louhi-ctl`: device-flow login, refresh, the AWS-named commands.
- Fuzz target `api_body`.
- Found at the first run and fixed: a handler that threw after taking
  `done` answered through the moved-from function (`bad_function_call`);
  the refusal now goes through a copy kept by `handle()`.

Gate on node-c (Pi 5, g++, Release, warnings as errors, never
throttled, Docker with user namespaces): `tools/ci.sh release` **200
passed** (136 before, 64 new). `tools/ci.sh check`'s format and header
checks pass. Not yet run: `louhi-ctl login` against a real Lugh.

## Dannan 0.8.3; step 5's endpoints (2026-11-15)

- Dannan repinned from 0.8.1 to 0.8.3 (`CMakeLists.txt:37`): additive for
  Louhi (the stream connector, RS256, placement copying only its answer).
  Gate on node-d (Pi 5, g++, Release, warnings as errors, never
  throttled, Docker with user namespaces): `tools/ci.sh release` **136
  passed**.
- The API's endpoints written into the design's "API and sign-on". the maintainer's
  answers: scope actions `read`, `register`, `run`; plain HTTP and HTTPS
  each the operator's choice, as Lir serves.

## User namespaces enforced; the Pis prepared (2026-11-15)

- the maintainer: user namespaces are enforced. The agent launches nothing on a
  runtime without them unless the operator allows it
  (`Policy::allow_without_user_namespaces`, `louhi-node
  --allow-without-user-namespaces`), and the node's status is not ready
  without them, as for memory limits. The fake runtime now has them; the
  real-Docker agent test allows both, since not every Pi has them.
- node-d: `cgroup_enable=memory` and Docker `userns-remap: default`
  (root in a container is uid 165536 on the node), one reboot; no daemon.json
  before (`/root/louhi-work/daemon.json.before`, `cmdline.txt.before`).
- node-b: Docker installed, `cgroup_enable=memory`, rebooted with
  the Lir Development Cluster session's checks passed (lir-node and the
  scheduler active, standing by for node-a, the board answering alike). Its
  userns-remap is still to do (a Docker restart).
- node-c: `cgroup_enable=memory` and `userns-remap: default`, one
  reboot, the same checks passed (lir-node, scheduler, Docker and redis
  active; standing by for node-a; the board answering alike).
- node-a is still to do (`ACTIVE.md`, "The build Pis").

Gate on node-d (Pi 5, g++, Release, warnings as errors, never
throttled, Docker 26.1.5 with user namespaces, memory controller on):
`tools/ci.sh release` **136 passed, none skipped**. The driver's real
tests, the hardened profile among them, pass under user namespaces; the
runtime reports memory, CPU and pid limits and user namespaces all on.
Nothing left on the node.

## louhi-node (2026-11-15; step 3's node)

- `tools/louhi_node.cpp`: a Dannan node that advertises "louhi" and hosts
  the agent over the Docker driver and the in-memory store; settings through
  Dannan's config reader (`LOUHI_*`, `/usr/local/etc/louhi/config.yaml`);
  JSON event lines on stdout as `lir-node` does. Stopping it leaves tasks
  running.
- `node/status.h`: the node's NodeStatus from what the runtime says and what
  is placed on it: ready when the runtime answers and can enforce memory
  limits (or the operator allows it), every degradation a condition,
  allocation of tasks wanted running, saturating, runtime text bounded so the
  record always validates. Written under the node's key when it changes.
- `store/memory_store.h`: `Whole::never`, which the node uses. A node's
  store in memory remembers nothing across a restart, so an agent over an
  always-ready one would have taken an empty store for "nothing wanted" and
  removed every task still running from before (discipline 3). Found in
  review before it was run.
- Capacity is not advertised yet: the advert matters only to placement
  across nodes (step 6), which waits on Dannan 0.9.0.

Gate on node-b (Pi 5, Debian 13, g++, Ninja, Release, warnings as
errors, never throttled, Docker 26.1.5, memory controller now on):
`tools/ci.sh release` **133 passed, none skipped**, Docker tests included;
`louhi-node --version` prints 0.1.0. A 25 s run of `louhi-node` on loopback
with its own key: listening, the runtime described with memory, CPU and pid
limits all enforced, ready, two conditions (no user namespaces; the store
not whole), one pass, stopped cleanly, nothing left on the node.

## Store interface and the agent over it (2026-11-15; step 4's first half)

What of step 4 does not wait on a Dagda ref.

- `store/store.h`: the store as Louhi needs it: local reads by key and by
  prefix, asynchronous writes with a deadline, prefix watches as prompts.
  It promises no uniqueness, transaction or linearizable write, because
  Dagda does not.
- `store/memory_store.h`: the store in memory, for tests and a single node
  until the Dagda store exists.
- `agent/store_bindings.h`: `StoreDesired` (the tasks placed on this node
  with the revisions they name; an undecodable record or a missing revision
  is left out and logged) and `StoreReports` (statuses written under each
  status's own key).
- `model/keys.h`: `task_definition_key`, and `tasks_prefix("")` lists every
  project's tasks.

Gate on node-b (Pi 5, Debian 13, g++, Ninja, Release, warnings as
errors, never throttled, no Docker): `tools/ci.sh release` 123 ran, **115
passed, 8 skipped** (the Docker tests, which skip without a daemon; they
passed on node-c at step 3 and nothing they test changed).

## Step 3: the node agent (2026-11-15)

The `louhi::agent` subsystem: what runs a node's tasks.

- `agent/specs.h`: records to the driver's specs. Names and labels tie every
  container and volume to its task; images pinned to their digest; declared
  CPU and memory are a container's own, the rest shared and never nothing.
- `agent/agent.h`: the reconciler. Each pass lists what the runtime has by
  Louhi's labels and acts on the difference with what is placed here:
  launches (pull, volumes, create, start in order), starts what was made but
  not started, removes and remakes a half-made task, stops a task run on its
  own when an essential container exits (kept for inspection, never
  relaunched), relaunches a service's task after a doubling backoff up to a
  budget within a window and then parks it until an operator unparks it
  (discipline 2), retries a failed launch after a backoff with the reason
  reported, tears down what is wanted stopped, and removes what nothing wants
  only once the desired state is whole. It keeps only pacing between passes;
  an agent started over running containers adopts them (disciplines 1 and
  3). Runtime events only prompt a pass; a lost stream is watched again.
  Statuses are reported only on change, with a rising sequence. A node whose
  runtime cannot enforce memory limits launches nothing unless the operator
  allows it.
- `test/fixtures/fake_driver.h`: a runtime in memory, for deterministic tests
  with stepped clocks and no sleep.
- `louhi-node` is not built yet: it becomes a Dannan node with the store
  (the next steps). The real-container gate is a test instead.

Gate on node-c (Pi 5, Debian 13, g++, Ninja, Release, warnings as
errors, never throttled, Docker 26.1.5): `tools/ci.sh release` **118
passed**, first run. Among them `AgentOnDocker.RunsAndStopsARealTask`: a
busybox task placed on the node was launched through Docker, reported
running, exited with 4 and was reported stopped with that reason, then
wanted stopped and removed with its volume; nothing was left on the node. It
ran with the memory override, because the Pi kernel has no memory controller
(`ACTIVE.md`, "Waiting on the maintainer", 0).

## Step 2: the runtime driver (2026-11-15)

The `louhi::runtime` subsystem: the container runtime as the agent sees it,
and the Docker Engine driver.

- `runtime/driver.h`: the `Driver` contract in Louhi's terms (describe,
  pull, volumes, create, start, stop, remove, list, watch), every operation
  with a deadline and the caller's executor, completing once, posted, never
  inline; `Result<T>` with an `Error` kind (unavailable, refused, not found,
  timed out, malformed). `RuntimeInfo` says which limits the node can
  enforce.
- `runtime/labels.h`: the labels and names Louhi gives what it makes, so a
  node re-derives what it runs from the runtime alone.
- `runtime/docker/api.h`: the Docker Engine API (v1.41) as pure functions.
  The hardened profile: Docker's network off, no privilege, all capabilities
  dropped but eight, no new privileges, Docker's seccomp left on, an init,
  private IPC, no restart policy, CPU, memory and process limits always set,
  no swap, volume mounts only, never a host path. Scratch volumes are bounded
  tmpfs. Strict, bounded parsers for every reply, the pull progress stream
  and the event stream (Louhi's containers only).
- `runtime/docker/local_http.h`: HTTP/1.1 over the daemon's local socket
  with Boost.Beast, one connection per request, a deadline on each, bounded
  replies and stream lines, cancellable streams.
- `runtime/docker/docker_driver.h`: the driver; a name or id that goes into
  a request path is checked first, so no caller can change the endpoint.
- Fuzz target `docker_reply` (every reply parser on any bytes). Not yet run
  (owed at the release gate).

Gate on node-c (Pi 5, Debian 13, g++, Ninja, Release, warnings as
errors, never throttled, Docker 26.1.5): `tools/ci.sh release` **96
passed**, the seven driver tests among them run against the real daemon
(pull with digest, the hardened profile as the daemon records it, start,
list by label, the event stream with an exit code, stop, remove, a name in
use refused, a path-changing name refused unsent). Nothing was left behind
on the node (no Louhi containers or volumes; the busybox:1.36 image is kept
as a cache). Hosted CI on the previous push passed every job.

Found on the way, each fixed with its test:
- A complete stream line over its bound slipped through when its newline
  came in the same read (a bug; the test was right).
- The chunked-stream test's data had one chunk size miscounted (the test was
  wrong).
- **The Pi kernel boots with `cgroup_disable=memory`**: Docker on the build
  Pis discards memory limits without an error (`Memory: 0` in the record,
  "WARNING: No memory limit support"). The driver now reports
  `limits_memory`, `limits_swap`, `limits_cpu` and `limits_pids`, and the
  test asserts that a node saying it cannot limit memory indeed records none.
  Fixing the node is the maintainer's (`ACTIVE.md`, "Waiting on the maintainer").
- CodeQL's upload failed on the private repository for want of
  `actions: read` (`ade032c`).

## Step 1: records and codecs (2026-11-15)

The `louhi::model` subsystem: every record of the design's "Records" as a
type with its rules, a versioned codec, and store keys. No I/O.

- `model/names.h`: the name grammar (a DNS label starting with a letter),
  environment names, absolute container paths with no "." or "..".
- `model/image.h`: image references by the Docker distribution grammar,
  strict, printing back exactly as given; sha256 digests only.
- `model/ids.h`: task ids, sixteen random bytes, thirty-two hex digits.
- `model/records.h`: Project, Quota, TaskDefinition (containers, ports,
  environment, mounts, scratch volumes, health checks, run_once), Service,
  Task (with its placement epoch), TaskStatus, NodeStatus, each with
  `validate()` that names the field at fault and never its value.
- `model/codec.h`: each record opens with its kind and format version (1);
  decoders bound every count and length before allocating, accept only
  defined enum and flag bytes, refuse trailing bytes, and validate; encoders
  refuse an invalid record. One spelling per record.
- `model/keys.h`: store keys with a prefix per project and kind; revisions
  sort in order.
- Fuzz targets `record` (every decoder on any bytes: refused, or re-encodes
  to the same bytes) and `image_reference` (refused, or prints back the same
  text). Built only with clang; not yet run (owed at the release gate).

Gate on node-a (Pi 5, Debian 13, g++, Ninja, Release, warnings as
errors): `tools/ci.sh release` **59 passed**, against Dannan 0.8.1.
`tools/ci.sh check` passes. The first run failed one test,
`ImageReference.PathSeparators`: the test was wrong, not the parser (a first
component with a '.' is a domain by the distribution grammar, and '_' is not
in a domain), and was corrected to test what it meant.

Also: hosted CI failed on every job of the first push because
`LOUHI_DANNAN_REPOSITORY` was a CMake PATH cache entry, which turned the
GitHub URL into a directory; it is a STRING now (`2be77b3`).

## Step 0: skeleton (2026-11-15)

The library target, the version header, `louhi_tests`, the hosted CI set
copied from Lir and renamed, the rules files, the plan and the design.
Gate on node-c (Pi 5, Debian 13, g++, Ninja, Release, warnings as
errors, never throttled): `tools/ci.sh release` 2 passed, against Dannan
0.8.1. The private repository `tomdionysus/louhi` was created and `develop`
pushed, at the maintainer's word.
