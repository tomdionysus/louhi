# Louhi - Completed Work

The record, newest first. `ACTIVE.md` is the plan. Paths under
`/root/louhi-work` in entries before 2026-11-18 are gone: the Pis were
wiped that day.

## A task's interface on its node: the command runner, the veth plan, nft (2026-11-20)

Designed first ("A task's network namespace"): a sandbox container per task
holds the network namespace its containers join; the host end of a veth pair
is named from the task id, so a restarted agent re-derives whose it is.

- `net/command.h`: `Commands`, and `ProcessCommands` on posix_spawn: a fixed
  argument list, never a shell, the program found on a fixed system PATH,
  input on stdin and output (bounded at 64 KiB, cutting said) over Unix
  socket pairs so a child that exits early raises no SIGPIPE, SIGKILL at the
  deadline, the child reaped without blocking, one posted completion.
  Interim: Louhi and Macha both need it, so it moves into Dannan (item 23),
  which starts from this one.
- `net/host.h`: `attach_plan` and `detach_plan` as argument lists (a veth
  pair, its far end moved into the sandbox's namespace as eth0 with the
  task's address and a default route, proxy ARP on the host end so the task
  needs no gateway address, the address routed to the host end in the VPC's
  routing domain); `IpHost` runs a plan step by step, undoes the pair on a
  failure and names the step; a pair not there is no error to remove.
  Names, addresses and process ids are checked first.
- `NftFirewall`: `Firewall` by `nft -f -`; a removal is add-then-delete, so
  it succeeds whether or not the table was there.
- Nothing calls them from the agent yet, and the real drivers have not
  touched a node: that needs the maintainer's go-ahead on a Pi (`CLAUDE.local.md`).
- The design's "a single node places on itself" is struck (the maintainer: no
  single-node code); the maintainer's answers to the stack's questions and his Q6
  ruling are recorded in ACTIVE.md.

Gates on node-b: release **334 passed, 1 skipped**; ASan with UBSan
and TSan **334 passed, 1 skipped** each (the skip is node-b's: Docker on
ext4 cannot bound storage). 335 tests, 318 before.

## The API's route table, its OpenAPI document, and /v1/status (2026-11-20)

The dashboard's first step (`TODO/2026-11-20-dashboard.md`, "Order" 1 and
part of 2), which waits on nothing.

- `api/routes.h`: the API's routes as one table (method, path template,
  scope, operation, operation id, summary). `Api::handle` matches the path,
  checks the path's names, answers 405 for a method no route takes, checks
  the route's scope once from the table, then runs the operation: the
  handlers no longer check scopes themselves.
- `GET /v1/openapi.json` (any valid token): OpenAPI 3.1, its paths from the
  route table and its schemas written beside them, built once. A test checks
  every route against the document and every documented path against the
  routes, and that every reference resolves; Dannan's HTTP core will give
  that check to every service (`check_openapi`), and Louhi moves onto it.
- `GET /v1/status`: this node's id, version, readiness and the store's
  state; `louhi:admin` also sees its task count and its last NodeStatus.
- A few malformed paths now answer 404 "the path" rather than naming a
  part; every status code is unchanged, and every earlier test passes as
  written.

Gates on node-b: release, ASan with UBSan and TSan **350 passed, 1
skipped** each. 351 tests, 346 before. The first release run failed to
link: `build/pi.sh` kept the Mac's file times, so ninja took an object
built from another branch for current; it now syncs by content.

## louhi-ctl's settings in YAML through Dannan's config reader (2026-11-20)

the maintainer: "All service config must be in yaml, using yaml-cpp", read through
Dannan's `config::Reader` under the no-duplication rule.

- louhi-ctl's own argument and environment parsing is gone: `read_args`
  declares its settings to Dannan's Reader (a config file, then `LOUHI_*`
  variables, then flags; `--config`, `$LOUHI_CONFIG`, or
  `~/.config/louhi/config.yaml`). A config error is a usage error.
- Login writes its settings (issuer, client id, endpoints, scopes, CA) to
  `config.yaml` with yaml-cpp's emitter (nothing else in the stack writes
  YAML, so writing stays Louhi's), and the tokens to an owner-only
  `credentials.yaml`, both through `durable_replace_file`.
- An old `credentials.json` at the default path is moved once, so nobody
  logs in again; logout removes it too.
- Three gaps in Dannan's Reader are worked around here and go into Dannan's
  second minor (its item 27): a value flag given twice, a lone "-" operand,
  and an empty variable read as unset. The workarounds go when it ships.
- Louhi finds and links yaml-cpp itself (Dannan links it privately).

Gates on node-b: release, ASan with UBSan and TSan **345 passed, 1
skipped** each. 346 tests, 335 before.

## Networking groundwork: the network records, security groups as nftables (2026-11-20)

