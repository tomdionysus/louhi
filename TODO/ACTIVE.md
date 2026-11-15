# Louhi - Active Work

Last updated: 2026-11-15 (end of session). `COMPLETED.md` is the record.

## Resume here

**State.** `develop`, pushed to the private `tomdionysus/louhi`. Steps 0 to 3
are done: records and codecs, the Docker driver, the agent, the store
interface with the in-memory store, and `louhi-node`; user namespaces and
memory limits are enforced. Last gate: 136 passed on node-d, Docker
tests included, under user namespaces. Dannan pinned at `0.8.1`.

**Read first:** the design, [`2026-11-15-louhi-design.md`](2026-11-15-louhi-design.md);
then this file's Decisions. the maintainer's standing instruction: work autonomously
until he is needed, commit and push; decisions go to him as interactive
questions with a recommendation.

**How work is done.** Nothing is built or run on the Mac. Builds and tests
run on the shared Pi 5 build servers, claimed on the board first
(`CLAUDE.local.md` has hosts and rules). `build/pi.sh <address> [step]
[gtest filter]` (untracked) syncs the tree and Dannan's history to a Pi and
runs a `tools/ci.sh` step there at `-j2` under `nice 19`. One board wait per
session at a time: a second `claim-board wait` replaces the first; wait on
several hosts in one command.

## Next, in order

1. **Repin Dannan to `0.8.3`** (tagged and pushed), its own commit and gate.
   Lugh's client and Lir's client are both on 0.8.3, and one process links
   one Dannan.
2. **Step 5: the API and `louhi-ctl`,** on `louhi-node`. What is decided:
   - HTTP and JSON on Dannan's listener layer (`dannan/listen/listener.h`,
     `StreamListener` handing up `listen::Stream`s), Beast's HTTP parser over
     the stream with a body limit and a read deadline. Lir's
     `lir/http/http.cpp` is the pattern (an `Exchange` per connection, the
     listener on a strand, stop in two steps); read it, do not copy it.
   - Sign-in: the Lugh plugin only, no anonymous mode. `lugh_client`
     (`../lugh`, target `lugh::client`, fetched with `LUGH_BUILD_NODE=OFF`;
     it takes the Dannan the build already has) verifies access tokens
     offline with `Verifier` and `HttpsKeySource`. Lir's `lir/auth/lugh.h`
     is the shape. Tests sign real tokens with `lugh/client/testing.h`.
   - Authorisation: the token's own claims, as Lir does. Audience "louhi";
     scopes `louhi:<project>:<action>` and `louhi:admin`; a grant lasts until
     exp. Not Dannan's `Identity::verify`, which answers only from a Lugh
     scope source in the same process.
   - Projects and quotas are created and set through the API, `louhi:admin`
     only.
   - Quota at admission: checked on the node that takes the request (single
     node); cross-node admission is agreement's, step 6.
   - Run task places on the node that takes it until step 6.
   - Endpoints, AWS vocabulary: register a task definition, run a task, stop
     it, describe and list tasks and statuses, put a project and its quota.
     Write the endpoint list into the design's "API and sign-on" before
     coding, and ask the maintainer only what it leaves open.
   - Watch out: `lugh_client` compiles Boost.JSON's `src.hpp` and so does
     Louhi (`runtime/docker/boost_json.cpp`); linking both may define it
     twice. Settle it at the first build (probably by Louhi dropping its own
     when it links `lugh::client`).
3. **Step 4's second half**, the Dagda store, waits on a Dagda ref.
4. **Steps 6 and 9** wait on Dannan 0.9.0's agreement registers.
5. **Step 8:** pin Lir `6b23268` (needs Dannan 0.8.3) and link
   `lir::client` with `-DLIR_BUILD_SERVER=OFF`. Lir's notes: `Tls` is
   declared in `client.h`; a QoS 0 publish's done fires once handed to the
   connection.

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
  late): its lir-node upgrades are postponed and it will message Louhi
  before doing them, so node-a need not wait on Lir; the standing cluster
  has four nodes, so 3 of 4 stay up. If Lir has since said it is upgrading,
  do not overlap. Post a note, claim the whole machine (cores "all",
  until about 20 minutes out) and reboot. Checks after (the Lir Development
  Cluster session's): `systemctl is-active lir-node
  claim-board-scheduler`; `journalctl -u claim-board-scheduler -n 3 -o
  cat` says "acting" on node-a, and node-b's log goes back to "standing
  by: node-a acts"; `curl -s
  'http://192.0.2.10:8183/retained?filter=stack/%23'` returns the same
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
- **Workload identity:** deferred.
- **MQTT client:** Lir's exported client; Louhi writes none.
- **Overlay carrier:** both: WireGuard (with a VPC tag inside) where two
  nodes reach each other directly, Dannan frames otherwise.
- **Persistent volumes:** what they are made of is decided later (step 14).
- **The build Pis** are also Louhi's test bed: Docker, user namespaces and
  the memory controller on all four.

## Waiting on the maintainer

1. **Dagda ref.** the maintainer: wait. Dagda 0.2.0 (every-node head spaces,
   `Heads::watch`, `lease::ClaimedCollection`) is tagged locally and held at
   the maintainer's ruling. Dagda's caution: claimed collections stand on leases, which
   order work on one node only today. Step 4's second half waits on this.
2. **Under-voltage:** node-a (110 events in 24 h) and node-d
   report `throttled=0x50000`. Builds pass; the maintainer's hardware, reported only.

## Owed at the first release gate

Build and run the three fuzz targets (`record`, `image_reference`,
`docker_reply`) with clang, `-max_total_time=60` each; the `clang`, `asan`,
`tsan` and `coverage` steps on a Pi. Hosted CI runs them on every push
meanwhile.
