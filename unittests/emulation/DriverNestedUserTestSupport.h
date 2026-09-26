//===- DriverNestedUserTestSupport.h - Explicit nested user memory --------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#ifndef NEVERD_EMULATION_TEST_DRIVERNESTEDUSERTESTSUPPORT_H
#define NEVERD_EMULATION_TEST_DRIVERNESTEDUSERTESTSUPPORT_H

#include "fixtures/driver_nested_user.h"
#include "gtest/gtest.h"

#include "neverd/emulation/DriverSession.h"

#include "llvm/ADT/ArrayRef.h"
#include "llvm/Support/Endian.h"

#include <algorithm>
#include <cstddef>

namespace neverd::emulation::nested_user_test {

constexpr llvm::StringLiteral DescriptorID = "descriptor";
constexpr llvm::StringLiteral PayloadID = "payload";
constexpr llvm::StringLiteral ResultID = "sink";

static_assert(sizeof(DriverNestedRequest) == 16);
static_assert(sizeof(DriverNestedBuffer) == 24);

inline std::vector<uint8_t> payload() {
  std::vector<uint8_t> Bytes(NestedBufferSize, NestedGuardByte);
  for (unsigned I = 0; I < NestedPayloadLength; ++I)
    Bytes[NestedPayloadOffset + I] = I + 1;
  return Bytes;
}

inline DriverRequest request(uint32_t Code = NestedUserTransform) {
  DriverRequest IO;
  IO.Kind = DriverRequestKind::DeviceControl;
  IO.ControlCode = Code;
  IO.Input.resize(sizeof(DriverNestedRequest));
  std::vector<uint8_t> Descriptor(sizeof(DriverNestedBuffer));
  llvm::support::endian::write32le(Descriptor.data() +
                                       offsetof(DriverNestedBuffer, Length),
                                   NestedPayloadLength);
  IO.UserBuffers = {{DescriptorID.str(), sizeof(DriverNestedBuffer),
                     std::move(Descriptor), DriverUserPageAccess::ReadOnly},
                    {PayloadID.str(), NestedBufferSize, payload(),
                     DriverUserPageAccess::ReadOnly},
                    {ResultID.str(), NestedBufferSize,
                     std::vector<uint8_t>(NestedBufferSize, NestedGuardByte),
                     DriverUserPageAccess::ReadWrite}};
  IO.UserPointers = {
      {{DriverUserBufferKind::Input, {}, offsetof(DriverNestedRequest, Buffer)},
       {DriverUserBufferKind::Memory, DescriptorID.str(), 0}},
      {{DriverUserBufferKind::Input, {}, offsetof(DriverNestedRequest, Result)},
       {DriverUserBufferKind::Memory, ResultID.str(), NestedResultOffset}},
      {{DriverUserBufferKind::Memory, DescriptorID.str(),
        offsetof(DriverNestedBuffer, Data)},
       {DriverUserBufferKind::Memory, PayloadID.str(), NestedPayloadOffset}},
      {{DriverUserBufferKind::Memory, DescriptorID.str(),
        offsetof(DriverNestedBuffer, Alias)},
       {DriverUserBufferKind::Memory, PayloadID.str(), NestedPayloadOffset}}};
  return IO;
}

inline void expectBacking(const DriverRequestResult &IO, bool Transformed,
                          bool Revoked = false) {
  ASSERT_EQ(IO.UserBuffers.size(), 3u);
  auto Find = [&](llvm::StringRef ID) {
    return std::find_if(IO.UserBuffers.begin(), IO.UserBuffers.end(),
                        [&](const auto &Buffer) { return Buffer.ID == ID; });
  };
  const auto Descriptor = Find(DescriptorID);
  const auto Input = Find(PayloadID);
  const auto Output = Find(ResultID);
  ASSERT_NE(Descriptor, IO.UserBuffers.end());
  ASSERT_NE(Input, IO.UserBuffers.end());
  ASSERT_NE(Output, IO.UserBuffers.end());
  EXPECT_EQ(Input->Backing, payload());
  std::vector<uint8_t> Expected(NestedBufferSize, NestedGuardByte);
  if (Transformed)
    for (unsigned I = 0; I < NestedPayloadLength; ++I)
      Expected[NestedResultOffset + I] = I + 1 + NestedTransformDelta;
  EXPECT_EQ(Output->Backing, Expected);
  ASSERT_EQ(Descriptor->Backing.size(), sizeof(DriverNestedBuffer));
  for (size_t Offset : {offsetof(DriverNestedBuffer, Data),
                        offsetof(DriverNestedBuffer, Alias)})
    EXPECT_EQ(
        llvm::support::endian::read64le(Descriptor->Backing.data() + Offset),
        Input->Address + NestedPayloadOffset);
  EXPECT_EQ(
      llvm::support::endian::read32le(Descriptor->Backing.data() +
                                      offsetof(DriverNestedBuffer, Length)),
      NestedPayloadLength);
  for (const auto &Buffer : IO.UserBuffers) {
    EXPECT_NE(Buffer.Address, 0u);
    EXPECT_EQ(Buffer.Size, Buffer.Backing.size());
    EXPECT_EQ(Buffer.Revoked, Revoked);
  }
  EXPECT_NE(Input->Address, Output->Address);
  EXPECT_EQ(Input->Access, DriverUserPageAccess::ReadOnly);
  EXPECT_EQ(Descriptor->Access, DriverUserPageAccess::ReadOnly);
}

} // namespace neverd::emulation::nested_user_test

#endif // NEVERD_EMULATION_TEST_DRIVERNESTEDUSERTESTSUPPORT_H