The design first ("The network records and security groups in detail"),
then the tests, then the code; on `feature/network-records`, squashed onto
develop (the maintainer's word to the stack: features on branches, squashed).

- `model/network.h`: Vpc, Subnet, RouteTable and SecurityGroup, with
  their checks. Ranges are IPv4 with no host bits, parsed by Dannan's
  `listen::Prefix` with one spelling each; a VPC or subnet is /16 to /28.
  `check_subnet` (inside its VPC, overlapping no sibling, the sibling
  named) and `check_route_table` (no route inside the VPC's range, which
  is local) need other records, so they are checks apart from
  `validate()`. `route()` is local first, then the longest prefix.
  A security rule is any, tcp, udp or icmp, ports only for tcp and udp,
  exactly one peer, a range or a group.
- Codecs for the four (kinds 8 to 11, format version unchanged), keys
  under `vpc/`, `subnet/`, `routetable/` and `securitygroup/` by project
  and VPC, and the `record` fuzz target over the new decoders.
- `net/firewall.h`: `resolve()` turns a task's groups into rules with every
  peer a range (a group peer is its members, one with none matches
  nothing); `render()` writes one nftables table of the task's own,
  replaced whole (add, delete, define), on its host-side interface:
  established and related accepted, invalid dropped, the rules accepted,
  the rest dropped, each direction alone. Interface names are checked, so
  no record writes into the rules. `Firewall` is the interface the `nft`
  driver will implement with the task namespaces (step 9).
- Found by the first run: the design's own rule refused a default route
  (`0.0.0.0/0` overlaps every VPC). The rule is AWS's: a route may not lie
  inside the VPC's range. Fixed in the design, the check and its test. A
  codec test used kind 8 as its unknown kind; it now uses 12 and 255.

Gate on node-c: **318 passed**, none skipped (302 before). Before
the merge (the maintainer: sanitizers on a feature's completion), on node-b:
ASan with UBSan **317 passed, 1 skipped**, TSan **317 passed, 1 skipped**
(`DockerDriverTest.ADiskVolumesSizeIsKeptWhereStorageIsBounded`: node-b's
Docker is on ext4, which cannot bound storage). The first TSan build
stopped in Lugh's client, whose warnings are errors by default while g++
warns inside Asio under `-fsanitize=thread`; a fetched Lugh's warnings are
now errors only where Louhi's are (develop `6971f64`).

## The stack's audits: what Louhi moved first (2026-11-20)

the maintainer's two audits (no duplicated functionality; no special node) were
negotiated among the sessions through the Lir Development Cluster session,
which keeps the converged plan
(`../lir-develop-cluster/TODO/2026-11-20-cluster-stack-placement.md`,
"Louhi"). Louhi agreed it, correcting item 3. What it released to Louhi
alone, done:

- **Dannan's `RetryState`** for the agent's restart budget and backoff
  (`Policy::restarts` is a `dannan::sched::RetryPolicy`; a quiet window
  starts the backoff again; a launch backs off and is never parked) and
  for the service token's backoff. The two never-parking policies take
  the plan's interim form (no budget, a 1 ms window, no run bound) until
  Dannan names one. One new test: after a quiet window a service task's
  backoff starts again.
- **Dannan's `hex`/`unhex`** for task ids; **`percent_encode`** for the
  Docker API's query strings (it was the unreserved set, byte for byte).
- **louhi-ctl tries every endpoint it knows** (S11): `--endpoint`
  repeatable, `$LOUHI_ENDPOINT` a list, every endpoint given at login
  saved (an older single `endpoint` is still read). A call goes to the
  next endpoint past one that cannot be connected to (nothing was sent),
  and a GET past one that gave no answer too; a write that may have
  reached a node is never sent to another, and the person is told it may
  have happened; any HTTP answer is the answer. Eight new tests.
- Item 3 (`api/client.cpp` onto Lugh's `HttpsClient`) is not a straight
  duplicate: Louhi's client speaks plain `http://`, which the maintainer ruled for
  the API. Settled with the coordinator: it moves once, onto Dannan's
  HTTP client if the maintainer says yes to the HTTP question, else onto Lugh's once
  it takes `http://`. Left as it is until then.

Gate on node-c: **302 passed**, none skipped (293 after the
RetryState move, 292 before). Two runs failed to compile first (a call to
the old backoff left in; local names shadowing in `ctl.cpp`), fixed.

## Step 4 begun: Dagda fetched, the revision and admission registers (2026-11-20)

- **the maintainer's rulings written into the design** ("What the stack provides",
  "Records", "Placement", "Projects and quotas", the API): records in one
  every-node head space of Dagda 0.3.2 in a domain "louhi"; the Task merge
  (a stop absorbs, else last-writer-wins on the hybrid clock); revision
  numbers and quota admission from agreement registers, their acceptor
  state in Dagda's `AcceptorStore`. The earlier "quota enforced twice"
  is replaced by admission through a register.
- **The space's owner.** Dagda 0.3.2 has none outside its test fixtures.
  Asked, the maintainer: "Dagda only does this if other projects that use Dagda can
  use this feature." It is Dagda's ruled register map (slice 3), which
  serves Lir, Lugh, the catalogue and the leases as well, and goes into
  Dagda's next release (the maintainer's ruling in the Dagda session). Louhi writes
  no owner, so its records wait for that release.
- **Dagda fetched** at `0.3.2` (`LOUHI_BUILD_DAGDA`, default on), with
  Dannan declared `OVERRIDE_FIND_PACKAGE` so Dagda's `find_package` takes
  the fetched one, and Dagda `EXCLUDE_FROM_ALL`. Dagda is private, so
  `tools/ci.sh` builds with it only where `../dagda` or
  `LOUHI_DAGDA_REPOSITORY` is, and hosted CI and CodeQL build without it.
  Configured, built and linked on node-c; Dagda's `install(EXPORT)`
  raised nothing. Told to the Dagda session.
- **`louhi::admit`**, Louhi's registers on Dannan's agreement:
  - `admit/registers.h`: a family's revision counter (the next revision,
    and the last 256 requests with what each was given, so a request asked
    again is answered with its own number; one retried past that window
    gets a new number, leaving a gap, never a duplicate); a project's
    admitted tasks, by id with each one's CPU and memory, usage derived
    from them and never counted, admitted all or none within the quota's
    tasks, CPU and memory, a refusal naming the limit and committing the
    value unchanged (a change cannot refuse); a release by task id, so it
    gives a share back once. Every change is `repeatable`, and leaves a
    value that does not decode as it was, with an empty result its caller
    refuses rather than throwing on Dannan's strand. Codecs versioned from
    1, bounded, in order, exact.
  - `admit/agreement.h`: `Registers`, implemented on `agree::Agreement`
    (`AgreementRegisters`, the slot by `slot_of`, the replica set the
    caller's), and `take_revision`, `admit_tasks`, `release_task`, each
    answering evidence with `unknown` when it may have taken effect.
    Nothing calls them yet: in real use they wait on the `AcceptorStore`.
  - Tested on Dannan's simulation harness (`sim::Cluster`, real
    `Agreement`s on `sim::Store`, linked through `dannan_sim_checkers`;
    a fetched Dannan has no `dannan::` alias for it): five nodes, one
    message in eight lost, one in ten duplicated, delays to 30 ms, a node
    crashing now and then, clients asking again with the same request
    until answered, eight seeds. Thirty requests are given revisions 1 to
    30, each one number however often it was asked; thirty admissions
    against a quota of ten admit exactly ten, no answer ever showing more;
    each admitted task released from two nodes at once leaves nothing
    admitted, and the twenty refused then admit exactly ten.
