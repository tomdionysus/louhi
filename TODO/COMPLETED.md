# Louhi - Completed Work

The record, newest first. `ACTIVE.md` is the plan.

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
