//
// Louhi - The Distributed Compute Layer
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

// The record formats (model/codec.h): each opens with its kind and format
// version and a mismatch of either is refused; decode(encode(r)) == r and
// encode(decode(b)) == b; a decoder refuses truncation, trailing bytes, a
// count or length over its bound, an enum or flag byte the format does not
// define, and a record that does not validate; an invalid record is never
// encoded.

#include <gtest/gtest.h>

#include <initializer_list>
#include <stdexcept>
#include <string>
#include <vector>

#include "louhi/model/codec.h"
#include "louhi/test/fixtures/records.h"

using namespace louhi::model;
namespace fixture = louhi::test;

namespace {

template <class Record, class Decode>
void expect_round_trip(const Record& record, Decode decode) {
  const Bytes bytes = encode(record);
  const Record back = decode(bytes);
  EXPECT_EQ(back, record);
  EXPECT_EQ(encode(back), bytes);
}

// Every proper prefix of a record's bytes is refused.
template <class Decode>
void expect_truncation_refused(const Bytes& bytes, Decode decode) {
  for (size_t size = 0; size < bytes.size(); ++size) {
    const std::span<const uint8_t> prefix(bytes.data(), size);
    EXPECT_THROW(decode(prefix), DecodeError) << "prefix of " << size << " of " << bytes.size();
  }
}

// Writes a record by hand, for bytes encode() would never make.
class Raw {
 public:
  Raw& u8(uint8_t v) { return (_w.u8(v), *this); }
  Raw& u16(uint16_t v) { return (_w.u16(v), *this); }
  Raw& u32(uint32_t v) { return (_w.u32(v), *this); }
  Raw& u64(uint64_t v) { return (_w.u64(v), *this); }
  Raw& str(const std::string& v) { return (_w.string(v), *this); }
  Bytes take() { return _w.take(); }

 private:
  dannan::net::Writer _w;
};

}  // namespace

TEST(Codec, EveryKindRoundTrips) {
  expect_round_trip(fixture::project(), decode_project);
  expect_round_trip(fixture::quota(), decode_quota);
  expect_round_trip(fixture::task_definition(), decode_task_definition);
  expect_round_trip(fixture::service(), decode_service);
  expect_round_trip(fixture::task(), decode_task);
  expect_round_trip(fixture::task_status(), decode_task_status);
  expect_round_trip(fixture::node_status(), decode_node_status);
}

TEST(Codec, OptionalPartsRoundTripEmpty) {
  auto d = fixture::task_definition();
  d.containers[0].health_check.reset();
  d.containers[0].image_digest.clear();
  d.volumes.clear();
  for (auto& c : d.containers) c.mounts.clear();
  expect_round_trip(d, decode_task_definition);

  auto t = fixture::task();
  t.node.reset();
  t.placement_epoch = 0;
  t.service.clear();
  expect_round_trip(t, decode_task);

  auto s = fixture::task_status();
  s.containers.clear();
  expect_round_trip(s, decode_task_status);
}

