# Louhi - Active Work

Last updated: 2026-11-20. `COMPLETED.md` is the record.

## Resume here

**State.** `develop` at `tomdionysus/louhi`, public and pushed, clean.
Done: steps 0 to 3, 5 and 8, and step 13's first half (records and codecs,
the Docker driver, the agent, the in-memory store, `louhi-node` with
`--retire`, the API with sign-in by the Lugh plugin, `louhi-ctl` as
`louhi::ctl`, events on Lir, images by digest, storage bounds). Step 4
under way: Dagda fetched; revision and admission registers (`louhi::admit`)
built and tested on Dannan's simulation harness, not yet called by the API.
Lugh `0.2.0` (repinned 2026-11-20; its commits since `53a3280` read, as it
keeps no changelog) moves into `lugh_client` what Louhi wrote for itself:
`discover()`, `bearer_token()` (16 KiB, Louhi's bound), `RelyingParty`,
`TokenSource` with failover across issuers, and a thread-safe
`testing::FakeHttps`; its HTTPS now goes through Dannan's StreamConnector,
with the same face. Chosen not to change in 0.1.0, whose final gate was
running: Louhi's own `louhi/auth/lugh.*` (RelyingParty),
`louhi/auth/service_token.*` (TokenSource), the API's bearer parsing and
`louhi/test/fixtures/fake_https.h` (FakeHttps) still work on 0.2.0 and are
the same API Louhi had. They are duplicates by the stack's rule, so the
first work after 0.1.0 (feature/lugh-relying-party, a patch release) moves
Louhi onto Lugh's and deletes them, with the tests moved, not dropped.
Dannan `0.10.0` (tagged 2026-11-20, `f70cb52`, SONAME libdannan0.10) waits
until Dagda and Lugh release on it (one Dannan a process). Its CHANGELOG's
"Adopting 0.10.0", against Louhi, for that move:
- `admit::RequestId` (sixteen random bytes) becomes `identity::RandomId<Tag>`;
  look for other hand-made random ids.
- The four fuzz targets register with `dannan::testing` and run under
  `dannan::fuzz::smoke` in `louhi_tests`; Louhi's own drivers and corpus
  replay go.
- `RetryPolicy::unbounded` is already used (agent launches, service token);
  check nothing else loops by hand.
- `model::parse_range` and the security-group and allow-list prefixes go
  through `listen::Prefix::parse`, which now refuses host bits set: check
  the records' tests and the README's examples, and decide whether Louhi
  refuses them too (it should: say so in the API errors).
- The admit simulation tests' teardown (the weak_ptr and owned attempts
  added for the ASan leak) is re-read: the Agreement lifetime faults are
  fixed, so ordering kept only for them goes.
- `agree::MemoryStore` replaces any in-memory acceptor store in tests;
  `status::SwappableSink` and `test::RecordingSink` replace log capture
  written here (fixtures); base64/hex/UTF-8 code of Louhi's own goes to
  `encoding/base.h`; hand-written mkdir-and-fsync to the durable helpers.
