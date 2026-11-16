# Louhi - Active Work

Last updated: 2026-11-16 (end of session). `COMPLETED.md` is the record.

## Resume here

**State.** `develop` at `tomdionysus/louhi`, public and pushed (the maintainer,
2026-11-15). Steps 0 to 3, 5 and 8 are done, and step 13's first half:
records and codecs, the Docker driver, the agent, the store interface with
the in-memory store, `louhi-node`, the API with sign-in by the Lugh plugin
and `louhi-ctl`, events on Lir, and images by digest with storage bounds.
Last gate: 237 passed on node-c, Docker tests included. Pins:
Dannan `0.8.3`, Lugh `382416d`, Lir `4443c16`.

**Read first:** the design, [`2026-11-15-louhi-design.md`](2026-11-15-louhi-design.md);
then this file's Decisions. the maintainer's standing instruction: work autonomously
until he is needed, commit and push; decisions go to him as interactive
questions with a recommendation. One step per session.

**How work is done.** Nothing is built or run on the Mac (TSan there only
to hunt a specific bug, as relayed from the maintainer on 2026-11-16; not yet in
`CLAUDE.md`). Builds and tests run on the shared Pi 5s, claimed on the
board first (`CLAUDE.local.md` has hosts and rules). `build/pi.sh <address>
[step] [gtest filter]` (untracked) syncs the tree and Dannan's, Lugh's and
Lir's histories to a Pi and runs a `tools/ci.sh` step there at `-j2` under
`nice 19`. To iterate on compile errors, rsync and run `ninja -k 0 -j2` in
the Pi's `build-ci/release` rather than a whole gate. One board wait per
session: a second `claim-board wait` replaces the first.

## Next, in order

1. **Step 13, owed:** a storage bound kept by a real runtime, which needs a
   Pi with `/var/lib/docker` on xfs with `pquota` (waits on the maintainer, below);
   a private registry signed in to for real. The image store on Dagda is
   step 13's second half and waits on the Dagda ref.
2. **Owed, end to end:** `louhi-ctl login` against a real Lugh issuer;
   `ServiceToken` (`louhi/auth/service_token.h`) against a real Lugh; a live
   publish to Lir (waits on the maintainer, below). Neither `louhi-ctl` nor
   `ServiceToken` has tests of its own.
3. **Step 4's second half,** the Dagda store: waits on a Dagda ref.
4. **Steps 6, 7 and 9** wait on Dannan 0.9.0's agreement registers. A
   service's task set changes through registers, so services wait with
   placement (the maintainer: anything that can use agreement waits for it).

## Waiting on the maintainer

1. **Live events on Lir.** `louhi-node` finds Lir in its Dannan roster, so
   a live publish means joining a Dannan cluster with Lir in it: the
   standing cluster (Lir's anonymous scopes there allow only `stack/#` and
   `test/#`; nothing under `louhi/` until the maintainer says so), or a Lir node of
   Louhi's own on a Pi. Recommendation: a Lir node of Louhi's own, removed
   afterwards.
2. **Hosted CI and Lugh.** Lugh is private, so hosted CI builds without the
   plugin (no API, no `louhi-ctl`). A Lugh read token as a repository secret
   (`LUGH_READ_TOKEN`, as Lir has) would let a job build it.
   Recommendation: add it.
3. **Dagda ref.** the maintainer: wait. Dagda 0.2.0 is tagged locally and held. Its
   caution: claimed collections stand on leases, which order work on one
   node only today.
4. **Under-voltage:** node-a and node-d report
   `throttled=0x50000`. node-d went down three times on
   2026-11-16: before 00:34Z, about 01:06Z, and again about 01:21Z (Lir's
   report). It needs a power-cycle from the maintainer. Louhi builds nothing there
   until the maintainer says it is sound.
5. **xfs for Docker on a Pi.** Every Pi runs overlay2 on ext4, which cannot
   bound storage, so no Pi launches a task unless `louhi-node` runs with
   `--allow-unbounded-storage`. To test a kept bound, one Pi needs
   `/var/lib/docker` on xfs mounted `pquota`. Recommendation: a loop-mounted
   xfs image file (16 GB) at `/var/lib/docker` on node-c, which has
   NVMe. Docker stops while its data moves, so whatever runs in Docker there
   (redis, per the table below) stops with it. Undone by unmounting and restoring the old
   directory.

## The build Pis

| Host | Docker | User namespaces | Memory controller | Notes |
|---|---|---|---|---|
| node-b | yes | **no** (to do) | on | lir-node, scheduler |
| node-c | yes | yes | on | lir-node, scheduler, redis |
| node-d | yes | yes | on | lir-node |
| node-a | **no** (to do) | **no** | **off** (to do) | lir-node, the acting scheduler; clang 19, no ninja |

Owed, at the maintainer's go already given; previous settings go to `/root/louhi-work/`
on each Pi first (`cmdline.txt.before`, `daemon.json.before`,
`nft-before-docker.txt`):

