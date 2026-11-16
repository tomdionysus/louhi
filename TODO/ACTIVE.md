# Louhi - Active Work

Last updated: 2026-11-15 (end of session). `COMPLETED.md` is the record.

## Resume here

**State.** `develop`, pushed to `tomdionysus/louhi`, public since
2026-11-15 at the maintainer's word. Steps 0 to 3, 5 and 8 are done: records and codecs, the Docker
driver, the agent, the store interface with the in-memory store,
`louhi-node`, the API with sign-in by the Lugh plugin and `louhi-ctl`, and
events on Lir. Last gate: 207 passed on node-c, Docker tests
included, against Dannan `0.8.3`, Lugh `a34770f` and Lir `4443c16`; hosted
CI green (without Lugh).

**Read first:** the design, [`2026-11-15-louhi-design.md`](2026-11-15-louhi-design.md);
then this file's Decisions. the maintainer's standing instruction: work autonomously
until he is needed, commit and push; decisions go to him as interactive
questions with a recommendation.

**How work is done.** Nothing is built or run on the Mac. Builds and tests
run on the shared Pi 5 build servers, claimed on the board first
(`CLAUDE.local.md` has hosts and rules). `build/pi.sh <address> [step]
[gtest filter]` (untracked) syncs the tree and Dannan's, Lugh's and Lir's histories to a Pi and
runs a `tools/ci.sh` step there at `-j2` under `nice 19`. One board wait per
session at a time: a second `claim-board wait` replaces the first; wait on
several hosts in one command.

## Next, in order

1. **Owed, end to end:** `louhi-ctl login` and a live sign-in against a
   real Lugh issuer; `ServiceToken` (the node's client-credentials token,
   `louhi/auth/service_token.h`) against a real Lugh; and a live publish
   to Lir. `louhi-node` finds Lir in its Dannan roster, so the last means
   joining a Dannan cluster that has Lir in it: the standing cluster, with
   Lir's and the maintainer's word, or a Lir node of Louhi's own on a Pi. Neither
   `louhi-ctl` nor `ServiceToken` has tests of its own.
2. **Step 13's first half,** which waits on nobody: a tag resolved to a
   digest when a revision is registered (the registry's manifest, the
   untrusted reply bounded and fuzzed), so every task of a revision runs
   the same bytes; and ephemeral storage limits enforced. The image store
   on Dagda is its second half.
3. **Step 4's second half**, the Dagda store, waits on a Dagda ref.
4. **Steps 6, 7 and 9** wait on Dannan 0.9.0's agreement registers: a
   service's task set changes through registers (design, "Placement"), so
   services wait with placement (the maintainer's ruling: anything that can use
   agreement waits for it).

## The build Pis

| Host | Docker | User namespaces | Memory controller | Standing services |
|---|---|---|---|---|
| node-b | yes | **no** (to do) | on | lir-node, scheduler |
| node-c | yes | yes | on | lir-node, scheduler, redis |
| node-d | yes | yes | on | lir-node (a fourth Lir node since 2026-11-15) |
| node-a | **no** (to do) | **no** | **off** (to do) | lir-node, scheduler (the acting one) |

