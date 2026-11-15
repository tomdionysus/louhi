# Louhi - Active Work

Last updated: 2026-11-15. `COMPLETED.md` is the record.

## Resume here

**State.** `develop`, pushed to the private `tomdionysus/louhi` (the maintainer,
2026-11-15: work autonomously, commit and push). Steps 0 (skeleton), 1
(records and codecs), 2 (the runtime driver) and 3 (the node agent) are
done; gates in `COMPLETED.md`. Dannan pinned at `0.8.1`.

**The design** is [`2026-11-15-louhi-design.md`](2026-11-15-louhi-design.md).
It was checked against the stack document
(`../dagda/TODO/2026-11-03-stack.md`) and the sibling code on 2026-11-15, and
rewritten after that check to the maintainer's direction: ECS and Fargate style
container compute with its networking and storage substrate, on a cluster we
own. Read it before anything else here.

**How work is done.** No builds or test runs on the Mac. Builds and tests run
on the shared Pi 5 build servers, claimed on the board first; the same
machines are Louhi's test bed for real containers and overlays (the maintainer,
2026-11-15). Hosts, addresses and the rules for touching them are in
`CLAUDE.local.md`.

`build/pi.sh <address> [step] [gtest filter]` (untracked) syncs the tree and
Dannan's history to a Pi and runs a `tools/ci.sh` step there at `-j2` under
`nice 19`. Claim the Pi first.

**Next, in order:**
1. Step 4 waits on a Dagda ref (the maintainer: wait). What does not wait: the store
   as an interface Louhi owns (records by key, prefix listing, watches), an
   in-memory implementation, and `Desired` and `Reports` over it, so the
   Dagda implementation later replaces one component. Then `louhi-node` as
   a Dannan node hosting the agent over that store.
2. When Louhi reaches step 8: Lir's client is ready (below). It is on
   Dannan 0.8.2 and a project links one Dannan, so Louhi repins to 0.8.2
   (tagged) first.

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

All the maintainer's, 2026-11-15, in the order given. A later one overrides an earlier
one where they differ.

- Louhi is the compute part of the stack: Dannan networking, Dagda storage,
  Lugh auth, Lir events, Louhi compute.
- Dannan is not to change for Louhi; Louhi might use it.
- The build cluster's Pi 5 machines are also a test bed for Louhi.
- Stack dialect, GPLv3, the stack layout.
- **Runtime:** the Docker Engine API over its local socket is the first
  driver, behind an interface.
- **Node platform:** Linux only.
- **Docker on the build Pis:** yes, on all four.
- **Scheduling:** wait for the store. Put back to the maintainer after the check; see
  "Waiting on the maintainer", 1.
- **MQTT client:** Lir exports one; Louhi does not write its own.
- **Overlay carrier:** both. A kernel tunnel where two nodes reach each
  other directly, Dannan frames otherwise.
- **Tenancy:** public tenants from the start. See "Waiting on the maintainer", 2.
- **API:** Louhi's own, declarative, with AWS's vocabulary; no Kubernetes or
  EC2 compatibility.
- **Workload identity:** deferred.
- **Sandbox:** hardened containers now, the runtime interface shaped for a
  microVM driver later.
- **Tagline:** "Louhi - The Distributed Compute Layer".
- **Focus:** ECS and Fargate style distributed container compute, and the
  networking and storage substrate to enable it.
- **The cluster is owned by us.** Every node is trusted. Trusted hosts and
  contributed machines are for later.
- **Placement (replaces "wait for the store"):** Dannan's placement
  functions now; a single scheduler on a lease revisited later.
- **Tenants (replaces "public tenants"):** our own projects, each with
  enforced quotas from the start.
- **Persistent volumes:** what they are made of is decided later.
- **Kernel tunnel:** WireGuard with a VPC tag inside.

## Waiting on the maintainer

0. **Memory limits on the build Pis.** The Raspberry Pi kernel boots with
   `cgroup_disable=memory` (seen in `/proc/cmdline` on node-c;
   `/sys/fs/cgroup/cgroup.controllers` is `cpuset cpu io pids`), so Docker
   discards every container's memory limit with only a warning. A node that
   cannot limit memory cannot keep one task from starving the others or the
   node. The fix is `cgroup_enable=memory` added to
   `/boot/firmware/cmdline.txt` and a reboot, per Pi: the maintainer's hardware and
   disruptive to every session, so not done. Recommend doing it on the
   machines Louhi tests on. Until then Louhi's node will refuse tasks there
   unless started with an explicit operator override, which the tests will
   use. Needed properly by step 3's real-container gate.
1. **Docker on node-a and node-b.** Installed on
   node-c and node-d on 2026-11-15 (Debian `docker.io` and
   `docker-cli`, 26.1.5; a hello-world container ran on each; the nft
   ruleset from before is in `/root/louhi-work/nft-before-docker.txt` on
   each). Node-a and node-b were held by other sessions and are not done.
   Two machines are enough until multi-node work at step 6.
2. **Dagda ref.** the maintainer, 2026-11-15: wait, no change to Dagda's order of
   work; nothing of Louhi's is blocked until step 4. Dagda says 0.2.0
   has every-node head spaces, `Heads::watch` and
   `lease::ClaimedCollection`, is tagged locally and held at the maintainer's ruling
   while Macha's port goes first; pushing it is the maintainer's call and could be
   today. 0.1.5 is pushed and pinnable now but is on Dannan 0.6 and has no
   watch. A 0.2.x with the port fixes is days away. Dagda's caution:
   claimed collections stand on leases, which order work on one node only
   today, so their uniqueness across nodes must be read in
   `dagda/lease/claimed_collection.h` before Louhi relies on it for task
   addresses. Step 4 waits on this.
3. **Lir's client library.** the maintainer gave the go on 2026-11-15 and Louhi passed
   it to Lir, saying Lir may want it from the maintainer directly. Lir says it is one
   to two sessions, independent of Dagda and of Lir's release gate; Louhi
   would pin a Lir commit. One dependency: whether Dannan has an outbound
   TLS stream connector, the counterpart of its listener. the maintainer allowed Louhi
   to ask Dannan as a question only; asked 2026-11-15, the answer goes to
   Lir. If there is none, what follows is the maintainer's, since Dannan is not to
   change for Louhi. Lir, later on 2026-11-15: the maintainer approved the client
   directly in Lir's session. **Ready**: `lir::client` on Lir's develop at
   `542517c` (351 passed on a Pi), not pushed, so fetched from the sibling
   checkout; FetchContent Lir at that commit with `-DLIR_BUILD_SERVER=OFF`
   and link `lir::client` (`lir/client/client.h`). Lir is on Dannan 0.8.2.
   Its outbound connection moves onto Dannan's StreamConnector when that
   0.8 patch ships, with the API unchanged. Nothing waits on the maintainer here now.
4. **What a persistent volume is made of.** the maintainer decides later; step 14.

## The ordered work

The steps and what each delivers are in the design, "Order of work".

Steps 0 and 1 are done (`COMPLETED.md`).

**Owed at the first release gate:** build and run the two fuzz targets
(`record`, `image_reference`) with clang, once each for
`-max_total_time=60`; the `clang`, `asan`, `tsan` and `coverage` steps on a
Pi. Hosted CI runs them on every push meanwhile.

**Hardware note:** node-a and node-d both report
`throttled=0x50000` (under-voltage has occurred since boot). Builds have
passed on both; it is the maintainer's hardware to look at, and is reported, not
acted on.
