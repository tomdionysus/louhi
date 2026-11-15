# Louhi - Completed Work

The record, newest first. `ACTIVE.md` is the plan.

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
