# Louhi - Active Work

Last updated: 2026-11-22. `COMPLETED.md` is the record; git has the history
of this file.

## Resume here

**State.** **0.1.1 is released** (2026-11-22): tag `0.1.1` on `main` at
`ddd285d`; `develop` is `main` merged back, plus the notes since. No
feature branch is open (COMPLETED.md, "0.1.1 released"). Every item below
waits on another project's release, or (item 4) on the maintainer's
go-ahead.

- **Pins:** Dannan `0.9.2`, Dagda `0.3.2`, Lugh `0.2.0`; Lir `e40dd2e`
  only when `LOUHI_BUILD_LIR` is on (off by default: no Lir release ships a
  client).
- **Tests:** 386 with Lir off (385 run on node-b, 1 skipped there); 389
  with it on.

**The deb on node-c.** Dannan 0.9.2's and Dagda 0.3.2's packages and
lintian are installed there (the maintainer's go-ahead, 2026-11-20, kept
for every project). The mirrors are `/root/louhi-pkg/*.git`; refresh
`louhi.git` and `lugh.git` with rsync first, then `chown -R root:root`
them (rsync keeps the Mac's owner, and git refuses a clone from a
repository another user owns). Then, in `/root/louhi-pkg`:
clone `louhi.git`, check out the release, and run
`DEB_BUILD_OPTIONS="nocheck parallel=2" LOUHI_LUGH_REPOSITORY=/root/louhi-pkg/lugh.git nice -n 19 tools/build-deb.sh`,
then `lintian --fail-on error,warning build-deb/*.changes`.
`build/pkg.sh` (untracked) is the whole script, including the
Dannan and Dagda packages, which need not be built again.

## Next, in order

2. **Dannan 0.10.x**, once Dagda has released on it (one Dannan a
   process; Dagda's 0.4.0 is under way). Lugh has: 0.4.0 (2026-11-22) is
   on Dannan 0.10.2 and lir::mqtt 0.1.0, carries the five TokenSource cases,
   and leaves lugh_client's API as 0.2.0's; its `lugh-node` flags are
   renamed (`--server-address`, `--server-port`, `--oauth-issuer`,
   `--oauth-mtls-port`), which touches nothing of Louhi's, as nothing of
   Louhi's starts `lugh-node`. Lugh 0.5.0 (Lugh's `feature/callback-vocabulary`,
   2026-11-22) makes lugh_client's `Fault`, `Outcome`, `Completion`, `Done`,
   `Executor` and `Deadline` aliases of Dannan's `contract/completion.h`,
   with two new fault kinds (`unavailable`, `timed_out`) where it said
   `failed`, and `lugh::undecided(fault)`. Checked: nothing in Louhi
   switches over a fault kind (its only uses are `ok()` and the message, in
   `ctl/ctl.cpp`), so Louhi needs no change for it. Read Dagda's and Lugh's changelogs' "Adopting" notes again then.
   Found so far against Louhi:
   - `admit::RequestId` becomes `identity::RandomId<Tag>`;
   - the four fuzz targets register with `dannan::testing` and run under
     `dannan::fuzz::smoke` in `louhi_tests`, replacing Louhi's drivers;
   - `listen::Prefix::parse` now refuses host bits set: check
     `model::parse_range`, the security-group and allow-list prefixes,
     their tests and the README's examples; refuse them too, and say so in
     the API's errors;
   - the admit simulation tests' teardown workaround (the weak_ptr and
     owned attempts) goes: Dannan fixed the Agreement lifetime faults and
     (0.10.1) a node's stop racing a scheduled step;
   - `agree::MemoryStore` (and `each_held`), `status::SwappableSink`,
     `test::RecordingSink`, `encoding/base.h`, the durable directory
     helpers, `Completion<T>` and `Outcome<T>` replace Louhi's own where
     they match; `RetryPolicy::unbounded` is already used;
   - CI: the asan and tsan steps pass `-DDANNAN_SANITIZE=address|thread`
     beside the global flags, and check `nm -D --undefined-only` on
     `libdannan.so` for `__asan_` or `__tsan_`;
   - the node's config on Dannan's `docs/config.md`: layers defaults,
     YAML, `secrets.yaml`, environment, command line; key `a.b` is
     `--a-b` and `LOUHI_A_B`; the shared keys `log.level` and
     `log.format`. Check every flag of `louhi-node` and `louhi-ctl`
     against that rule.
