# Louhi - Active Work

Last updated: 2026-11-15. `COMPLETED.md` is the record.

## Resume here

**State.** `develop` holds one commit, the skeleton with the plan and design,
made at the maintainer's word and not pushed. Step 0, the skeleton, is written: the
library target, the version header, `louhi_tests`, the hosted CI set, the
rules files and the design. Dannan pinned at `0.8.1`. Gate: see "Step 0"
below. Step 1 has not been started.

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

**Next, in order, when the maintainer says to go on:**
1. the maintainer's answers to "Waiting on the maintainer".
2. Step 1: records and codecs.

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
   change for Louhi. Step 8 waits on this.
4. **What a persistent volume is made of.** the maintainer decides later; step 14.

## The ordered work

The steps and what each delivers are in the design, "Order of work".

### Step 0: skeleton

Written 2026-11-15. Gate on node-c (Pi 5, Debian 13, g++, Ninja,
Release, warnings as errors, never throttled): `tools/ci.sh release` 2
passed, against Dannan 0.8.1. The tagline in the header blocks was changed
after that run (comments only) and the build was not repeated.
`tools/ci.sh check` (header blocks and clang-format) passes on the Mac. The
`clang`, `asan`, `tsan` and `coverage` steps have not been run.