Still owed, all at the maintainer's go already given:
- **node-a:** install Docker (`docker.io`, `docker-cli`; save the
  nft ruleset first to `/root/louhi-work/nft-before-docker.txt`), set
  `userns-remap: default` in `/etc/docker/daemon.json`, add
  `cgroup_enable=memory` to `/boot/firmware/cmdline.txt` (copy first to
  `/root/louhi-work/cmdline.txt.before`), one reboot. Lir (2026-11-15,
  late): its rolling lir-node upgrade finished at about 22:40Z, all four on
  7db72d1, and Lir cleared node-a's reboot; the standing cluster has four
  nodes, so 3 of 4 stay up. Node-a was then held by Miach until 04:32Z,
  with Dagda next for a seven-hour measurement, so the reboot waits for a
  free node-a; Lir asked to be told before it. Post a note, claim the whole machine (cores "all",
  until about 20 minutes out) and reboot. Checks after (the Lir Development
  Cluster session's): `systemctl is-active lir-node
  claim-board-scheduler`; `journalctl -u claim-board-scheduler -n 3 -o
  cat` says "acting" on node-a, and node-b's log goes back to "standing
  by: node-a acts"; the board's
  `/retained?filter=stack/%23` on node-a (address in `CLAUDE.local.md`)
  returns the same
  as another node's. Node-a logged 110 under-voltage events in 24 h: if it
  is slow to come back, suspect its power supply first. Tell Lir when done.
- **node-b:** `userns-remap: default` and `systemctl restart docker`
  when node-b is unclaimed, with a note; no reboot. Then check `docker info`
  shows `name=userns`.

Previous settings are kept in `/root/louhi-work/` on each Pi
(`cmdline.txt.before`, `daemon.json.before`, `nft-before-docker.txt`).

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

- **Scope:** Louhi is the stack's compute layer (Dannan networking, Dagda
  storage, Lugh auth, Lir events, Louhi compute): ECS and Fargate style
  container compute with the networking and storage substrate to enable it.
- **The cluster is owned by us;** every node is trusted. Trusted hosts and
  contributed machines are for later, and nothing built may close that off.
- **Dannan is not to change for Louhi;** Louhi uses it as it stands.
- Stack dialect, GPLv3, the stack layout; tagline "Louhi - The Distributed
  Compute Layer".
- **Runtime:** the Docker Engine API over its local socket, behind an
  interface. **Node platform:** Linux only.
- **Sandbox:** hardened containers now, the runtime interface shaped for a
  microVM driver later. **Memory limits and user namespaces are enforced:**
  the agent launches nothing on a runtime lacking either unless the
  operator allows it.
- **Placement:** a register per task in Dannan's agreement (0.9.0): any node
  proposes, agreement settles one node, no scheduler. Dannan's `changed_at`
  is the fencing token handed to run_once tasks. The same primitive serves
  address allocation and quota admission across nodes. Nothing that places
  tasks across nodes (step 6) or allocates addresses (step 9) is built
  before 0.9.0; no stand-in meanwhile (the maintainer's ruling on agreement).
- **Tenants:** our own projects, each with enforced quotas from the start.
  Projects and quotas are created through the API by `louhi:admin` only.
- **API:** Louhi's own, declarative, AWS's vocabulary; no Kubernetes or EC2
  compatibility. Sign-in by the Lugh plugin only, no anonymous mode;
  authorisation from the token's audience ("louhi") and scopes, as Lir does.
  Scope actions `read`, `register`, `run`, with `louhi:<project>:*` and
  `louhi:admin`; plain HTTP and HTTPS each the operator's choice.
- **Workload identity:** deferred.
- **MQTT client:** Lir's exported client; Louhi writes none.
- **Overlay carrier:** both: WireGuard (with a VPC tag inside) where two
  nodes reach each other directly, Dannan frames otherwise.
- **Persistent volumes:** what they are made of is decided later (step 14).
- **The build Pis** are also Louhi's test bed: Docker, user namespaces and
  the memory controller on all four.

## Waiting on the maintainer

0. **Hosted CI and Lugh.** Lugh is private, so hosted CI builds Louhi with
   `LOUHI_BUILD_LUGH=OFF` (no plugin, no API, no `louhi-ctl`). To build the
   plugin there, the repository needs a read token for Lugh as a secret, as
   Lir has (`LUGH_READ_TOKEN`); a job would then check Lugh out and name it
   with `LOUHI_LUGH_REPOSITORY`. Recommendation: add it.

1. **Dagda ref.** the maintainer: wait. Dagda 0.2.0 (every-node head spaces,
   `Heads::watch`, `lease::ClaimedCollection`) is tagged locally and held at
   the maintainer's ruling. Dagda's caution: claimed collections stand on leases, which
   order work on one node only today. Step 4's second half waits on this.
2. **Under-voltage:** node-a (110 events in 24 h) and node-d
   report `throttled=0x50000`. Builds pass; the maintainer's hardware, reported only.

## Owed at the first release gate

Build and run the four fuzz targets (`record`, `image_reference`,
`docker_reply`, `api_body`) with clang, `-max_total_time=60` each; the `clang`, `asan`
and `coverage` steps on a Pi. Hosted CI runs them on every push
meanwhile. TSan runs on one Pi only, node-a (a 39-bit VA kernel,
clang 19 and `libclang-rt-19-dev`; build with `CC=clang-19
CXX=clang++-19` and Makefiles, there is no ninja there). g++'s TSan fails
on the other Pis' 47-bit kernels (relayed by Dannan, 2026-11-16). So the
release's TSan run is on node-a with clang, or hosted CI's `tsan` job;
`tools/ci.sh` names Ninja today, so a node-a run needs a generator
setting first.