- **node-a:** install `docker.io` and `docker-cli` (save the nft
  ruleset first), `userns-remap: default` in `/etc/docker/daemon.json`,
  `cgroup_enable=memory` in `/boot/firmware/cmdline.txt` (change nothing
  else), one reboot. Lir cleared
  it on 2026-11-15 and asked to be told before and after. It needs the
  whole machine: post a note, claim cores "all" for about 20 minutes, queue
  for it if held (Dagda had a seven-hour measurement queued there). Checks
  after: `systemctl is-active lir-node claim-board-scheduler`;
  `journalctl -u claim-board-scheduler -n 3 -o cat` says "acting" on
  node-a and node-b's goes back to "standing by: node-a acts";
  the board's `/retained?filter=stack/%23` on node-a answers as another
  node's does. Slow to come back: suspect its power supply first.
- **node-b:** `userns-remap: default` and `systemctl restart docker`
  while node-b is unclaimed, with a note; no reboot. Check `docker info`
  shows `name=userns`.

## Principles

- The four laws and five disciplines, `docs/principles-and-laws.md`, cited by
  number.
- A caller describes a task, never a machine.
- Declared state in Dagda; nodes converge towards it; nothing is remembered
  that cannot be re-derived.
- Desired state and observed state are separate records with separate
  writers.
- Nothing here claims a guarantee the stack does not give: no exactly-once,
  no uniqueness without a claim, no lock.

## Decisions

All the maintainer's, 2026-11-15. Superseded ones are folded in; the history is in git.

- **Scope:** the stack's compute layer (Dannan networking, Dagda storage,
  Lugh auth, Lir events, Louhi compute): ECS and Fargate style container
  compute with the networking and storage substrate to enable it.
- **The cluster is owned by us;** every node is trusted. Trusted hosts and
  contributed machines are for later, and nothing built may close that off.
- **Dannan is not to change for Louhi;** Louhi uses it as it stands.
- Stack dialect, GPLv3, the stack layout; tagline "Louhi - The Distributed
  Compute Layer". The repository is public.
- **Runtime:** the Docker Engine API over its local socket, behind an
  interface. **Node platform:** Linux only.
- **Sandbox:** hardened containers now, the runtime interface shaped for a
  microVM driver later. Memory limits and user namespaces are enforced: the
  agent launches nothing on a runtime lacking either unless the operator
  allows it.
- **Placement:** a register per task in Dannan's agreement (0.9.0): any
  node proposes, agreement settles one node, no scheduler. `changed_at` is
  the fencing token for run_once tasks. The same primitive serves address
  allocation and quota admission across nodes. Nothing that places tasks
  across nodes, allocates addresses or keeps a service's count is built
  before 0.9.0; no stand-in meanwhile.
- **Tenants:** our own projects, each with enforced quotas from the start,
  created and set through the API by `louhi:admin` only.
- **API:** Louhi's own, declarative, AWS's vocabulary; no Kubernetes or EC2
  compatibility. Sign-in by the Lugh plugin only, no anonymous mode;
  authorisation from the token's audience ("louhi") and scopes
  (`louhi:<project>:read|register|run|*`, `louhi:admin`); plain HTTP and
  HTTPS each the operator's choice.
- **Events:** on Lir, through Lir's exported `lir::client`; Louhi writes no
  MQTT client. The node signs in to Lir as a Lugh service principal by
  `private_key_jwt` or `client_secret_basic`, the operator's choice.
- **Workload identity:** deferred.
- **Overlay carrier:** both: WireGuard (with a VPC tag inside) where two
  nodes reach each other directly, Dannan frames otherwise.
- **Persistent volumes:** what they are made of is decided later (step 14).
- **Images (2026-11-16):** a tag is resolved to a digest at registration
  through the runtime (the Docker driver's `GET /distribution`), no registry
  client of Louhi's own; registries are an operator allow-list (no list
  allows none) with the operator's credentials per registry in
  `secrets.yaml`; per-project credentials wait for a secret store.
- **Ephemeral storage (2026-11-16):** the writable layer is bounded per task
  by xfs project quotas through the runtime, and a node that cannot bound
  it refuses tasks unless the operator allows it. Scratch has two kinds,
  memory (tmpfs, counted in the task's memory) and disk (quota-bounded).
- **The build Pis** are also Louhi's test bed: Docker, user namespaces and
  the memory controller on all four.

## Owed at the first release gate

- The four fuzz targets (`record`, `image_reference`, `docker_reply`,
  `api_body`) built with clang and run `-max_total_time=60` each.
- The `clang`, `asan` and `coverage` steps on a Pi.
- TSan: every Pi boots the 4K-page kernel since 2026-11-16 (Lir session,
  at the maintainer's request), so g++'s TSan runs on all four. Sanitizer builds go on
  node-b or node-c (15 GB), never node-d (4 GB, ran out of memory);
  `free -m` first.

Hosted CI runs all of these but the fuzzers on every push meanwhile.