3. **The Dagda store** (the rest of step 4), on Dagda's register map (its
   D6 design, `../dagda/TODO/2026-11-20-register-map.md`, with Louhi's
   review taken), when Dagda releases it with the `AcceptorStore` and the
   caught-up signal:
   - the API onto the registers: RegisterTaskDefinition takes its revision
     from `take_revision`, RunTask admits through `admit_tasks`, StopTask
     releases through `release_task`. Which replica set serves Louhi's
     slots (`Serving`) goes to the maintainer with a recommendation then;
   - the records in a domain "louhi" (key from the cluster key by HKDF
     under "louhi/storage/v1"), spaces under "louhi/", tombstones "never";
     every record under 64 KiB but a task definition, which goes to a Dagda
     object when over it;
   - a task record merges with "wanted stopped" absorbing, other field
     groups last-writer-wins on Dannan's hybrid clock;
   - the store stays `Whole::never` for the agent until the caught-up
     signal says whole (discipline 3). Whether "alone" (no peer to compare)
     counts as whole goes to the maintainer then.
4. **Networking:** a task's network namespace on one node, the `nft`
   driver behind `net::Firewall` (the pieces are built and unit-tested). On
   a Pi it creates namespaces and loads nftables rules: the maintainer's
   go-ahead first, a board note, everything removed after.
5. **Steps 6, 7 and the rest of 9** on Dannan's agreement registers:
   placement, then services, then address allocation, on the same
   `AcceptorStore`. Nothing that places across nodes, allocates addresses
   or keeps a service's count is built without it.
6. **Events back on**, when Lir's rewritten broker accepts retained
   messages and wills. The client is released: Lir 0.2.0 (tag `0.2.0`,
   `9085820` on main, 2026-11-22), packaged as `liblir-client0.2` with
   `liblir-dev` 0.2.0; its contract `lir/client/client.h` is the stored
   contract header byte for byte, so Louhi's calls stand;
   `find_package(lir 0.2.0 CONFIG REQUIRED)` and `lir::client`, Build-Depends
   `liblir-dev (>= 0.2.0), liblir-dev (<< 0.3~)`. The broker that takes
   retain and wills will be Lir 0.3.0, after Dannan 0.11.0. The client is on Dannan 0.10.2,
   so it follows item 2, and `liblir-dev` now depends on `libboost-dev`. Until
   Dagda's nouns ship, that broker grants QoS 0 only (until D9), no retain
   (until D6), and refuses every will at CONNECT (until D5): the maintainer's
   ruling, 2026-11-22, sent by Lir. Louhi's publisher sets a retained QoS 1
   will, so every CONNECT would be refused. Decided with Lir (2026-11-22):
   the pin stays on `e40dd2e` and events stay off until D5 and D6 ship, as
   the retained statuses and the will are how a consumer recovers and how a
   dead node's status clears (design, "Events"); no QoS 0, unretained,
   will-less mode. Then: pin the release, QoS 1 if D9 has shipped and
   otherwise QoS 0 for every topic, keep retain and the will, turn
   `LOUHI_BUILD_LIR` on by default, update the README's "Events on Lir".
7. **Owed end to end**, when a Lugh issuer is at hand: `louhi-ctl login`
   and the node's token against a real Lugh; task events live on Lir; a
   private registry signed in to for real.