- `Completion<T>`/`Outcome<T>` replace Louhi's own callback result types
  where they match (the store, the drivers' completions).
- When the node's config or logging is next touched: the stack's log
  settings, `log.level` (all, debug, info, warn, error;
  `dannan::status::parse_log_level`) and `log.format` (text, json;
  `parse_log_format`), read through `dannan::config`. The maintainer's
  ruling (2026-11-20): YAML is the source of truth, and flags and
  environment variables override any key through `dannan::config`, named
  by one pattern: key `a.b` is `--a-b` and `LOUHI_A_B` (`log.level` is
  `--log-level` and `LOUHI_LOG_LEVEL`). Check every existing flag of
  `louhi-node` and `louhi-ctl` against its YAML key by that pattern.
- CI: asan and tsan steps set `DANNAN_SANITIZE`, and the symbol check
  (`nm -D --undefined-only`) runs in them.
Pins: Dannan `0.9.2`, Dagda `0.3.2`, Lugh `0.2.0`, Lir `e40dd2e` (opt-in)
(2026-11-20: Lugh's `dbb0573` and Lir's `6268fef` rewritten when the
stack's history was scrubbed; the same trees). Events are off by default
(`LOUHI_BUILD_LIR`, coordinator 2026-11-20): Lir 0.1.0 ships no client, and
`liblir-client0.1` is owed with no date; the `events` CI step still builds and
tests them on `e40dd2e`, and they return in the release after Lir's client
does (Lir has Louhi's calls as its first requirements). On the pin to Dannan 0.10.0,
tools/ci.sh's asan and tsan steps also pass `-DDANNAN_SANITIZE=address` or
`thread` (Dannan's docs/building.md), keeping the global flags: every stack
library a sanitizer build links is built from source and instrumented.
Last gate: **318 passed**, none skipped, on node-c (2026-11-20),
Docker on xfs with user namespaces, on `feature/network-records` (the
network records and security groups as nftables), which squashes onto
develop once ASan and TSan have run on it once each (the maintainer, 2026-11-20:
sanitizers on a feature's completion, before the merge).

**the maintainer's working rules for the stack (2026-11-20):** features and milestones
on `feature/<name>` branches, squashed onto develop when done; every build
and test on the cluster; ASan and TSan on a feature's completion before
its merge, or to hunt a bug nothing else found; test once; no mutants
unless the change is small or high risk; no flaky test tolerated; commit
and push at any time; settle cross-project questions with the other
sessions by the stack's goals before asking the maintainer.

**The stack's converged plan** (the maintainer's audits, 2026-11-20) gives Louhi its
list: `../lir-develop-cluster/TODO/2026-11-20-cluster-stack-placement.md`,
"The converged plan", "Louhi". Done of it: RetryState, hex, percent_encode,
louhi-ctl's endpoints (COMPLETED.md). The rest waits on Lugh's client
(relying party, discovery, token source, fake Https), Dannan's next minor
(the node-tool helper, the random id type, the callback vocabulary, the
in-memory AcceptorStore, replica sets that follow membership, readiness)
and the maintainer's HTTP question (`api/client.cpp` moves then). the maintainer's answers
(2026-11-20): HTTP and JSON move into Dannan, and `api/client.cpp` moves
onto Dannan's client when it ships; the register map orders by the hybrid
clock; DNS answered by every node, yes; retiring a node stays an operator
command. Q6 (the maintainer): where two projects need a thing and one depends on the
other, the lower one keeps it, so the relying-party class, discovery, the
token source and the fake HTTPS are lugh_client's; Lugh is building them,
and Louhi moves onto each as it ships (`auth/lugh.*`, `auth/service_token.*`,
the discovery in `ctl/ctl.cpp`, `test/fixtures/fake_https.h`). Dannan's next minor
has not started: the maintainer told Dannan to stop until he has answered the other
agents' issues (2026-11-20). Gateways and ingress
go on any node with public reach, settled by a register, not on nodes the
operator marks (settled by the goals; written into the design's
Networking section).

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
- Sanitizers (ASan, TSan) run once each when a feature or milestone is
  complete, before it is squashed onto develop, or to hunt a bug nothing
  else found (the maintainer, 2026-11-20, replacing the rule of 2026-11-17).
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
   the maintainer's rulings (2026-11-20), to be written into the design first:
   - **A task record merges** with "wanted stopped" absorbing (no merge
     undoes a stop) and every other field group last-writer-wins on
     Dannan's hybrid clock (`Context::hlc()`).
   - **Revision numbers** come from Dannan's agreement: a register per
     definition family holding the next revision, so N is unique. The
     revision's content then goes to Dagda under N.
   - **Quota admission** comes from agreement: a register per project
     holding what is admitted (tasks, CPU, memory); RunTask commits "add
     if within quota", a stop commits the subtraction.
   - **The acceptor state** for both lives in Dagda's durable
     `AcceptorStore`, asked of the Dagda session on the maintainer's word for its
     release after 0.3.2 (for Lir, Lugh, Kvasir and Louhi alike; rule 3).
     Louhi writes none. Meanwhile Louhi builds the registers' callers and
     tests them on Dannan's simulation harness with `test::sim::Store`
     (`dannan/test/fixtures/sim/agreement.h:33`, linked through
     `dannan::sim_checkers`). Registering and RunTask wait on the store
     for real use; the order inside step 4 is the next session's to plan.
   - **Domain and build:** a domain "louhi", its key derived from the
     cluster key by HKDF under "louhi/storage/v1", spaces under "louhi/";
     Dagda fetched from `../dagda` like the others (Dannan declared with
     `OVERRIDE_FIND_PACKAGE`, Dagda's install skipped). A failed fetch
     goes to the Dagda session and to the maintainer.

   Dagda's next release (the maintainer's ruling in the Dagda session, 2026-11-20:
   no longer held by Macha's cutover) is to bring, test first: Context v2
   read-one and prefix listing with paging; the caught-up signal, a node's
   view covering the clocks its active peers advertise for the space;
   FetchContent beside a fetched Dannan; and the AcceptorStore. Louhi
   asked for: an absent key read as empty; this node's view by default,
   a choice per call; pages of a few hundred with a resumable cursor, each
   listing from one view; caught-up per space, as a query and a watch
   notice. Louhi may start on 0.3.2 with the layer below and move when
   it lands.

   Neither Lir nor Lugh is on Dagda yet: Louhi is the first hosted system
   on its head spaces after Macha.

   **The space's owner (the maintainer, 2026-11-20, asked this session):** "Dagda
   only does this if other projects that use Dagda can use this feature."
   The owner is Dagda's built-in register map (ruled 2026-11-13 in Dagda's
   three-systems design, its slice 3, unbuilt; Lir's retained, Lugh's
   JWKS, the catalogue and the leases use it), which the Dagda session
   recommends for its next release. Louhi writes no owner. The two
   additions Louhi would like (the owner's ordering stamp, a per-record
   merge hook) go in only if another project needs them; if not, a task's
   stop is a key of its own, only written as stopped, and the record
   carries its hybrid-clock stamp inside. The Dagda session is asking Lir
   and Lugh. The records therefore wait for that release.

   the maintainer's rulings in the Dagda session (2026-11-20), for that release: the
   register map (it serves Louhi, Lir, Lugh, the catalogue and the
   leases); the `AcceptorStore` as a node-local log per agreement space
   with group commit; caught-up per space as a query and a notice, from a
   survey of the peers holding the space (not an advert), three-valued:
   caught up, behind (naming peers and heads), or alone (no peer to
   compare). Whether alone counts as whole for removals goes to the maintainer when
   the store is built (a node that has never heard a peer may be cut off,
   or may be the cluster's only node).

   Done (2026-11-20, COMPLETED.md): Dagda fetched at 0.3.2; the revision
   and admission registers (`admit/registers.h`, `admit/agreement.h`),
   tested on Dannan's simulation harness.
   Left in step 4, in order:
   - the API onto the registers: RegisterTaskDefinition takes its
     revision from `take_revision`, RunTask admits through `admit_tasks`,
     StopTask releases through `release_task`. In real use they wait on
     Dagda's `AcceptorStore`, and on **which replica set serves Louhi's
     slots** (`Serving`), which goes to the maintainer with a recommendation when
     the store is there: the tests use the simulation's genesis set;
   - the Dagda store on the register map, when Dagda releases it. Dagda's
     D6 design (`../dagda/TODO/2026-11-20-register-map.md`), with Louhi's
     review taken (2026-11-20): `changed(view_before, view_after, prefix)`
     for the store's watches; a commit's outcome names each key's new
     (stamp, node id); **64 KiB per value**, refused at put, anything larger
     a Dagda object named in the register's refs (Dagda keeps and frees
     it); tombstones per space, a 30-day window by default, past which a
     node long away can revive a removed key, or "never". For Louhi: bound
     every record under 64 KiB but a task definition, which goes to an
     object when over it; open the space with "never" tombstones, as task
     and project removals must not come back.
1. **Networking groundwork** (the maintainer, 2026-11-16). Done: the network records
   and security groups as nftables (COMPLETED.md). Left: a task's network
   namespace on one node, with the `nft` driver behind `net::Firewall`.
   On a Pi that creates namespaces and loads nftables rules, so it needs
   the maintainer's go-ahead, a note on the board, and everything removed after
   (`CLAUDE.local.md`). Nothing that allocates addresses or places across
   nodes.
2. **Steps 6 and 7, and the rest of step 9**, on Dannan's agreement
   registers (`Context::agreement()`, in the pinned Dannan): placement,
   then services, then address allocation, on the same Dagda
   `AcceptorStore` as revisions and quota; Louhi writes none of its own
   (the maintainer, 2026-11-20), as Lir does not.
3. **Owed, end to end**, when a Lugh issuer is at hand:
   - `louhi-ctl login` and `ServiceToken` against a real Lugh;
   - task events (not only a node's status) published live to Lir, which
     needs the API, and so an issuer;
   - a private registry signed in to for real (`--registry-credential`).
4. **The image store on Dagda** (step 13's second half), after item 0.

The order of 1 and 2 is this session's reading; the maintainer may put networking
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

0. **the maintainer's two stack audits (2026-11-20)**, no duplicated functionality and
   no special node, are reported to the coordinating session,
   which correlates all eight projects
   (`../lir-develop-cluster/TODO/2026-11-20-cluster-stack-placement.md`).
   Louhi's findings: the Lugh relying-party plugin copied from Lir's, the
   HTTP server and client cores, the node-tool scaffolding, OIDC discovery
   and the service-token source (to Dannan or Lugh's client); and its own
   fixes, using Dannan's `RetryState` and hex helpers. Special roles: the
   first Lugh issuer for service tokens, one endpoint in louhi-ctl, and the
   designed gateway and ingress nodes "the operator marks". Nothing moves
   until the maintainer decides.

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
  allocation, quota admission and (2026-11-20) revision numbers. Nothing
  that places tasks across nodes, allocates addresses or keeps a
  service's count is built without it; no stand-in.
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
- **Dannan pin (2026-11-16):** moved once from `0.8.3` to `0.9.0` and its patches,
  with Lugh and Lir (done 2026-11-20, at 0.9.2).
- **Retiring a node (2026-11-17):** `louhi-node --retire`, the same
  command as `lir-node`'s.
- **Dagda (2026-11-20):** records now, on Dagda `0.3.2`'s head spaces,
  pinned at the tag though contract v2 freezes only at slice 4, in a
  domain "louhi" keyed from the cluster key; Dagda fetched like the
  other siblings. A task record merges with a stop absorbing, else
  last-writer-wins on the hybrid clock.
- **Agreement (2026-11-20):** revision numbers, quota admission and
  placement are Dannan agreement registers, their acceptor state in
  Dagda's durable `AcceptorStore` (asked for, one for every hosted
  system). No acceptor store of Louhi's own.
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
