# Louhi - Active Work

Last updated: 2026-11-20. `COMPLETED.md` is the record.

## Resume here

**State.** `develop` at `tomdionysus/louhi`, public and pushed, clean.
Done: steps 0 to 3, 5 and 8, and step 13's first half (records and codecs,
the Docker driver, the agent, the in-memory store, `louhi-node` with
`--retire`, the API with sign-in by the Lugh plugin, `louhi-ctl` as
`louhi::ctl`, events on Lir, images by digest, storage bounds).
Pins: Dannan `0.9.2`, Lugh `dbb0573`, Lir `6268fef` (2026-11-20).
Last gate: **277 passed**, none skipped, on node-c (2026-11-20),
Docker on xfs with user namespaces.

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

0. **The Dagda store** (step 4's second half; the maintainer, 2026-11-20: records
   now, placement later). Louhi's records in an every-node head space of
   Dagda `0.3.2`, pinned at the tag, behind `store/store.h`. What 0.3.2
   gives and lacks (slice 1 only; slices 2 to 5 paused until Macha's
   cutover), read 2026-11-20:
   - head spaces with the owner's codec and merge, `watch` and `changed`
     on `dagda::Context` (`dagda/context.h:340-421`, `heads/space.h`);
   - no read-one or list-by-prefix on the Context: one layer down,
     `Node::spaces()->find(id)` and `Space::entries` (`space.h:478-495`);
     asked of the Dagda session for the Context;
   - a commit is on this node, peers owed it, with no durability level
     (slice 2): what the API's writes report must say so;
   - no caught-up signal for a new node (the metadata plane is ready once
     spaces open locally): the store stays `Whole::never` for the agent
     until there is one, so nothing unknown is removed (discipline 3);
     asked of the Dagda session;
   - one domain per Dagda node, its key derived by the hosted system
     (`StorageKey(Secret)`, no helper for other domains);
   - `dagda::test_fixtures` (`Cluster`, `ClusterNode`, stepped time; real
     loopback ports, so not fully deterministic);
   - consumed as an installed package with exact-version compatibility
     (`find_package(dagda 0.3.2)`, the tested route).

   The Dagda session's answers (2026-11-20). It works on nothing but
   Macha's cutover, so read-one, prefix and caught-up on the Context go to
   the maintainer, and it will say when he rules.
   - **Fetching Dagda:** untested. Dagda's CMake does
     `find_package(dannan 0.9.2 CONFIG REQUIRED)` and ignores an existing
     `dannan::dannan` target, so declare Dannan with
     `FetchContent_Declare(dannan ... OVERRIDE_FIND_PACKAGE)` before Dagda
     is made available. Its `install(EXPORT)` names `dannan::dannan`, which
     may fail when Dannan is a subproject: skip Dagda's install. If the
     fetch fails, tell the Dagda session the error; making Dagda
     fetch-friendly is a change to Dagda, so the maintainer's.
   - **Reads meanwhile:** `Node::spaces()->find(id)`, then `Space::local()`
     or `converged()` for a view, and `Space::entries(tree, view, cursor,
     budget)` for pages in key order; a prefix is a cursor at the prefix,
     stopping at the first key past it.
   - **Caught up:** none. A view can lag other nodes' commits, and on a
     node with several heads its own too. `Space::covers(view, dot)`, the
     dot from `CommitOutcome::dot`, says whether a view holds what this
     node wrote. The signal Dagda would propose compares the view's clock
     with the clocks peers advertise. Until then, never whole for removals.
   - Only `every_node` and `node_local` open; `replicas: N` is not built.
   Design questions this raises (the merge per record kind, what a
   conflicting write of the same task means, the domain and key) go to
   the maintainer first, written into the design. Neither Lir nor Lugh is on Dagda
   yet: Louhi is the first hosted system on its head spaces after Macha.
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
   registers (`Context::agreement()`, in the pinned Dannan): placement,
   then services, then address allocation. They wait for Dagda's acceptor
   store (its slice 4, paused until Macha's cutover); Louhi writes no
   `AcceptorStore` of its own (the maintainer, 2026-11-20), as Lir does not.
4. **Owed, end to end**, when a Lugh issuer is at hand:
   - `louhi-ctl login` and `ServiceToken` against a real Lugh;
   - task events (not only a node's status) published live to Lir, which
     needs the API, and so an issuer;
   - a private registry signed in to for real (`--registry-credential`).
5. **The image store on Dagda** (step 13's second half), after item 0.

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

1. **node-d:** Louhi builds nothing on it until the maintainer says it is
   sound. It reported under-voltage again on 2026-11-19
   (`throttled=0x50005`) and has 4 GB.
2. **Hosted CI runs ASan, UBSan and TSan on every push**
   (`.github/workflows/ci.yml`). It uses no Pi time, so it was left; the maintainer
   may want it out under the sanitizer rule.

## The build Pis

All four were wiped to a bare OS on 2026-11-18 and have Docker again
since 2026-11-20 (the maintainer's word). All: Debian 13, kernel `6.18.62-v8-va48+`
(4K pages, 48-bit VA), g++, clang++, cmake, ninja, the build's development
packages, Docker 26.1.5 with `userns-remap: default`. Each runs lir-node,
lugh, mimir and the board's scheduler (and macha on node-c and node-d):
not Louhi's to touch.

| Host | Memory | Memory controller | Storage bounds | Throttled | Notes |
|---|---|---|---|---|---|
| node-c | 15 GB | on | **yes** (xfs) | no | the gate's Pi |
| node-b | 15 GB | on | no (ext4) | no | second choice |
| node-d | 4 GB | on | no (ext4) | `0x50000` | not used for builds (Waiting on the maintainer, 1) |
| node-a | 15 GB | **off** | no (ext4) | `0x50000` | hosts the board; last choice |

node-c's Docker data is in `/var/lib/docker-xfs.img` (16 GB), loop-mounted
at `/var/lib/docker` with `pquota` from `/etc/fstab`; the undo is
`/root/louhi-work/UNDO-xfs.txt`. Every other Pi needs
`--allow-unbounded-storage` to launch a task. What each install replaced is
in `/root/louhi-work/` on each Pi; `build/docker-install.sh [xfs]` is the
script. node-a's memory controller needs `cgroup_enable=memory` and a
reboot of the board's host: only if the maintainer wants node-a as a test node.

What each Pi's settings were before Louhi first changed them, before the
wipe, are in `build/pi-records/<host>/` (untracked, on the Mac).

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
- **Dagda (2026-11-20):** records now, on Dagda `0.3.2`'s head spaces,
  pinned at the tag though contract v2 freezes only at slice 4;
  placement later, on Dagda's acceptor store. No acceptor store of
  Louhi's own.
- **Workload identity:** deferred.
- **Overlay carrier:** both: WireGuard (with a VPC tag inside) where two
  nodes reach each other directly, Dannan frames otherwise.
- **Persistent volumes:** what they are made of is decided later (step 14).
- **Hosted CI (2026-11-16):** stays without the Lugh plugin (Lugh is
  private; no read token). The Pis cover the plugin.
- **The build Pis** are also Louhi's test bed: Docker with user
  namespaces on all four, xfs storage bounds on node-c (2026-11-20, after
  the wipe of 2026-11-18). Anything more is asked for each time.

## Owed at the first release gate

- The four fuzz targets (`record`, `image_reference`, `docker_reply`,
  `api_body`) built with clang and run `-max_total_time=60` each.
- The `clang`, `asan`, `tsan` and `coverage` steps on a Pi: node-b or
  node-c (15 GB). Check `free -m` first.
