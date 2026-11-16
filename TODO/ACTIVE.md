# Louhi - Active Work

Last updated: 2026-11-16 (end of session). `COMPLETED.md` is the record.

## Resume here

**State.** `develop` at `tomdionysus/louhi`, public and pushed, clean.
Done: steps 0 to 3, 5 and 8, and step 13's first half (records and codecs,
the Docker driver, the agent, the in-memory store, `louhi-node`, the API
with sign-in by the Lugh plugin and `louhi-ctl`, events on Lir, images by
digest, storage bounds). Last gate: **238 passed** on node-c, whose
Docker is on xfs with project quotas, so storage bounds are kept for real.
Pins: Dannan `0.8.3`, Lugh `382416d`, Lir `4443c16`.

**Read first:** the design, [`2026-11-15-louhi-design.md`](2026-11-15-louhi-design.md);
then this file's Decisions. the maintainer's standing instruction: work autonomously
until he is needed, commit and push (pushes allowed, the maintainer 2026-11-16);
decisions go to him as interactive questions with a recommendation. One
step per session.

**How work is done.**

- Nothing is built or run on the Mac (TSan there only to hunt a specific
  bug). Builds and tests run on the shared Pi 5s, claimed on the board
  first (`CLAUDE.local.md` has hosts and rules).
- `build/pi.sh <address> [step] [gtest filter]` (untracked) syncs the tree
  and Dannan's, Lugh's and Lir's histories to a Pi and runs a `tools/ci.sh`
  step there at `-j2` under `nice 19`. To iterate on compile errors, rsync
  and run `ninja -k 0 -j2` in the Pi's `build-ci/release`.
- Gate on **node-c** where it can be had: it is the only Pi whose
  storage tests take their kept branch. node-b works, with those branches
  as refusals. Not node-d (down, and too small for sanitizers). Not
  node-a (no ninja, no Docker).
- The claim board's hook counts any package install or `mkfs` over ssh as
  heavy: claim at least one core first. One board wait per session; a
  second `claim-board wait` replaces the first.
- Lookups: the basemind CLI (`basemind -q code grep|outline|symbols`);
  `grep`, `rg`, `awk`, `sed` and the like are blocked by a hook. Logs under
  `build/` are not indexed: read them with Read, by line range.

## Next, in order

1. **Tests of their own for `louhi-ctl` and `ServiceToken`**
   (`tools/louhi_ctl.cpp`, `louhi/auth/service_token.h`), against fakes. the maintainer
   chose this as the next step (2026-11-16). Neither has a test today.
2. **The Dannan pin stays at `0.8.3` until Dannan 0.9.0** (the maintainer,
   2026-11-16): no move to 0.8.5 meanwhile; the pin moves once, to 0.9.0,
   as its own commit with one gate. What 0.8.4 and 0.8.5 bring rides along
   then: 0.8.4 brought StartTLS with no answer, a stop token on
   `NodeEvents::wait_for_change`, and a fix to deferred answers' overload
   accounting. 0.8.5 fixes Louhi's finding: a second `Client::stop()` or
   `Server::stop()` no longer posts to a stopped io, so a Node stopped
   while its io ran may be destroyed after it. Node's contract: stop it
   while its io runs. Louhi's order (destroy the Node first) stays right.
   Lugh's develop (`2d75176`) pins Dannan 0.8.5 too, its back-off now
   Dannan's: a Lugh pin past `382416d` needs Dannan 0.8.5 or a later 0.8
   patch, so the Lugh pin stays at `382416d` until Dannan moves too.
   `lugh_client`'s face is unchanged.