TEST(Codec, VolumeKindsAndEphemeralStorageRoundTrip) {
  auto d = fixture::task_definition();
  d.ephemeral_storage_mib = 123'456;
  d.volumes.push_back({"cache", VolumeKind::disk, 50'000});
  expect_round_trip(d, decode_task_definition);
  EXPECT_EQ(decode_task_definition(encode(d)).volumes[1].kind, VolumeKind::disk);
}

TEST(Codec, AVolumeKindTheFormatDoesNotDefineIsRefused) {
  auto d = fixture::task_definition();
  d.containers.resize(1);
  d.containers[0].mounts.clear();
  d.volumes = {{"v", VolumeKind::disk, 1}};
  Bytes bytes = encode(d);
  // A definition ends: volume name, kind (u8), size (u32), run_once (u8),
  // registered (u64).
  const size_t kind = bytes.size() - 8 - 1 - 4 - 1;
  ASSERT_EQ(bytes[kind], static_cast<uint8_t>(VolumeKind::disk));
  for (const uint8_t bad : std::initializer_list<uint8_t>{0, 3, 255}) {
    bytes[kind] = bad;
    EXPECT_THROW(decode_task_definition(bytes), DecodeError) << int(bad);
  }
}

TEST(Codec, NegativeExitCodesSurvive) {
  auto s = fixture::task_status();
  s.containers[1].exit_code = -2147483647 - 1;
  expect_round_trip(s, decode_task_status);
}

TEST(Codec, RecordsOpenWithKindAndVersion) {
  const Bytes bytes = encode(fixture::service());
  ASSERT_GE(bytes.size(), 2u);
  EXPECT_EQ(bytes[0], static_cast<uint8_t>(Kind::service));
  EXPECT_EQ(bytes[1], format_version);
  EXPECT_EQ(peek_kind(bytes), Kind::service);
  EXPECT_EQ(peek_kind(encode(fixture::node_status())), Kind::node_status);
}

TEST(Codec, PeekRefusesUnknownKindsAndShortBytes) {
  EXPECT_THROW(peek_kind(Bytes{}), DecodeError);
  EXPECT_THROW(peek_kind(Bytes{1}), DecodeError);
  EXPECT_THROW(peek_kind(Bytes{0, 1}), DecodeError);
  // The first kind past those this build knows (the network records took 8
  // to 11).
  EXPECT_THROW(peek_kind(Bytes{12, 1}), DecodeError);
  EXPECT_THROW(peek_kind(Bytes{255, 1}), DecodeError);
}

TEST(Codec, AKindIsNeverReadAsAnother) {
  const Bytes project = encode(fixture::project());
  EXPECT_THROW(decode_quota(project), DecodeError);
  EXPECT_THROW(decode_service(encode(fixture::task())), DecodeError);
}

TEST(Codec, AnotherVersionIsRefused) {
  Bytes bytes = encode(fixture::project());
  bytes[1] = format_version + 1;
  EXPECT_THROW(decode_project(bytes), DecodeError);
  bytes[1] = 0;
  EXPECT_THROW(decode_project(bytes), DecodeError);
}

TEST(Codec, TruncationIsRefused) {
  expect_truncation_refused(encode(fixture::project()), decode_project);
  expect_truncation_refused(encode(fixture::quota()), decode_quota);
  expect_truncation_refused(encode(fixture::task_definition()), decode_task_definition);
  expect_truncation_refused(encode(fixture::service()), decode_service);
  expect_truncation_refused(encode(fixture::task()), decode_task);
  expect_truncation_refused(encode(fixture::task_status()), decode_task_status);
  expect_truncation_refused(encode(fixture::node_status()), decode_node_status);
}

TEST(Codec, TrailingBytesAreRefused) {
  Bytes bytes = encode(fixture::task());
  bytes.push_back(0);
  EXPECT_THROW(decode_task(bytes), DecodeError);
}

TEST(Codec, FlagBytesAreZeroOrOne) {
  // A service ends: spread flag (u8), updated (u64).
  Bytes bytes = encode(fixture::service());
  bytes[bytes.size() - 9] = 2;
  EXPECT_THROW(decode_service(bytes), DecodeError);
}

TEST(Codec, EnumBytesAreOnesTheFormatDefines) {
  auto t = fixture::task();
  t.node.reset();
  t.placement_epoch = 0;
  t.service.clear();
  Bytes bytes = encode(t);
  // kind, version, id (16), project, family, revision, service, then desired.
  const size_t desired = 2 + 16 + (4 + t.project.size()) + (4 + t.family.size()) + 4 + 4;
  ASSERT_EQ(bytes[desired], static_cast<uint8_t>(DesiredState::running));
  for (const uint8_t bad : std::initializer_list<uint8_t>{0, 3, 255}) {
    bytes[desired] = bad;
    EXPECT_THROW(decode_task(bytes), DecodeError) << int(bad);
  }
}

TEST(Codec, CountsOverTheirBoundAreRefusedBeforeAllocating) {
  // A node status claiming four billion conditions, with no bytes behind it.
  const Bytes bytes = Raw()
                          .u8(static_cast<uint8_t>(Kind::node_status))
                          .u8(format_version)
                          .u64(0)
                          .u64(0)  // node id, sixteen bytes
                          .u64(1)
                          .str("0.1.0")
                          .str("docker")
                          .str("26")
                          .u32(1)
                          .u32(1)
                          .u32(0)
                          .u32(0)
                          .u32(0)
                          .u8(1)
                          .u32(0xffffffff)
                          .take();
  EXPECT_THROW(decode_node_status(bytes), DecodeError);
}

TEST(Codec, LengthsOverTheirBoundAreRefused) {
  const Bytes bytes = Raw().u8(static_cast<uint8_t>(Kind::project)).u8(format_version).str(std::string(64, 'a')).str("").u64(0).take();
  EXPECT_THROW(decode_project(bytes), DecodeError);
}

TEST(Codec, AnInvalidRecordIsNeitherDecodedNorEncoded) {
  // Well-formed bytes for a project whose name is not a name.
  const Bytes bytes = Raw().u8(static_cast<uint8_t>(Kind::project)).u8(format_version).str("Web").str("").u64(0).take();
  EXPECT_THROW(decode_project(bytes), DecodeError);

  auto p = fixture::project();
  p.name = "Web";
  EXPECT_THROW(encode(p), std::invalid_argument);
}