8. **The image store on Dagda** (step 13's second half), after item 3.
9. **The dashboard** (`2026-11-20-dashboard.md`), on Lugh's
   `@lugh/admin-ui` and Dannan's HTTP core.

## Waiting on others

- **Dagda:** 0.4.0 on Dannan 0.10.x; then the register map, the
  `AcceptorStore` and caught-up (item 3).
- **Lugh:** nothing now (0.4.0 is on Dannan 0.10.2); it says before
  lugh_client's types move onto Dannan's completion (item 2).
- **Lir:** its broker taking retain and wills once Dagda's D5 and D6 ship;
  it says when (item 6). The client is out as Lir 0.2.0.
- **Dannan:** the HTTP core (`api/client.cpp` moves onto its client) and
  the process runner (`net/command.*` moves to it).
- **The maintainer:** a join to the standing Lir cluster
  needs his word each time.

## How work is done

The maintainer's rules (2026-11-20 and 21), which override anything below:

- Features on `feature/<name>`, squashed onto develop when gated; commit
  and push at any time. Releases: README on installation and use, deb
  packages, the full gate (sanitizers and fuzzing) once, tag `x.y.z` on
  `main`; minor or patch bumps only; nothing half-done in a release.
- Every build and test on the cluster, never the Mac (this session, a
  subagent or a worktree). Claim on the board before a heavy job, `-j` no
  more than the cores, `nice 19`, release the moment it ends. **Claim or
  queue only for a specific job ready to run now**, never in expectation.
  Work that other projects wait on goes first; yield between steps when
  asked. Stop everything at once when the maintainer says stop.
- ASan and TSan once on a feature's completion, before its merge, or to
  hunt a bug. Every stack library a sanitizer build links is built from
  source and instrumented (global flags today; `DANNAN_SANITIZE` from
  Dannan 0.10.x). Test once; no flaky test; no mutants unless small or
  high risk.
- **CI steps name every option they rely on** (`-DLOUHI_BUILD_LIR=OFF`):
  a build tree kept on a Pi holds old cached values (COMPLETED.md, 0.1.0).
- Versions are written in full, `x.y.z`, everywhere; a Debian range's
  bounds too (`<< 0.10.0~`).
- On a repin, read the dependency's changelog (or its commits) against
  Louhi's code: what to delete, replace or stop working round. Record what
  changed and what was chosen not to.
- Settle cross-project questions with the other sessions by the stack's
  goals, through the coordinating session; only what the goals cannot
  settle goes to the maintainer. In anything tracked, name it "the
  coordinating session", never by its session name.
- Nothing identifying in the repository: no hosts, addresses or user
  paths (`CLAUDE.local.md` holds them); the Pis are node-a to node-d here.
- `build/pi.sh <address> [step]` (untracked) syncs the worktree named by
  `LOUHI_TOP` with `rsync --checksum` and runs a `tools/ci.sh` step at
  `-j2`. Text tools only on text (logs, notes); code through basemind and
  Read.

## The build Pis

All four: Debian 13, kernel `6.18.62-v8-va48+`, Docker 26.1.5 with user
namespaces. Each runs other projects' services: not Louhi's to touch.

| Host | Memory | Storage bounds | Use |
|---|---|---|---|
| node-c | 15 GB | yes (xfs, `/var/lib/docker-xfs.img`) | gates; the deb |
| node-b | 15 GB | no (ext4) | gates (1 test skipped) |
| node-a | 15 GB, no memory controller | no | hosts the board; last |
| node-d | 4 GB | no | gates; a heavy link at `-j1` |

All four take any load, node-d included (the maintainer, 2026-11-20,
confirmed by the coordinating session 2026-11-22); a Pi lost mid-job is
normal, and the job is run again elsewhere. Questions about using them go
to the board and the coordinating session.

node-c's xfs undo is `/root/louhi-work/UNDO-xfs.txt`. Before-wipe records
are in `build/pi-records/` (untracked).

## Principles

- The four laws and five disciplines, `docs/principles-and-laws.md`, cited
  by number.
- A caller describes a task, never a machine. No single-node special case.
- Declared state in Dagda; nodes converge towards it; nothing is
  remembered that cannot be re-derived. Desired and observed state are
  separate records with separate writers.
- No guarantee the stack does not give: no exactly-once, no uniqueness
  without a claim, no lock.

## Decisions

All the maintainer's, 2026-11-15 unless dated; superseded ones folded in.

- **Scope:** ECS and Fargate style container compute, with the networking
  and storage substrate for it, on the stack (Dannan, Dagda, Lugh, Lir).
  The cluster is ours and trusted; contributed machines later, not closed
  off. Dannan is not changed for Louhi.
- **Runtime:** the Docker Engine API over its socket, behind an interface;
  Linux nodes only. Hardened containers now, shaped for a microVM driver
  later. Memory limits, user namespaces and (2026-11-16) storage bounds
  are enforced unless the operator allows otherwise.
- **Agreement:** placement, address allocation, quota admission and
  (2026-11-20) revision numbers are Dannan agreement registers, their
  acceptor state in Dagda's `AcceptorStore`; no acceptor store of
  Louhi's own, no scheduler, no stand-in.
- **Records (2026-11-20):** in Dagda, domain "louhi"; a task record merges
  with a stop absorbing, else last-writer-wins on the hybrid clock.
- **Tenants:** projects with enforced quotas, set by `louhi:admin` only.
- **API:** Louhi's own, declarative, AWS's vocabulary. Sign-in by the Lugh
  plugin only; audience "louhi", scopes `louhi:<project>:read|register|run|*`
  and `louhi:admin`; HTTP and HTTPS the operator's choice.
- **Events:** on Lir through Lir's own client; Louhi writes no MQTT client.
  Off by default until a Lir release ships that client (the coordinating
  session, 2026-11-20).
- **Images (2026-11-16):** a tag resolved to a digest at registration
  through the runtime; registries an operator allow-list with credentials
  in `secrets.yaml`.
- **Storage (2026-11-16):** a task's writable layer bounded by xfs project
  quotas; scratch volumes in memory (counted) or on disk (bounded).
- **Config (2026-11-20):** YAML through `dannan::config` is the source of
  truth; flags and environment variables override any key by Dannan's
  naming rule.
- **Retiring a node (2026-11-17):** `louhi-node --retire`, as `lir-node`.
- **Overlay carrier:** WireGuard where two nodes reach each other directly,
  Dannan frames otherwise. Gateways and ingress on any node with public
  reach, settled by a register.
- Deferred: workload identity; what persistent volumes are made of.