- **`louhi-ctl login --scope openid`** is a usage error before anything
  is sent (`LoginRefusesTheOpenidScope`): louhi-ctl needs no ID token,
  and one would fail verification after the person had answered.

Gate on node-c (never throttled, Docker on xfs with user
namespaces): **292 passed**, none skipped (277 before: one ctl test, twelve
register tests, two simulation tests). `tools/ci.sh check` passes. The
first run failed to compile (an include missing from the register test,
and dangling-else warnings in the simulation test); fixed and run again.

## Docker on all four Pis again; the full gate on Dannan 0.9.2 (2026-11-20)

- the maintainer: put Docker on all the Pis. Each Pi in turn, one core claimed, a
  note on the board before and after: `docker.io` and `docker-cli`
  26.1.5, `userns-remap: default` written before the daemon's first
  start, so it never ran without user namespaces. node-c's Docker data
  is on a 16 GB xfs loop file (`/var/lib/docker-xfs.img`, `pquota`, in
  `/etc/fstab`). What each change replaced is in `/root/louhi-work/` on
  each Pi (`nft-before-docker.txt`, `cmdline.txt.before`, `fstab.before`;
  node-c's `UNDO-xfs.txt`); the script is `build/docker-install.sh`.
  No reboot; lir-node, lugh, mimir, macha and the board stayed active,
  and the board answered on node-a after.
- node-c's stale socket from before the wipe was removed first (no
  daemon behind it).

Gate on node-c (never throttled, Docker on xfs with user
namespaces): **277 passed**, none skipped. Nothing left on the node but
the busybox image, kept as a cache.

## Dannan 0.9.2, with Lugh and Lir moved with it (2026-11-20)

