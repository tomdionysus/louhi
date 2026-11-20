# Louhi - Active Work

Last updated: 2026-11-20. `COMPLETED.md` is the record.

## Resume here

**State.** `develop` at `tomdionysus/louhi`, public and pushed, clean.
Done: steps 0 to 3, 5 and 8, and step 13's first half (records and codecs,
the Docker driver, the agent, the in-memory store, `louhi-node` with
`--retire`, the API with sign-in by the Lugh plugin, `louhi-ctl` as
`louhi::ctl`, events on Lir, images by digest, storage bounds).
Pins: Dannan `0.9.2`, Lugh `dbb0573`, Lir `6268fef` (2026-11-20).
Last gate: **264 passed, 13 skipped** on node-c (2026-11-20): the
skipped are every real-Docker test, since the Pis were wiped to a bare OS
on 2026-11-18 and none has Docker. Last gate with Docker: **277 passed**
(2026-11-16, Dannan 0.8.3). The Docker tests are owed on 0.9.2 when Docker
is back ("Waiting on the maintainer", 1).

**Read first:** the design, [`2026-11-15-louhi-design.md`](2026-11-15-louhi-design.md);
then this file's Decisions. the maintainer's standing instruction: work autonomously
until he is needed, commit and push (pushes allowed, the maintainer 2026-11-16);
decisions go to him as interactive questions with a recommendation. One
step per session.

**How work is done.**

- Nothing is built or run on the Mac, by this session, a subagent or a
  worktree (the maintainer, repeated 2026-11-17: a build there is killed). Builds and
  tests run on the shared Pi 5s, claimed on the board first
  (`CLAUDE.local.md` has hosts and rules).
- `build/pi.sh <address> [step] [gtest filter]` (untracked) syncs the tree
  and Dannan's, Lugh's and Lir's histories to a Pi and runs a `tools/ci.sh`
  step there at `-j2` under `nice 19`. To iterate on compile errors, rsync
  and run `ninja -k 0 -j2` in the Pi's `build-ci/release`.
- Which Pi: node-b or node-c. Not node-d until the maintainer says it is sound
  (under-voltage, 4 GB). node-a builds now (it has ninja) but hosts the
  board and reports under-voltage: last choice.
- Sanitizers run only for a release or to hunt a named bug, never as a
  step's gate (the maintainer; repeated to every session 2026-11-17).
- The claim board's hook counts any package install or `mkfs` over ssh as
  heavy: claim at least one core first. One board wait per session; a
  second `claim-board wait` replaces the first. When another session's
  work has priority on the cluster, start nothing until the board says it
  is clear.
- Lookups: the basemind CLI (`basemind -q code grep|outline|symbols`, with
  `--path-contains`); `grep`, `rg`, `awk`, `sed`, `head`, `tail` and `cat`
  over local files are blocked by a hook. Files under `build/` are not
  indexed: read them with Read, by line range. A sibling's file at a pinned
  tag: `git -C ../dannan show 0.8.3:path > build/name`, then Read.

## Next, in order

1. **`louhi-ctl login --scope openid` fails** (found 2026-11-16): the
   issuer then returns an ID token, which Lugh's client verifies against a
   `Verifier` holding no keys (`ctl/ctl.cpp`, `login`). Either load the
   issuer's JWKS first or refuse `openid` as a usage error; louhi-ctl needs
   no ID token. Small; a test in `ctl_test.cpp` first.
2. **Networking groundwork** (the maintainer, 2026-11-16), the parts of steps 9 and
   10 that need no agreement: Subnet, RouteTable and SecurityGroup records
   and codecs; a security group rendered to nftables rules as a pure
   function, behind an interface, tested against fakes; a task's network
   namespace on one node. Nothing that allocates addresses or places across
   nodes; anything near that line goes to the maintainer first.