3. **Owed, end to end**, when a Lugh issuer is at hand:
   - `louhi-ctl login` and `ServiceToken` against a real Lugh;
   - task events (not only a node's status) published live to Lir, which
     needs the API, and so an issuer;
   - a private registry signed in to for real (`--registry-credential`).

   For Lir: the standing cluster has `louhi/#` open to anonymous clients,
   and it stays open (the maintainer, via the Lir session). Join at `192.0.2.10:7537`
   (port 7537, not 7437), plain TCP, MQTT on 1983. Use the key from
   `../lir/build/pi-cluster/cluster.key`, copied to the node 0600 and
   deleted after. Tell the Lir session before and after, stop the node
   cleanly, and space joins out: a departed member counts until Dannan's
   horizon, and with node-d down the cluster keeps a majority only while
   all three Lir nodes are up.
4. **The image store on Dagda** (step 13's second half) and **the Dagda
   store** (step 4's second half): both wait on the Dagda ref.
5. **Steps 6, 7 and 9** wait on Dannan 0.9.0's agreement registers. A
   service's task set changes through registers, so services wait with
   placement (the maintainer: anything that can use agreement waits for it).

## Waiting on the maintainer

1. **Dagda ref.** the maintainer: wait. Dagda 0.2.0 is tagged locally and held. Its
   caution: claimed collections stand on leases, which order work on one
   node only today.
2. **node-d** went down three times on 2026-11-16 (before 00:34Z,
   about 01:06Z, about 01:21Z), and needs a power-cycle. It and
   node-a report under-voltage (`throttled=0x50000`). Louhi builds
   nothing on node-d until the maintainer says it is sound.

## The build Pis

| Host | Docker | User namespaces | Memory controller | Storage bounds | Notes |
|---|---|---|---|---|---|
| node-b | yes | **no** (to do) | on | no (ext4) | lir-node, scheduler; Kvasir's MySQL container |
| node-c | yes | yes | on | **yes** (xfs) | lir-node, scheduler, redis (not in Docker) |
| node-d | yes | yes | on | no (ext4) | lir-node; down |
| node-a | **no** (to do) | **no** | **off** (to do) | no | lir-node, the acting scheduler; clang 19, no ninja |

All four run the 4K-page, 48-bit-VA kernel (`6.18.62-v8-va48+`) since
2026-11-16, so ASan, UBSan and TSan run on each, with g++ and clang 19
(node-d has no clang). There is no special sanitizer Pi.

node-c's Docker data is in `/var/lib/docker-xfs.img` (16 GB),
loop-mounted at `/var/lib/docker` with `pquota` from `/etc/fstab` (the maintainer's
go, 2026-11-16). The undo is `/root/louhi-work/UNDO-xfs.txt`, beside
`fstab.before-xfs`; the old data is in `/var/lib/docker.before-xfs`. Every
other Pi needs `--allow-unbounded-storage` to launch a task.

Owed, at the maintainer's go already given. Save the previous settings to
`/root/louhi-work/` on each Pi first (`cmdline.txt.before`,
`daemon.json.before`, `nft-before-docker.txt`):

- **node-a:** install `docker.io` and `docker-cli` (save the nft
  ruleset first), `userns-remap: default` in `/etc/docker/daemon.json`,
  `cgroup_enable=memory` in `/boot/firmware/cmdline.txt` (change nothing
  else), one reboot. Lir cleared it on 2026-11-15 and asked to be told
  before and after. It needs the whole machine: post a note, claim cores
  "all" for about 20 minutes, queue for it if held. Checks after:
  `systemctl is-active lir-node claim-board-scheduler`; `journalctl -u
  claim-board-scheduler -n 3 -o cat` says "acting" on node-a, and node-b's
  goes back to "standing by: node-a acts"; the board's
  `/retained?filter=stack/%23` on node-a answers as another node's does.
  If it is slow to come back, suspect its power supply first.
- **node-b:** `userns-remap: default` and `systemctl restart docker`
  while node-b is unclaimed, with a note; no reboot. Kvasir's MySQL
  container runs there, so ask Kvasir first. Check `docker info` shows
  `name=userns`.

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
  `private_key_jwt` or `client_secret_basic`, the operator's choice, or
  anonymously where Lir allows it. Live tests use the standing cluster,
  where `louhi/#` stays open (2026-11-16).
- **Images (2026-11-16):** a tag is resolved to a digest at registration
  through the runtime (the Docker driver's `GET /distribution`), with no
  registry client of Louhi's own. Registries are an operator allow-list
  (no list allows none), with the operator's credentials per registry in
  `secrets.yaml`; per-project credentials wait for a secret store.
- **Ephemeral storage (2026-11-16):** a task's writable layer is bounded
  by its `ephemeral_storage_mib`, shared equally, through xfs project
  quotas. Scratch volumes come in two kinds: memory (tmpfs, counted in the
  task's memory) and disk (quota-bounded).
- **Dannan pin (2026-11-16):** stays at `0.8.3` until 0.9.0, then moves
  once; the Lugh pin waits with it, since a later Lugh needs Dannan 0.8.5.
- **Workload identity:** deferred.
- **Overlay carrier:** both: WireGuard (with a VPC tag inside) where two
  nodes reach each other directly, Dannan frames otherwise.
- **Persistent volumes:** what they are made of is decided later (step 14).
- **Hosted CI (2026-11-16):** stays without the Lugh plugin (Lugh is
  private; no read token). The Pis cover the plugin.
- **The build Pis** are also Louhi's test bed: Docker, user namespaces and
  the memory controller on all four; Docker on xfs on node-c (2026-11-16).

## Owed at the first release gate

- The four fuzz targets (`record`, `image_reference`, `docker_reply`,
  `api_body`) built with clang and run `-max_total_time=60` each.
- The `clang`, `asan`, `tsan` and `coverage` steps on a Pi. Any of the four
  runs sanitizers now. Prefer node-b or node-c (15 GB): node-d has 4 GB
  and ran out of memory once. Check `free -m` first.

Hosted CI runs all of these but the fuzzers on every push meanwhile.