- Pins: Dannan `0.8.3` to `0.9.2`; Lugh `382416d` to `dbb0573` (the
  commit Lir's Lugh plugin builds at, on the same Dannan); Lir `4443c16`
  to `6268fef` (its pushed develop, on 0.9.2). One step, as decided.
- No source change was needed. Of 0.9.0's changes for a caller from 0.8,
  Louhi uses none (`CallOutcome::peer`, `InboundHandlers`, `StateLock`);
  `durable_replace_file`'s new `FileSystem&` defaults to POSIX. Lugh's
  client dropped `public_key_der`, which Louhi never called; Lir's client
  is unchanged between the two commits. Dannan's TSan suppressions are
  still at `dannan/test/tsan-suppressions.txt`.
- Found by the first run, which aborted: node-c keeps a Docker socket
  file from before the wipe with no daemon behind it, so `build/pi.sh`
  turned the real-Docker tests on, and `AgentOnDocker`'s first failed
  ASSERT returned past its io thread, still joinable: `std::terminate`,
  the rest of the binary unrun. The test now stops its io and joins on
  every way out, so a failure fails it alone. `build/pi.sh` turns the
  Docker tests on only when the daemon answers `/_ping`. The stale socket
  is left (not Louhi's machine to change).

Gate on node-c (never throttled, no Docker since the wipe): **277
run, 264 passed, 13 skipped** (every real-Docker test). The Docker tests
have not yet run on Dannan 0.9.2; they are owed when Docker is back.

## The Pis wiped; the documents brought up to date (2026-11-19)

- the maintainer had all four Pis cleaned to a bare OS on 2026-11-18. Docker and
  `/root/louhi-work` are gone from each; the build toolchain is there.
  The records of each Pi's settings before Louhi changed them were copied
  off first, to `build/pi-records/<host>/` on the Mac (untracked).
- Dannan 0.9.0 was released meanwhile (0.9.2 the latest), so the pin move
  is first in the plan.
- `ACTIVE.md` rewritten for both; the README's status brought up to what
  is built (277 tests, images by digest, storage bounds, `--retire`,
  `louhi::ctl`).

No code changed, so no gate.

## louhi-node --retire (2026-11-17)

- Found by the Lir session: the standing Lir cluster counted a fifth,
  unreachable member, the `louhi-node` run on node-c of 2026-11-16, though
  it stopped cleanly. Dannan never drops an absent member; only
  `Nodes::retire` (an operator's word) does. The plan said otherwise and is
  corrected.
- the maintainer: Louhi needs the same command as `lir-node`'s. `louhi-node --key FILE
  --state DIR --retire NODE_ID...` opens the node's state without starting
  it and retires each id (Dannan 0.8.3 has `Nodes::retire`, so no pin
  moves). Refused while the node runs (the state lock), and for this node's
  own id; one JSON line an id, `retired` false when it was not a member.
- Checked on node-b with two `louhi-node`s on loopback and a
  throwaway key: refused while running (exit 1); b retired from a's state,
  true, then false; a's own id refused (exit 1); a malformed id a usage
  error (exit 2). Nothing left on the Pi. A `louhi-node` tool has no unit
  test; the check is by hand, as `lir-node`'s.

Gate on node-b (never throttled, Docker on ext4): **277 run, 276
passed, 1 skipped** (the storage-bound test, which needs xfs quotas), the
same count.

## Tests of their own for louhi-ctl and ServiceToken (2026-11-16)

- `ServiceToken` (`auth/service_token.h`) takes the issuer's `lugh::client::Https`,
  an executor and a timer function (the agent's `Clock::after` shape), so it is
  tested against a fake issuer and stepped time; `louhi-node` hands it an
  `HttpsClient` and the io's timer. A timer still pending holds it weakly.
- `louhi-ctl` is now `louhi::ctl` (`ctl/ctl.h`), built with the Lugh plugin;
  `tools/louhi_ctl.cpp` only binds it to the real issuer, API, files, clocks
  and streams. `api::Requester` is the interface `api::Client` implements.
- Shared fixtures: `test/fixtures/fake_https.h` (an issuer that holds each
  request until answered, or answers by a script) and `test/fixtures/timers.h`
  (moved from the agent's test).
- Tests: `auth_service_token_test.cpp` (11) and `ctl_test.cpp` (28): every
  command's call, operands one segment each, usage errors before anything is
  asked, the credentials codec and file (owner-only, replaced whole,
  bounded), the token refreshed with a minute or less left and the rotated
  refresh token kept, a refused refresh changing nothing, the endpoint's
  precedence, exit codes, the device-flow login, logout.

Found and fixed, each with its test:
- `ServiceToken::make` accepted private_key_jwt without a key or kid, and
  client_secret_basic without a secret; the OAuth client would then have
  thrown inside an io handler once discovery was read. Refused at `make`,
  as is tls_client_auth (the token's HTTPS presents no certificate).
- `louhi-ctl run-task --count -1` sent 18446744073709551615 (`stoull` wraps
  a sign), and `--count abc` exited 1; `--count`, `--revision` and `--limit`
  take digits only, else exit 2. A malformed `--endpoint` exits 2.
- `louhi-ctl` wrote its credentials with a writer of its own, against rule
  3; it uses Dannan's `durable_replace_file`.
- The promise a command waited on lived on the stack: an answer after the
  wait gave up would have set a promise already gone. It is shared now.
- The first run failed one test, and the test was wrong: its failure helper
  expected the first token after a second had been taken.

Gate on node-c (never throttled, Docker on xfs with user
namespaces): **277 passed** (238 before, 39 new), none skipped.

## Storage bounds kept for real; first live publish to Lir; louhi-node's stop (2026-11-16)

- node-c's Docker data moved onto a loop-mounted xfs file with
  `pquota` (the maintainer's go; the undo in `/root/louhi-work/UNDO-xfs.txt`). The
  driver reports `limits_storage` there. The storage test takes its kept
  branch, the agent's real task runs bounded, and a new test shows a disk
  volume's size kept: a write past it fails. Checked by hand too: 4 MiB fit
  in an 8 MiB volume, and the next write stopped at 8 MiB.
- First live publish to Lir, on the standing cluster with `louhi/#` open
  (the maintainer's choice). A `louhi-node` on node-c joined at 01:34Z and published
  its retained status on `louhi/nodes/<id>`, read back through the board.
  Its clean stop cleared it (Lir saw it cleared at 01:35:29Z).
- Found: every `louhi-node` stop hung. Main ended the io threads before
  `dannan::Node` was destroyed, and Node's client's second `stop()` waits on
  its own strand. Seen with gdb, then reproduced on a solo node. The node
  is now destroyed while its io threads still run, as `dannan-node`'s is;
  a solo node stops in 5 s.

Gate on node-c (never throttled, Docker on xfs with user
namespaces): **238 passed** (237 before, 1 new), none skipped.

## Lugh pinned at 382416d (2026-11-16)

Lugh's HttpsClient on Dannan's connector, and Lugh itself on Dannan
0.8.3, which Louhi already pins; `HttpsClient`'s constructor is unchanged.
Gate on node-c (never throttled, Docker): **237 passed**, the same
count. node-d stopped answering ssh mid-session (posted on the
board).

## Step 13, first half: images by digest, ephemeral storage bounds (2026-11-16)

- The design's detail written first ("Images in detail", "Ephemeral
  storage in detail"); the maintainer's answers: resolve through the runtime, an
  operator allow-list with per-registry credentials, xfs project quotas
  with a refusal otherwise, and scratch in two kinds.
- `runtime/registries.h`: the allow-list by domain (`docker.io` for a
  reference with no domain; no list allows none) and one credential per
  allowed registry, a user name with a password or an identity token.
- `Driver::resolve`; the Docker driver asks `GET /distribution/<ref>/json`
  with `X-Registry-Auth` (base64url, by hand), refuses an unlisted registry
  before asking, and pulls with the same header. `parse_distribution` is in
  the `docker_reply` fuzz target.
- The API resolves every container's image before a revision is admitted;
  a digest the caller sends is ignored. Refusals: `image_refused` and
  `image_not_found` (400), `image_unresolved` (503); no revision is spent.
- `TaskDefinition::ephemeral_storage_mib` (64 to 1 TiB, 1024 by default in
  the API), shared equally by its containers as `StorageOpt` `size`.
  `VolumeKind` is `memory` (tmpfs; the sum must fit in the task's memory)
  or `disk` (the local driver's `size` option). Record format 2: version 1
  was never kept.
- `RuntimeInfo::limits_storage` (overlay2 on xfs); the agent and the node's
  status refuse a node without it unless `--allow-unbounded-storage`, and
  then send no bound. `louhi-node --registry`, `--registry-credential`.
- Found on the Pis: Docker answers a storage bound on ext4 with a 500
  ("--storage-opt is supported only for overlay over xfs with 'pquota'"),
  so the driver classes it `unavailable` and the agent's launch backs off
  with that reason. Only reachable where xfs lacks `pquota`.

Gate on node-d (Pi 5, g++, Release, warnings as errors, Docker with
user namespaces, ext4): **237 passed** (207 before, 30 new). Not yet run:
any storage bound kept by a real runtime, since no Pi has xfs.

## Step 8: events on Lir (2026-11-15)

- The topics written into the design's "Events" first; the maintainer's answer on
  how the node signs in to Lir: `private_key_jwt` and `client_secret_basic`,
  both.
- `louhi/events/events.h`: a task's status retained on
  `louhi/projects/<p>/tasks/<t>/nodes/<n>`; `task.started`, `task.stopped`,
  `task.parked` on a change of state, `task.run` and `task.stop_requested`
  from the API once their writes land, on `louhi/projects/<p>/events`; a
  node's status retained on `louhi/nodes/<n>`, cleared by its will or on a
  clean stop. `EventReports` passes the agent's reports to the store first.
- `louhi/events/lir.h`: the publisher over `lir::client` at Lir
  `4443c16` (its pushed develop, with discovery in Dannan's roster),
  fetched with `LIR_BUILD_SERVER=OFF`; publishes Lir does not take are
  counted. `LOUHI_BUILD_LIR` (on); Lir is public, so hosted CI builds it.
- `louhi/auth/service_token.h`: the node's own token by the
  client-credentials grant, refreshed at two thirds of its life, with
  backoff; Lir's password callback reads it without waiting.
- `louhi-node --lir [--lir-tls] [--lir-client-id ...]`. A signing key is
  read as PKCS#8 DER, Dannan's only key format, rather than Louhi parsing
  PEM itself.
- Hosted CI's first public run failed TSan on Asio's executor count (a
  release decrement then an acquire fence, which g++'s TSan cannot model),
  reached through Dannan's connector; `tools/ci.sh tsan` now uses the
  fetched Dannan's own suppressions, as Lir does, and CI is green.

Gate on node-c (Pi 5, g++, Release, warnings as errors, never
throttled, Docker with user namespaces): **207 passed** (200 before, 7
new). Not yet run: a live publish to Lir, and `ServiceToken` against a
real Lugh (`ACTIVE.md`, "Owed, end to end").

## Step 5: the API and louhi-ctl (2026-11-15)

Merged from `feature/api` (`5c1fdce`, merge `f309ecb`).

- `louhi/api/api.h`: the design's endpoints over the store, apart from
  HTTP. Projects and quotas by `louhi:admin` only; every other operation
  behind `louhi:<project>:<read|register|run>`, checked before anything
  about the project is said. Revisions from 1; RunTask on this node, epoch
  1, admitted against the quota (tasks, CPU, memory) counting this node's
  admitted-but-unwritten tasks; a client token remembered an hour; a
  stopped task stops counting. Answers posted, never inline.
- `louhi/api/json.h`: strict, bounded bodies; unknown members refused; the
  field named, never the value; what the API shows registers back as is.
- `louhi/api/auth.h`: Bearer only, audience "louhi", unbound, before exp;
  unparsable scopes skipped.
- `louhi/api/http.h`: HTTP/1.1 on Dannan's listeners, plain and HTTPS each
  optional; body and header limits, a read deadline, one request per
  connection. `louhi/api/client.h`: the client over Dannan's connector.
- `louhi/auth/lugh.h`: the Lugh plugin, `lugh::client` at `a34770f`. With
  it, Louhi's own Boost.JSON definitions are dropped (the double-definition
  question settled); without it (`LOUHI_BUILD_LUGH=OFF`, hosted CI) there
  is no API and no `louhi-ctl`.
- `louhi-node --api-http/--api-https --lugh-issuer`; it now joins its io
  threads before the Api and store go out of scope.
- `louhi-ctl`: device-flow login, refresh, the AWS-named commands.
- Fuzz target `api_body`.
- Found at the first run and fixed: a handler that threw after taking
  `done` answered through the moved-from function (`bad_function_call`);
  the refusal now goes through a copy kept by `handle()`.

Gate on node-c (Pi 5, g++, Release, warnings as errors, never
throttled, Docker with user namespaces): `tools/ci.sh release` **200
passed** (136 before, 64 new). `tools/ci.sh check`'s format and header
checks pass. Not yet run: `louhi-ctl login` against a real Lugh.

## Dannan 0.8.3; step 5's endpoints (2026-11-15)

- Dannan repinned from 0.8.1 to 0.8.3 (`CMakeLists.txt:37`): additive for
  Louhi (the stream connector, RS256, placement copying only its answer).
  Gate on node-d (Pi 5, g++, Release, warnings as errors, never
  throttled, Docker with user namespaces): `tools/ci.sh release` **136
  passed**.
- The API's endpoints written into the design's "API and sign-on". the maintainer's
  answers: scope actions `read`, `register`, `run`; plain HTTP and HTTPS
  each the operator's choice, as Lir serves.

## User namespaces enforced; the Pis prepared (2026-11-15)

- the maintainer: user namespaces are enforced. The agent launches nothing on a
  runtime without them unless the operator allows it
  (`Policy::allow_without_user_namespaces`, `louhi-node
  --allow-without-user-namespaces`), and the node's status is not ready
  without them, as for memory limits. The fake runtime now has them; the
  real-Docker agent test allows both, since not every Pi has them.
- node-d: `cgroup_enable=memory` and Docker `userns-remap: default`
  (root in a container is uid 165536 on the node), one reboot; no daemon.json
  before (`/root/louhi-work/daemon.json.before`, `cmdline.txt.before`).
- node-b: Docker installed, `cgroup_enable=memory`, rebooted with
  the Lir Development Cluster session's checks passed (lir-node and the
  scheduler active, standing by for node-a, the board answering alike). Its
  userns-remap is still to do (a Docker restart).
- node-c: `cgroup_enable=memory` and `userns-remap: default`, one
  reboot, the same checks passed (lir-node, scheduler, Docker and redis
  active; standing by for node-a; the board answering alike).
- node-a is still to do (`ACTIVE.md`, "The build Pis").

Gate on node-d (Pi 5, g++, Release, warnings as errors, never
throttled, Docker 26.1.5 with user namespaces, memory controller on):
`tools/ci.sh release` **136 passed, none skipped**. The driver's real
tests, the hardened profile among them, pass under user namespaces; the
runtime reports memory, CPU and pid limits and user namespaces all on.
Nothing left on the node.

## louhi-node (2026-11-15; step 3's node)

- `tools/louhi_node.cpp`: a Dannan node that advertises "louhi" and hosts
  the agent over the Docker driver and the in-memory store; settings through
  Dannan's config reader (`LOUHI_*`, `/usr/local/etc/louhi/config.yaml`);
  JSON event lines on stdout as `lir-node` does. Stopping it leaves tasks
  running.
- `node/status.h`: the node's NodeStatus from what the runtime says and what
  is placed on it: ready when the runtime answers and can enforce memory
  limits (or the operator allows it), every degradation a condition,
  allocation of tasks wanted running, saturating, runtime text bounded so the
  record always validates. Written under the node's key when it changes.
- `store/memory_store.h`: `Whole::never`, which the node uses. A node's
  store in memory remembers nothing across a restart, so an agent over an
  always-ready one would have taken an empty store for "nothing wanted" and
  removed every task still running from before (discipline 3). Found in
  review before it was run.
- Capacity is not advertised yet: the advert matters only to placement
  across nodes (step 6), which waits on Dannan 0.9.0.

Gate on node-b (Pi 5, Debian 13, g++, Ninja, Release, warnings as
errors, never throttled, Docker 26.1.5, memory controller now on):
`tools/ci.sh release` **133 passed, none skipped**, Docker tests included;
`louhi-node --version` prints 0.1.0. A 25 s run of `louhi-node` on loopback
with its own key: listening, the runtime described with memory, CPU and pid
limits all enforced, ready, two conditions (no user namespaces; the store
not whole), one pass, stopped cleanly, nothing left on the node.

## Store interface and the agent over it (2026-11-15; step 4's first half)

What of step 4 does not wait on a Dagda ref.

- `store/store.h`: the store as Louhi needs it: local reads by key and by
  prefix, asynchronous writes with a deadline, prefix watches as prompts.
  It promises no uniqueness, transaction or linearizable write, because
  Dagda does not.
- `store/memory_store.h`: the store in memory, for tests and a single node
  until the Dagda store exists.
- `agent/store_bindings.h`: `StoreDesired` (the tasks placed on this node
  with the revisions they name; an undecodable record or a missing revision
  is left out and logged) and `StoreReports` (statuses written under each
  status's own key).
- `model/keys.h`: `task_definition_key`, and `tasks_prefix("")` lists every
  project's tasks.

Gate on node-b (Pi 5, Debian 13, g++, Ninja, Release, warnings as
errors, never throttled, no Docker): `tools/ci.sh release` 123 ran, **115
passed, 8 skipped** (the Docker tests, which skip without a daemon; they
passed on node-c at step 3 and nothing they test changed).

## Step 3: the node agent (2026-11-15)

The `louhi::agent` subsystem: what runs a node's tasks.

- `agent/specs.h`: records to the driver's specs. Names and labels tie every
  container and volume to its task; images pinned to their digest; declared
  CPU and memory are a container's own, the rest shared and never nothing.
- `agent/agent.h`: the reconciler. Each pass lists what the runtime has by
  Louhi's labels and acts on the difference with what is placed here:
  launches (pull, volumes, create, start in order), starts what was made but
  not started, removes and remakes a half-made task, stops a task run on its
  own when an essential container exits (kept for inspection, never
  relaunched), relaunches a service's task after a doubling backoff up to a
  budget within a window and then parks it until an operator unparks it
  (discipline 2), retries a failed launch after a backoff with the reason
  reported, tears down what is wanted stopped, and removes what nothing wants
  only once the desired state is whole. It keeps only pacing between passes;
  an agent started over running containers adopts them (disciplines 1 and
  3). Runtime events only prompt a pass; a lost stream is watched again.
  Statuses are reported only on change, with a rising sequence. A node whose
  runtime cannot enforce memory limits launches nothing unless the operator
  allows it.
- `test/fixtures/fake_driver.h`: a runtime in memory, for deterministic tests
  with stepped clocks and no sleep.
- `louhi-node` is not built yet: it becomes a Dannan node with the store
  (the next steps). The real-container gate is a test instead.

Gate on node-c (Pi 5, Debian 13, g++, Ninja, Release, warnings as
errors, never throttled, Docker 26.1.5): `tools/ci.sh release` **118
passed**, first run. Among them `AgentOnDocker.RunsAndStopsARealTask`: a
busybox task placed on the node was launched through Docker, reported
running, exited with 4 and was reported stopped with that reason, then
wanted stopped and removed with its volume; nothing was left on the node. It
ran with the memory override, because the Pi kernel has no memory controller
(`ACTIVE.md`, "Waiting on the maintainer", 0).

## Step 2: the runtime driver (2026-11-15)

The `louhi::runtime` subsystem: the container runtime as the agent sees it,
and the Docker Engine driver.

- `runtime/driver.h`: the `Driver` contract in Louhi's terms (describe,
  pull, volumes, create, start, stop, remove, list, watch), every operation
  with a deadline and the caller's executor, completing once, posted, never
  inline; `Result<T>` with an `Error` kind (unavailable, refused, not found,
  timed out, malformed). `RuntimeInfo` says which limits the node can
  enforce.
- `runtime/labels.h`: the labels and names Louhi gives what it makes, so a
  node re-derives what it runs from the runtime alone.
- `runtime/docker/api.h`: the Docker Engine API (v1.41) as pure functions.
  The hardened profile: Docker's network off, no privilege, all capabilities
  dropped but eight, no new privileges, Docker's seccomp left on, an init,
  private IPC, no restart policy, CPU, memory and process limits always set,
  no swap, volume mounts only, never a host path. Scratch volumes are bounded
  tmpfs. Strict, bounded parsers for every reply, the pull progress stream
  and the event stream (Louhi's containers only).
- `runtime/docker/local_http.h`: HTTP/1.1 over the daemon's local socket
  with Boost.Beast, one connection per request, a deadline on each, bounded
  replies and stream lines, cancellable streams.
- `runtime/docker/docker_driver.h`: the driver; a name or id that goes into
  a request path is checked first, so no caller can change the endpoint.
- Fuzz target `docker_reply` (every reply parser on any bytes). Not yet run
  (owed at the release gate).

Gate on node-c (Pi 5, Debian 13, g++, Ninja, Release, warnings as
errors, never throttled, Docker 26.1.5): `tools/ci.sh release` **96
passed**, the seven driver tests among them run against the real daemon
(pull with digest, the hardened profile as the daemon records it, start,
list by label, the event stream with an exit code, stop, remove, a name in
use refused, a path-changing name refused unsent). Nothing was left behind
on the node (no Louhi containers or volumes; the busybox:1.36 image is kept
as a cache). Hosted CI on the previous push passed every job.

Found on the way, each fixed with its test:
- A complete stream line over its bound slipped through when its newline
  came in the same read (a bug; the test was right).
- The chunked-stream test's data had one chunk size miscounted (the test was
  wrong).
- **The Pi kernel boots with `cgroup_disable=memory`**: Docker on the build
  Pis discards memory limits without an error (`Memory: 0` in the record,
  "WARNING: No memory limit support"). The driver now reports
  `limits_memory`, `limits_swap`, `limits_cpu` and `limits_pids`, and the
  test asserts that a node saying it cannot limit memory indeed records none.
  Fixing the node is the maintainer's (`ACTIVE.md`, "Waiting on the maintainer").
- CodeQL's upload failed on the private repository for want of
  `actions: read` (`ade032c`).

## Step 1: records and codecs (2026-11-15)

The `louhi::model` subsystem: every record of the design's "Records" as a
type with its rules, a versioned codec, and store keys. No I/O.

- `model/names.h`: the name grammar (a DNS label starting with a letter),
  environment names, absolute container paths with no "." or "..".
- `model/image.h`: image references by the Docker distribution grammar,
  strict, printing back exactly as given; sha256 digests only.
- `model/ids.h`: task ids, sixteen random bytes, thirty-two hex digits.
- `model/records.h`: Project, Quota, TaskDefinition (containers, ports,
  environment, mounts, scratch volumes, health checks, run_once), Service,
  Task (with its placement epoch), TaskStatus, NodeStatus, each with
  `validate()` that names the field at fault and never its value.
- `model/codec.h`: each record opens with its kind and format version (1);
  decoders bound every count and length before allocating, accept only
  defined enum and flag bytes, refuse trailing bytes, and validate; encoders
  refuse an invalid record. One spelling per record.
- `model/keys.h`: store keys with a prefix per project and kind; revisions
  sort in order.
- Fuzz targets `record` (every decoder on any bytes: refused, or re-encodes
  to the same bytes) and `image_reference` (refused, or prints back the same
  text). Built only with clang; not yet run (owed at the release gate).

Gate on node-a (Pi 5, Debian 13, g++, Ninja, Release, warnings as
errors): `tools/ci.sh release` **59 passed**, against Dannan 0.8.1.
`tools/ci.sh check` passes. The first run failed one test,
`ImageReference.PathSeparators`: the test was wrong, not the parser (a first
component with a '.' is a domain by the distribution grammar, and '_' is not
in a domain), and was corrected to test what it meant.

Also: hosted CI failed on every job of the first push because
`LOUHI_DANNAN_REPOSITORY` was a CMake PATH cache entry, which turned the
GitHub URL into a directory; it is a STRING now (`2be77b3`).

## Step 0: skeleton (2026-11-15)

The library target, the version header, `louhi_tests`, the hosted CI set
copied from Lir and renamed, the rules files, the plan and the design.
Gate on node-c (Pi 5, Debian 13, g++, Ninja, Release, warnings as
errors, never throttled): `tools/ci.sh release` 2 passed, against Dannan
0.8.1. The private repository `tomdionysus/louhi` was created and `develop`
pushed, at the maintainer's word.