3. **Steps 6 and 7, and the rest of step 9**, on Dannan's agreement
   registers (`Context::agreement()`, now in the pinned Dannan): placement,
   then services (a service's task set changes through registers), then
   address allocation. Read `dannan/agree/` and the 0.9.0 changelog first;
   design questions the real agreement API raises go to the maintainer first (the
   acceptor state lives behind a hosted system's `AcceptorStore`, which
   for Louhi wants Dagda or a local durable store: ask).
4. **Owed, end to end**, when a Lugh issuer is at hand:
   - `louhi-ctl login` and `ServiceToken` against a real Lugh;
   - task events (not only a node's status) published live to Lir, which
     needs the API, and so an issuer;
   - a private registry signed in to for real (`--registry-credential`).
5. **The image store on Dagda** (step 13's second half) and **the Dagda
   store** (step 4's second half): both wait on the Dagda ref.

The order of 1 to 3 is this session's reading; the maintainer may put networking
or placement first.

### Joining the standing Lir cluster

`louhi/#` is open to anonymous clients there and stays open (the maintainer, via the
Lir session). **Ask the maintainer before joining again.** Dannan never drops an
absent member; only an operator's retire does, so every join stays in the
agreed roster for good. The node-c run of 2026-11-16 (node id
`8e23d6f7673ee6cfc093b5061491b131`) was still counted as a fifth,
unreachable member on 2026-11-17, and the cluster read majority 4 of 5;
retiring it there is the maintainer's call and Lir's command (`lir-node --retire`).
Ask the Lir session whether it is done before relying on it.

When the maintainer allows a join: the address, ports and key location are in
`CLAUDE.local.md` (never here: this repository is public); confirm them
with the Lir session, since the cluster has been redeployed since. Copy
the key to the node 0600 and delete it after. Tell the Lir session before and after, stop the
node cleanly, then retire its id on every member (`louhi-node --retire`
retires only from a Louhi node's own state).

## Waiting on the maintainer

1. **Docker on the Pis again.** The wipe removed it everywhere. The 13
   real-Docker tests (the driver's, the agent's real task, the storage
   bound) skip until it is back, and have not run on Dannan 0.9.2.
   node-c keeps a stale `/var/run/docker.sock` from before the wipe;
   `build/pi.sh` pings the daemon, so it is harmless, but it goes when
   Docker is reinstalled. Recommended: Docker with `userns-remap:
   default` on node-b and node-c, and node-c's Docker data on xfs with
   `pquota` again. Each is a change to a shared machine: it needs the maintainer's
   go, a note on the board, and the previous settings saved first.
2. **Dagda ref.** the maintainer: wait. Dagda has since tagged 0.3.1, with 0.3.2 at
   its release gate (2026-11-19). The caution given at 0.2.0: claimed
   collections stand on leases, which order work on one node only.
3. **node-d:** Louhi builds nothing on it until the maintainer says it is
   sound. It reported under-voltage again on 2026-11-19
   (`throttled=0x50005`) and has 4 GB.
4. **Hosted CI runs ASan, UBSan and TSan on every push**
   (`.github/workflows/ci.yml`). It uses no Pi time, so it was left; the maintainer
   may want it out under the sanitizer rule.

## The build Pis

Seen 2026-11-19, after the wipe. All four: Debian 13, kernel
`6.18.62-v8-va48+` (4K pages, 48-bit VA), g++, clang++, cmake, ninja, and
the four development packages the build needs (Boost, OpenSSL, yaml-cpp,
GoogleTest). None has Docker. None has `/root/louhi-work`; `build/pi.sh`
makes it again.

| Host | Memory | Memory controller | Throttled | Notes |
|---|---|---|---|---|
| node-b | 15 GB | on | no | first choice with node-c |
| node-c | 15 GB | on | no | NVMe; was the xfs Docker host |
| node-d | 4 GB | on | `0x50005` | not used by Louhi (Waiting on the maintainer, 3) |
| node-a | 15 GB | **off** | `0x50000` | hosts the claim board; last choice |

`lir-node` and `claim-board-scheduler` were active on node-a and node-c
on 2026-11-19: the standing cluster survived the wipe or was redeployed.
It is not Louhi's to touch.

When Docker comes back (Waiting on the maintainer, 1), per Pi:

- Save first, to `/root/louhi-work/`: `nft list ruleset`,
  `/etc/docker/daemon.json` if any, `/boot/firmware/cmdline.txt`,
  `/etc/fstab`.
- Install `docker.io` and `docker-cli`; `"userns-remap": "default"` in
  `/etc/docker/daemon.json`; `systemctl restart docker`; `docker info`
  shows `name=userns`.
- node-a only, if it is ever a test node: `cgroup_enable=memory` in
  `/boot/firmware/cmdline.txt` and one reboot. It hosts the board, so it
  needs the whole machine claimed, a note, and the Lir session told before
  and after; after, `systemctl is-active lir-node claim-board-scheduler`.
- Storage bounds (node-c): a 16 GB file formatted xfs, loop-mounted at
  `/var/lib/docker` with `pquota` from `/etc/fstab`, Docker stopped while
  it is moved. Without it a node needs `--allow-unbounded-storage`.

What each Pi's settings were before Louhi first changed them, and
node-c's xfs undo note, were copied off before the wipe to
`build/pi-records/<host>/` (untracked, on the Mac).

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

All the maintainer's, 2026-11-15 unless dated. Superseded ones are folded in; the
history is in git.

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
  microVM driver later. Memory limits, user namespaces and (2026-11-16)
  storage bounds are enforced: the agent launches nothing on a runtime
  lacking any of them unless the operator allows it.
- **Placement:** a register per task in Dannan's agreement: any node
  proposes, agreement settles one node, no scheduler. `changed_at` is the
  fencing token for run_once tasks. The same primitive serves address
  allocation and quota admission across nodes. Nothing that places tasks
  across nodes, allocates addresses or keeps a service's count is built
  without it; no stand-in.
- **Tenants:** our own projects, each with enforced quotas from the start,
  created and set through the API by `louhi:admin` only.
- **API:** Louhi's own, declarative, AWS's vocabulary; no Kubernetes or EC2
  compatibility. Sign-in by the Lugh plugin only, no anonymous mode;
  authorisation from the token's audience ("louhi") and scopes
  (`louhi:<project>:read|register|run|*`, `louhi:admin`); plain HTTP and
  HTTPS each the operator's choice.
- **Events:** on Lir, through Lir's exported `lir::client`; Louhi writes no
  MQTT client. The node signs in to Lir as a Lugh service principal by
  `private_key_jwt` or `client_secret_basic`, the operator's choice, or
  anonymously where Lir allows it. Live tests use the standing cluster,
  where `louhi/#` stays open (2026-11-16), with the maintainer's word for each join
  (2026-11-17).
- **Images (2026-11-16):** a tag is resolved to a digest at registration
  through the runtime (the Docker driver's `GET /distribution`), with no
  registry client of Louhi's own. Registries are an operator allow-list
  (no list allows none), with the operator's credentials per registry in
  `secrets.yaml`; per-project credentials wait for a secret store.
- **Ephemeral storage (2026-11-16):** a task's writable layer is bounded
  by its `ephemeral_storage_mib`, shared equally, through xfs project
  quotas. Scratch volumes come in two kinds: memory (tmpfs, counted in the
  task's memory) and disk (quota-bounded).
- **Dannan pin (2026-11-16):** moved once from `0.8.3` to the 0.9 line,
  with Lugh and Lir (done 2026-11-20, at 0.9.2).
- **Retiring a node (2026-11-17):** `louhi-node --retire`, the same
  command as `lir-node`'s.
- **Workload identity:** deferred.
- **Overlay carrier:** both: WireGuard (with a VPC tag inside) where two
  nodes reach each other directly, Dannan frames otherwise.
- **Persistent volumes:** what they are made of is decided later (step 14).
- **Hosted CI (2026-11-16):** stays without the Lugh plugin (Lugh is
  private; no read token). The Pis cover the plugin.
- **The build Pis** are also Louhi's test bed; what is installed on them
  for that is asked for each time (they were wiped on 2026-11-18).

## Owed at the first release gate

- The four fuzz targets (`record`, `image_reference`, `docker_reply`,
  `api_body`) built with clang and run `-max_total_time=60` each.
- The `clang`, `asan`, `tsan` and `coverage` steps on a Pi: node-b or
  node-c (15 GB). Check `free -m` first.
- A full gate with real Docker, user namespaces and xfs storage bounds,
  none skipped.
