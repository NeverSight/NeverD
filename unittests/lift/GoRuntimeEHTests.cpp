//===- GoRuntimeEHTests.cpp - Go runtime frame metadata tests -------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// Coverage for the Go `pclntab` decoder: the pc-value tables every per-PC
/// fact in a Go image is encoded in, the pointer maps the collector reads a
/// frame with, and the Go 1.2 table layout that predates the offset block the
/// modern one opens with.
///
/// Every table here is assembled byte by byte rather than compiled, because
/// the layouts that matter most are the ones no toolchain on this machine can
/// still emit -- the Go 1.2 magic covers releases from 2013 to 2020 -- and
/// because a decoder is only as good as its behaviour on bytes a linker would
/// never write.
///
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "neverd/loader/Go/GoRuntimeEH.h"

#include <array>
#include <cstring>
#include <optional>
#include <string>
#include <vector>

using namespace neverd;
using namespace neverd::go_loader;

namespace {

//===----------------------------------------------------------------------===//
// Byte-buffer builder
//===----------------------------------------------------------------------===//

/// Assembles the exact byte layouts the decoder reads.  Building the tables
/// here rather than compiling a fixture keeps every encoding branch reachable,
/// including the malformed ones no real toolchain emits.
class ByteBuilder {
public:
  void u8(uint8_t V) { Bytes.push_back(V); }
  void u32(uint32_t V) { append(&V, sizeof(V)); }
  void u64(uint64_t V) { append(&V, sizeof(V)); }

  /// The unsigned varint `runtime.readvarint` decodes.
  void varint(uint32_t V) {
    do {
      uint8_t Byte = V & 0x7f;
      V >>= 7;
      if (V)
        Byte |= 0x80;
      Bytes.push_back(Byte);
    } while (V);
  }

  void str(const std::string &S) {
    Bytes.insert(Bytes.end(), S.begin(), S.end());
    Bytes.push_back(0);
  }

  size_t size() const { return Bytes.size(); }
  const std::vector<uint8_t> &data() const { return Bytes; }

private:
  void append(const void *P, size_t N) {
    const auto *B = static_cast<const uint8_t *>(P);
    Bytes.insert(Bytes.end(), B, B + N);
  }
  std::vector<uint8_t> Bytes;
};

//===----------------------------------------------------------------------===//
// Image
//===----------------------------------------------------------------------===//

constexpr va_t kTextVA = 0x400000;
constexpr size_t kTextSize = 0x2000;
constexpr va_t kDataVA = 0x500000;
constexpr size_t kDataSize = 0x8000;
/// The `pclntab` goes at the start of the data segment; funcdata payloads,
/// which it addresses by relocated pointer, go well past whatever it needs.
constexpr va_t kPclnVA = kDataVA;
constexpr va_t kPayloadVA = kDataVA + 0x4000;

constexpr uint32_t kGo12Magic = 0xFFFFFFFBu;
constexpr uint32_t kGo116Magic = 0xFFFFFFFAu;

/// A 64-bit image with one executable segment and one data segment, plus a
/// cursor for the funcdata payloads the tables point at.
struct GoTestImage {
  BinaryImage Img;
  va_t PayloadCursor = kPayloadVA;

  explicit GoTestImage(Arch A = Arch::X64) {
    Img.Arch = A;
    Img.Format = BinaryFormat::ELF;
    Img.Bits = Bitness::Bits64;
    Img.Base = kTextVA;
    Img.Entry = kTextVA;

    Segment Text;
    Text.Name = ".text";
    Text.VA = kTextVA;
    Text.Size = kTextSize;
    Text.Flags = SegmentFlags::Readable | SegmentFlags::Executable;
    Text.Data.assign(kTextSize, 0x90);
    Img.Segments.push_back(std::move(Text));

    Segment Data;
    Data.Name = ".data";
    Data.VA = kDataVA;
    Data.Size = kDataSize;
    Data.Flags = SegmentFlags::Readable;
    Data.Data.assign(kDataSize, 0);
    Img.Segments.push_back(std::move(Data));
  }

  va_t addPayload(const std::vector<uint8_t> &Bytes) {
    const va_t VA = PayloadCursor;
    EXPECT_TRUE(Img.writeVA(VA, Bytes.data(), Bytes.size()));
    PayloadCursor = (PayloadCursor + Bytes.size() + 7) & ~va_t(7);
    return VA;
  }

  void installPclnTab(const std::vector<uint8_t> &Bytes) {
    ASSERT_LE(Bytes.size(), kPayloadVA - kPclnVA);
    ASSERT_TRUE(Img.writeVA(kPclnVA, Bytes.data(), Bytes.size()));
  }

  void writeText(va_t VA, const std::vector<uint8_t> &Bytes) {
    ASSERT_TRUE(Img.writeVA(VA, Bytes.data(), Bytes.size()));
  }
};

/// An `E8 rel32` call, which is the form a Go body reaches a runtime helper by
/// on x86-64.
std::vector<uint8_t> makeCall(va_t SiteVA, va_t TargetVA) {
  const int32_t Rel = static_cast<int32_t>(static_cast<int64_t>(TargetVA) -
                                           static_cast<int64_t>(SiteVA + 5));
  std::vector<uint8_t> Bytes(5, 0);
  Bytes[0] = 0xE8;
  std::memcpy(Bytes.data() + 1, &Rel, sizeof(Rel));
  return Bytes;
}

//===----------------------------------------------------------------------===//
// pclntab assembly
//===----------------------------------------------------------------------===//

/// A pc-value table given as the pairs it decodes to: the value in effect and
/// how many `MinLC` units it stays in effect for.
///
/// Go's encoder never emits the same value twice in a row and neither may this
/// one, because a zero value delta after the first pair is how the encoding
/// spells the end of the table.
struct PCValueTable {
  struct Step {
    int32_t Value = 0;
    uint32_t PCUnits = 0;
  };
  std::vector<Step> Steps;
  /// Emitted verbatim in place of \ref Steps when set, for the encodings a
  /// well-formed step list cannot express.
  std::vector<uint8_t> RawBytes;

  bool empty() const { return Steps.empty() && RawBytes.empty(); }
};

void appendPCValueTable(ByteBuilder &B, const PCValueTable &T) {
  if (!T.RawBytes.empty()) {
    for (uint8_t Byte : T.RawBytes)
      B.u8(Byte);
    return;
  }
  int32_t Previous = -1;
  for (const PCValueTable::Step &S : T.Steps) {
    const int32_t Delta = S.Value - Previous;
    Previous = S.Value;
    B.varint((static_cast<uint32_t>(Delta) << 1) ^
             static_cast<uint32_t>(Delta >> 31));
    B.varint(S.PCUnits);
  }
  B.u8(0);
}

struct GoFuncSpec {
  std::string Name;
  va_t EntryVA = 0;
  uint32_t DeferReturn = 0;
  uint8_t FuncID = 0;
  uint8_t Flag = 0;
  PCValueTable PcSP;
  /// pcdata tables by index; an entry left unset gets offset 0, which is how a
  /// record spells "this function has no such table".
  std::vector<std::optional<PCValueTable>> PCData;
  /// funcdata payload addresses by index; an entry left unset gets the null
  /// pointer the pointer-shaped array uses for an absent entry.
  std::vector<std::optional<va_t>> FuncData;
};

struct BuiltPclnTab {
  std::vector<uint8_t> Bytes;
  /// Blob-relative offsets, so that a test can corrupt one field of one record
  /// without rebuilding the table around it.
  std::vector<size_t> RecordOffsets;
  std::vector<size_t> PCDataArrayOffsets;
  std::vector<size_t> FuncDataArrayOffsets;

  void put8(size_t Offset, uint8_t V) { Bytes[Offset] = V; }
  void put32(size_t Offset, uint32_t V) {
    std::memcpy(Bytes.data() + Offset, &V, sizeof(V));
  }
  void put64(size_t Offset, uint64_t V) {
    std::memcpy(Bytes.data() + Offset, &V, sizeof(V));
  }
};

/// Assemble a whole `pclntab` for \p Magic.
///
/// \p EndVA closes the last function, which the table states as the entry of a
/// sentinel `functab` element rather than as a size.
///
/// \p PreGo112Record selects the older of the two `_func` shapes the Go 1.2
/// magic covers: `nfuncdata` as a whole word, and no `deferreturn` or
/// `funcID` at all.
BuiltPclnTab buildPclnTab(uint32_t Magic, const std::vector<GoFuncSpec> &Funcs,
                          va_t EndVA, uint8_t MinLC = 1,
                          bool PreGo112Record = false) {
  constexpr size_t PtrSize = 8;
  const bool Is12 = Magic == kGo12Magic;
  const size_t N = Funcs.size();
  // `_func` up to and including `nfuncdata`, i.e. where the pcdata array
  // starts.  Go 1.16 added `cuOffset` ahead of it.
  const size_t FixedSize = PtrSize + (Is12 ? 32 : 36);
  const size_t HeaderSize = Is12 ? 16 : 64;
  auto align8 = [](size_t V) { return (V + 7) & ~size_t(7); };

  // Offset zero of `pctab` is reserved: a record spells an absent table as
  // offset zero, so no table may start there.
  ByteBuilder PcTab;
  PcTab.u8(0);
  std::vector<uint32_t> PcSPOffsets;
  std::vector<std::vector<uint32_t>> PCDataOffsets;
  for (const GoFuncSpec &F : Funcs) {
    if (F.PcSP.empty()) {
      PcSPOffsets.push_back(0);
    } else {
      PcSPOffsets.push_back(static_cast<uint32_t>(PcTab.size()));
      appendPCValueTable(PcTab, F.PcSP);
    }
    std::vector<uint32_t> Offsets;
    for (const std::optional<PCValueTable> &T : F.PCData) {
      if (!T) {
        Offsets.push_back(0);
        continue;
      }
      Offsets.push_back(static_cast<uint32_t>(PcTab.size()));
      appendPCValueTable(PcTab, *T);
    }
    PCDataOffsets.push_back(std::move(Offsets));
  }

  ByteBuilder Names;
  Names.u8(0);
  std::vector<uint32_t> NameOffsets;
  for (const GoFuncSpec &F : Funcs) {
    NameOffsets.push_back(static_cast<uint32_t>(Names.size()));
    Names.str(F.Name);
  }

  // The Go 1.2 functab sits immediately behind the function count, so it is
  // placed before everything the modern layout is free to order at will.
  size_t Cursor = HeaderSize;
  size_t FuncTabOff = 0;
  size_t FileTabOff = 0;
  size_t CuTabOff = 0;
  if (Is12) {
    FuncTabOff = Cursor;
    Cursor += (2 * N + 1) * PtrSize + 4;
    FileTabOff = Cursor;
    Cursor = align8(Cursor + 4);
  }
  const size_t NamesOff = Cursor;
  Cursor += Names.size();
  const size_t PcTabOff = Cursor;
  Cursor += PcTab.size();
  if (!Is12) {
    CuTabOff = Cursor;
    Cursor += 4;
    FileTabOff = Cursor;
    Cursor += 1;
    Cursor = align8(Cursor);
    FuncTabOff = Cursor;
    Cursor += (N + 1) * 2 * PtrSize;
  }

  BuiltPclnTab Out;
  for (const GoFuncSpec &F : Funcs) {
    Cursor = align8(Cursor);
    Out.RecordOffsets.push_back(Cursor);
    const size_t PCDataArray = Cursor + FixedSize;
    Out.PCDataArrayOffsets.push_back(PCDataArray);
    // `runtime.funcdata` rounds the pointer array up to a pointer boundary,
    // and does so on the absolute address; keeping every record eight-aligned
    // makes the blob offset agree with it.
    size_t FuncDataArray = PCDataArray + F.PCData.size() * 4;
    if (FuncDataArray % PtrSize != 0)
      FuncDataArray += 4;
    Out.FuncDataArrayOffsets.push_back(FuncDataArray);
    Cursor = FuncDataArray + F.FuncData.size() * PtrSize;
  }
  Out.Bytes.assign(align8(Cursor), 0);

  Out.put32(0, Magic);
  Out.put8(6, MinLC);
  Out.put8(7, static_cast<uint8_t>(PtrSize));
  Out.put64(8, N);
  if (!Is12) {
    Out.put64(16, 0); // nfiles
    Out.put64(24, NamesOff);
    Out.put64(32, CuTabOff);
    Out.put64(40, FileTabOff);
    Out.put64(48, PcTabOff);
    Out.put64(56, FuncTabOff);
  }

  for (size_t I = 0; I < Names.size(); ++I)
    Out.put8(NamesOff + I, Names.data()[I]);
  for (size_t I = 0; I < PcTab.size(); ++I)
    Out.put8(PcTabOff + I, PcTab.data()[I]);

  // Every offset a Go 1.2 record holds is measured from the head of the whole
  // table rather than from the sub-table it names.
  const size_t NameAdjust = Is12 ? NamesOff : 0;
  const size_t PcAdjust = Is12 ? PcTabOff : 0;
  const size_t FuncOffBase = Is12 ? 0 : FuncTabOff;

  for (size_t I = 0; I < N; ++I) {
    const size_t Slot = FuncTabOff + I * 2 * PtrSize;
    Out.put64(Slot, Funcs[I].EntryVA);
    Out.put64(Slot + PtrSize, Out.RecordOffsets[I] - FuncOffBase);
  }
  Out.put64(FuncTabOff + N * 2 * PtrSize, EndVA);
  if (Is12) {
    Out.put32(FuncTabOff + (2 * N + 1) * PtrSize,
              static_cast<uint32_t>(FileTabOff));
    Out.put32(FileTabOff, 0); // nfiletab
  }

  for (size_t I = 0; I < N; ++I) {
    const GoFuncSpec &F = Funcs[I];
    const size_t O = Out.RecordOffsets[I];
    Out.put64(O, F.EntryVA);
    Out.put32(O + 8, static_cast<uint32_t>(NameAdjust + NameOffsets[I]));
    Out.put32(O + 12, 0); // args
    Out.put32(O + 16, F.DeferReturn);
    Out.put32(O + 20, PcSPOffsets[I] == 0
                          ? 0
                          : static_cast<uint32_t>(PcAdjust + PcSPOffsets[I]));
    Out.put32(O + 24, 0); // pcfile
    Out.put32(O + 28, 0); // pcln
    Out.put32(O + 32, static_cast<uint32_t>(F.PCData.size()));
    if (Is12 && PreGo112Record) {
      Out.put32(O + 36, static_cast<uint32_t>(F.FuncData.size()));
    } else if (Is12) {
      Out.put8(O + 36, F.FuncID);
      Out.put8(O + 39, static_cast<uint8_t>(F.FuncData.size()));
    } else {
      Out.put32(O + 36, 0); // cuOffset
      Out.put8(O + 40, F.FuncID);
      Out.put8(O + 41, F.Flag);
      Out.put8(O + 43, static_cast<uint8_t>(F.FuncData.size()));
    }
    for (size_t J = 0; J < PCDataOffsets[I].size(); ++J)
      Out.put32(Out.PCDataArrayOffsets[I] + J * 4,
                PCDataOffsets[I][J] == 0
                    ? 0
                    : static_cast<uint32_t>(PcAdjust + PCDataOffsets[I][J]));
    for (size_t J = 0; J < F.FuncData.size(); ++J)
      Out.put64(Out.FuncDataArrayOffsets[I] + J * PtrSize,
                F.FuncData[J].value_or(0));
  }
  return Out;
}

/// A `runtime.stackmap`: `n int32`, `nbit int32`, then `n` bitmaps each padded
/// out to `(nbit+7)/8` bytes.
std::vector<uint8_t>
buildStackMap(int32_t BitCount,
              const std::vector<std::vector<uint8_t>> &Bitmaps) {
  ByteBuilder B;
  B.u32(static_cast<uint32_t>(Bitmaps.size()));
  B.u32(static_cast<uint32_t>(BitCount));
  const size_t Stride = static_cast<size_t>((BitCount + 7) / 8);
  for (const std::vector<uint8_t> &Bitmap : Bitmaps) {
    EXPECT_LE(Bitmap.size(), Stride);
    for (size_t I = 0; I < Stride; ++I)
      B.u8(I < Bitmap.size() ? Bitmap[I] : 0);
  }
  return B.data();
}

/// `FUNCDATA_OpenCodedDeferInfo` as Go 1.22 and later spell it: the distance
/// below varp of the defer bitmask byte, then of the first closure slot.  The
/// rest of the run follows that slot and the record does not count them.
std::vector<uint8_t> buildContiguousDeferInfo(uint32_t DeferBits,
                                              uint32_t Slots) {
  ByteBuilder B;
  B.varint(DeferBits);
  B.varint(Slots);
  return B.data();
}

/// The same funcdata as Go 1.18 through 1.21 spelled it: the bitmask offset,
/// how many closure slots the frame has, and then each of them, from the
/// last-declared defer to the first.  The compiler was free to place them
/// anywhere, which is why they had to be listed.
std::vector<uint8_t> buildEnumeratedDeferInfo(uint32_t DeferBits,
                                              const std::vector<uint32_t> &Slots) {
  ByteBuilder B;
  B.varint(DeferBits);
  B.varint(static_cast<uint32_t>(Slots.size()));
  for (uint32_t Slot : Slots)
    B.varint(Slot);
  return B.data();
}

/// One defer as Go 1.14 through 1.17 described it, back when a deferred call
/// could still take arguments: how wide its argument frame is, where its
/// closure sits, and where each argument is stored, how wide it is, and where
/// the call wants it.
struct LegacyDefer {
  uint32_t ArgWidth = 0;
  uint32_t ClosureOffset = 0;
  std::vector<std::array<uint32_t, 3>> Arguments;
};

/// The pre-Go 1.18 record, which leads with the largest argument frame any of
/// the defers needs.  That leading word is what shifts every field after it,
/// so a reader that does not expect it misreads the whole record.
std::vector<uint8_t>
buildLegacyEnumeratedDeferInfo(uint32_t MaxArgSize, uint32_t DeferBits,
                               const std::vector<LegacyDefer> &Defers) {
  ByteBuilder B;
  B.varint(MaxArgSize);
  B.varint(DeferBits);
  B.varint(static_cast<uint32_t>(Defers.size()));
  for (const LegacyDefer &Defer : Defers) {
    B.varint(Defer.ArgWidth);
    B.varint(Defer.ClosureOffset);
    B.varint(static_cast<uint32_t>(Defer.Arguments.size()));
    for (const std::array<uint32_t, 3> &Argument : Defer.Arguments)
      for (uint32_t Word : Argument)
        B.varint(Word);
  }
  return B.data();
}

//===----------------------------------------------------------------------===//
// Assertion helpers
//===----------------------------------------------------------------------===//

const ExceptionFunction *findRecord(const ExceptionInfo &Info, va_t EntryVA) {
  for (const ExceptionFunction &F : Info.Functions)
    if (F.Go && F.Go->EntryVA == EntryVA)
      return &F;
  return nullptr;
}

bool anyDiagnosticContains(const std::vector<std::string> &Diagnostics,
                           const std::string &Needle) {
  for (const std::string &D : Diagnostics)
    if (D.find(Needle) != std::string::npos)
      return true;
  return false;
}

/// A function that has exceptional control flow purely because it declares a
/// `deferreturn`, which is the cheapest way to make the decoder keep a record.
GoFuncSpec makeDeferringFunc(const std::string &Name, va_t EntryVA) {
  GoFuncSpec F;
  F.Name = Name;
  F.EntryVA = EntryVA;
  F.DeferReturn = 0x20;
  F.PcSP.Steps = {{0x30, 0x80}};
  return F;
}

//===----------------------------------------------------------------------===//
// pc-value decoding
//===----------------------------------------------------------------------===//

TEST(GoPCValue, DecodesUnsafePointRangesWithEveryDefinedKind) {
  GoTestImage T;
  GoFuncSpec Work = makeDeferringFunc("main.work", kTextVA + 0x100);
  PCValueTable UnsafePoints;
  UnsafePoints.Steps = {{-1, 0x10}, {-2, 0x08}, {-3, 0x04},
                        {-4, 0x04}, {-5, 0x04}, {7, 0x08}};
  Work.PCData = {UnsafePoints};
  T.installPclnTab(
      buildPclnTab(kGo116Magic, {Work}, kTextVA + 0x200).Bytes);

  parseGoExceptions(T.Img);

  const ExceptionFunction *F = findRecord(T.Img.ExceptionMetadata,
                                          kTextVA + 0x100);
  ASSERT_NE(F, nullptr);
  const std::vector<GoUnsafePointRange> &Ranges = F->Go->UnsafePointRanges;
  ASSERT_EQ(Ranges.size(), 6u);

  EXPECT_EQ(Ranges[0].Range.Begin, kTextVA + 0x100);
  EXPECT_EQ(Ranges[0].Range.End, kTextVA + 0x110);
  EXPECT_EQ(Ranges[0].Kind, GoUnsafePointKind::Safe);
  EXPECT_EQ(Ranges[0].NativeValue, -1);

  EXPECT_EQ(Ranges[1].Range.Begin, kTextVA + 0x110);
  EXPECT_EQ(Ranges[1].Range.End, kTextVA + 0x118);
  EXPECT_EQ(Ranges[1].Kind, GoUnsafePointKind::Unsafe);

  // Both restart spellings normalize together, and only the native value
  // still tells them apart.
  EXPECT_EQ(Ranges[2].Kind, GoUnsafePointKind::RestartSequence);
  EXPECT_EQ(Ranges[2].NativeValue, -3);
  EXPECT_EQ(Ranges[3].Kind, GoUnsafePointKind::RestartSequence);
  EXPECT_EQ(Ranges[3].NativeValue, -4);

  EXPECT_EQ(Ranges[4].Kind, GoUnsafePointKind::RestartAtEntry);
  EXPECT_EQ(Ranges[5].Kind, GoUnsafePointKind::Unknown);
  EXPECT_EQ(Ranges[5].NativeValue, 7);
  EXPECT_EQ(Ranges[5].Range.End, kTextVA + 0x12c);
}

TEST(GoPCValue, ScalesProgramCounterDeltasByMinLC) {
  GoTestImage T(Arch::AArch64);
  GoFuncSpec Work = makeDeferringFunc("main.work", kTextVA + 0x100);
  PCValueTable UnsafePoints;
  UnsafePoints.Steps = {{-1, 4}, {-2, 2}};
  Work.PCData = {UnsafePoints};
  T.installPclnTab(
      buildPclnTab(kGo116Magic, {Work}, kTextVA + 0x200, /*MinLC=*/4).Bytes);

  parseGoExceptions(T.Img);

  const ExceptionFunction *F = findRecord(T.Img.ExceptionMetadata,
                                          kTextVA + 0x100);
  ASSERT_NE(F, nullptr);
  ASSERT_EQ(F->Go->UnsafePointRanges.size(), 2u);
  EXPECT_EQ(F->Go->UnsafePointRanges[0].Range.End, kTextVA + 0x110);
  EXPECT_EQ(F->Go->UnsafePointRanges[1].Range.End, kTextVA + 0x118);
}

TEST(GoPCValue, ReportsATableThatRunsOffTheImage) {
  GoTestImage T;
  GoFuncSpec Work = makeDeferringFunc("main.work", kTextVA + 0x100);
  PCValueTable UnsafePoints;
  UnsafePoints.Steps = {{-2, 0x10}};
  Work.PCData = {UnsafePoints};
  BuiltPclnTab Tab = buildPclnTab(kGo116Magic, {Work}, kTextVA + 0x200);
  // Point the table past everything the image maps.  The offset is still a
  // number the record could hold, so only the read can reject it.
  Tab.put32(Tab.PCDataArrayOffsets[0], 0x00F00000);
  T.installPclnTab(Tab.Bytes);

  parseGoExceptions(T.Img);

  const ExceptionFunction *F = findRecord(T.Img.ExceptionMetadata,
                                          kTextVA + 0x100);
  ASSERT_NE(F, nullptr);
  EXPECT_TRUE(F->Go->UnsafePointRanges.empty());
  EXPECT_EQ(F->ParseStatus, ExceptionParseStatus::Partial);
  EXPECT_TRUE(anyDiagnosticContains(F->Diagnostics,
                                    "not a readable pc-value table"));
}

TEST(GoPCValue, RejectsAValueDeltaThatOverflowsTheAccumulator) {
  GoTestImage T;
  GoFuncSpec Work = makeDeferringFunc("main.work", kTextVA + 0x100);
  // Two maximal positive deltas in a row take the running value past what an
  // int32 holds, which no real table does and which must not wrap.
  ByteBuilder Raw;
  for (unsigned I = 0; I < 2; ++I) {
    Raw.varint(0xFFFFFFFEu); // zigzag(INT32_MAX)
    Raw.varint(4);
  }
  Raw.u8(0);
  PCValueTable UnsafePoints;
  UnsafePoints.RawBytes = Raw.data();
  Work.PCData = {UnsafePoints};
  T.installPclnTab(
      buildPclnTab(kGo116Magic, {Work}, kTextVA + 0x200).Bytes);

  parseGoExceptions(T.Img);

  const ExceptionFunction *F = findRecord(T.Img.ExceptionMetadata,
                                          kTextVA + 0x100);
  ASSERT_NE(F, nullptr);
  EXPECT_TRUE(F->Go->UnsafePointRanges.empty());
  EXPECT_EQ(F->ParseStatus, ExceptionParseStatus::Partial);
}

TEST(GoPCValue, RejectsAnUnterminatedVarint) {
  GoTestImage T;
  GoFuncSpec Work = makeDeferringFunc("main.work", kTextVA + 0x100);
  PCValueTable UnsafePoints;
  UnsafePoints.RawBytes = {0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x00};
  Work.PCData = {UnsafePoints};
  T.installPclnTab(
      buildPclnTab(kGo116Magic, {Work}, kTextVA + 0x200).Bytes);

  parseGoExceptions(T.Img);

  const ExceptionFunction *F = findRecord(T.Img.ExceptionMetadata,
                                          kTextVA + 0x100);
  ASSERT_NE(F, nullptr);
  EXPECT_TRUE(F->Go->UnsafePointRanges.empty());
  EXPECT_EQ(F->ParseStatus, ExceptionParseStatus::Partial);
}

TEST(GoPCValue, LeavesUnsafePointsEmptyWhenTheRecordDeclaresNoTable) {
  GoTestImage T;
  GoFuncSpec Work = makeDeferringFunc("main.work", kTextVA + 0x100);
  Work.PCData = {std::nullopt};
  T.installPclnTab(
      buildPclnTab(kGo116Magic, {Work}, kTextVA + 0x200).Bytes);

  parseGoExceptions(T.Img);

  const ExceptionFunction *F = findRecord(T.Img.ExceptionMetadata,
                                          kTextVA + 0x100);
  ASSERT_NE(F, nullptr);
  EXPECT_TRUE(F->Go->UnsafePointRanges.empty());
  EXPECT_EQ(F->ParseStatus, ExceptionParseStatus::Complete);
}

//===----------------------------------------------------------------------===//
// Stack maps
//===----------------------------------------------------------------------===//

TEST(GoStackMaps, DecodesBothPointerMapsAndTheirBitmaps) {
  GoTestImage T;
  const va_t ArgsVA = T.addPayload(buildStackMap(3, {{0b101}, {0b010}}));
  const va_t LocalsVA =
      T.addPayload(buildStackMap(9, {{0x01, 0x01}, {0xFF, 0x00}}));

  GoFuncSpec Work = makeDeferringFunc("main.work", kTextVA + 0x100);
  PCValueTable StackMapIndex;
  StackMapIndex.Steps = {{0, 0x40}, {1, 0x40}};
  Work.PCData = {std::nullopt, StackMapIndex};
  Work.FuncData = {ArgsVA, LocalsVA};
  T.installPclnTab(
      buildPclnTab(kGo116Magic, {Work}, kTextVA + 0x200).Bytes);

  parseGoExceptions(T.Img);

  const ExceptionInfo &Info = T.Img.ExceptionMetadata;
  ASSERT_TRUE(Info.GoModule.has_value());
  // From Go 1.16 the position is fixed by the magic and nothing is probed.
  ASSERT_TRUE(Info.GoModule->StackMapPCDataIndex.has_value());
  EXPECT_EQ(*Info.GoModule->StackMapPCDataIndex, 1u);

  const ExceptionFunction *F = findRecord(Info, kTextVA + 0x100);
  ASSERT_NE(F, nullptr);
  EXPECT_EQ(F->ParseStatus, ExceptionParseStatus::Complete);

  ASSERT_TRUE(F->Go->ArgsPointerMap.has_value());
  EXPECT_EQ(F->Go->ArgsPointerMap->RecordVA, ArgsVA);
  EXPECT_EQ(F->Go->ArgsPointerMap->BitCount, 3u);
  ASSERT_EQ(F->Go->ArgsPointerMap->Bitmaps.size(), 2u);
  const GoStackMapBitmap &Args0 = F->Go->ArgsPointerMap->Bitmaps[0];
  EXPECT_EQ(Args0.Index, 0u);
  EXPECT_TRUE(Args0.isPointerSlot(0));
  EXPECT_FALSE(Args0.isPointerSlot(1));
  EXPECT_TRUE(Args0.isPointerSlot(2));
  // A slot past `nbit` is not described by the map, so it is not a pointer.
  EXPECT_FALSE(Args0.isPointerSlot(3));
  EXPECT_FALSE(F->Go->ArgsPointerMap->Bitmaps[1].isPointerSlot(0));
  EXPECT_TRUE(F->Go->ArgsPointerMap->Bitmaps[1].isPointerSlot(1));

  ASSERT_TRUE(F->Go->LocalsPointerMap.has_value());
  EXPECT_EQ(F->Go->LocalsPointerMap->BitCount, 9u);
  ASSERT_EQ(F->Go->LocalsPointerMap->Bitmaps.size(), 2u);
  // Nine bits occupy two bytes, and the ninth lives in the low bit of the
  // second one.
  EXPECT_EQ(F->Go->LocalsPointerMap->Bitmaps[0].Bits.size(), 2u);
  EXPECT_TRUE(F->Go->LocalsPointerMap->Bitmaps[0].isPointerSlot(8));
  EXPECT_FALSE(F->Go->LocalsPointerMap->Bitmaps[1].isPointerSlot(8));
}

TEST(GoStackMaps, CountsTheBitmapsOfAZeroBitMap) {
  GoTestImage T;
  // The record the linker shares between every function whose argument area
  // holds no pointer: one bitmap, zero bits wide, and so no bytes at all.
  const va_t ArgsVA = T.addPayload(buildStackMap(0, {{}}));
  const va_t LocalsVA = T.addPayload(buildStackMap(4, {{0b0101}}));

  GoFuncSpec Work = makeDeferringFunc("main.work", kTextVA + 0x100);
  PCValueTable StackMapIndex;
  StackMapIndex.Steps = {{0, 0x40}};
  Work.PCData = {std::nullopt, StackMapIndex};
  Work.FuncData = {ArgsVA, LocalsVA};
  T.installPclnTab(
      buildPclnTab(kGo116Magic, {Work}, kTextVA + 0x200).Bytes);

  parseGoExceptions(T.Img);

  const ExceptionFunction *F =
      findRecord(T.Img.ExceptionMetadata, kTextVA + 0x100);
  ASSERT_NE(F, nullptr);
  EXPECT_EQ(F->ParseStatus, ExceptionParseStatus::Complete);

  // The map declares its bitmap even though it spans no bytes, so the index
  // that names it stays satisfiable and the table it selects into is kept.
  ASSERT_TRUE(F->Go->ArgsPointerMap.has_value());
  EXPECT_EQ(F->Go->ArgsPointerMap->BitCount, 0u);
  ASSERT_EQ(F->Go->ArgsPointerMap->Bitmaps.size(), 1u);
  EXPECT_TRUE(F->Go->ArgsPointerMap->Bitmaps[0].Bits.empty());
  EXPECT_FALSE(F->Go->ArgsPointerMap->Bitmaps[0].isPointerSlot(0));
  ASSERT_EQ(F->Go->StackMapRanges.size(), 1u);
  EXPECT_EQ(F->Go->StackMapRanges[0].Index, 0);
}

TEST(GoStackMaps, StartsEachBitmapOnItsOwnByteBoundary) {
  GoTestImage T;
  // A bit count that is an exact multiple of eight is where a stride computed
  // one byte out stops being harmless, because the rounding that hides the
  // error for every other width contributes nothing here.
  const va_t ArgsVA = T.addPayload(buildStackMap(8, {{0x01}, {0x80}, {0x24}}));
  GoFuncSpec Work = makeDeferringFunc("main.work", kTextVA + 0x100);
  Work.FuncData = {ArgsVA};
  T.installPclnTab(
      buildPclnTab(kGo116Magic, {Work}, kTextVA + 0x200).Bytes);

  parseGoExceptions(T.Img);

  const ExceptionFunction *F = findRecord(T.Img.ExceptionMetadata,
                                          kTextVA + 0x100);
  ASSERT_NE(F, nullptr);
  ASSERT_TRUE(F->Go->ArgsPointerMap.has_value());
  const std::vector<GoStackMapBitmap> &Bitmaps =
      F->Go->ArgsPointerMap->Bitmaps;
  ASSERT_EQ(Bitmaps.size(), 3u);
  for (const GoStackMapBitmap &Bitmap : Bitmaps)
    EXPECT_EQ(Bitmap.Bits.size(), 1u);
  EXPECT_EQ(Bitmaps[0].Bits[0], 0x01);
  EXPECT_EQ(Bitmaps[1].Bits[0], 0x80);
  EXPECT_EQ(Bitmaps[2].Bits[0], 0x24);
  EXPECT_TRUE(Bitmaps[1].isPointerSlot(7));
  EXPECT_TRUE(Bitmaps[2].isPointerSlot(2));
  EXPECT_TRUE(Bitmaps[2].isPointerSlot(5));
  EXPECT_FALSE(Bitmaps[2].isPointerSlot(3));
}

TEST(GoStackMaps, TiesEachBitmapToThePCRangeThatSelectsIt) {
  GoTestImage T;
  const va_t ArgsVA = T.addPayload(buildStackMap(1, {{0b1}, {0b0}, {0b1}}));
  GoFuncSpec Work = makeDeferringFunc("main.work", kTextVA + 0x100);
  PCValueTable StackMapIndex;
  StackMapIndex.Steps = {{-1, 0x08}, {0, 0x18}, {2, 0x20}};
  Work.PCData = {std::nullopt, StackMapIndex};
  Work.FuncData = {ArgsVA};
  T.installPclnTab(
      buildPclnTab(kGo116Magic, {Work}, kTextVA + 0x200).Bytes);

  parseGoExceptions(T.Img);

  const ExceptionFunction *F = findRecord(T.Img.ExceptionMetadata,
                                          kTextVA + 0x100);
  ASSERT_NE(F, nullptr);
  ASSERT_EQ(F->Go->StackMapRanges.size(), 3u);
  // The unset value is kept as the table spelled it rather than resolved to
  // index zero the way the runtime's prologue fallback would.
  EXPECT_EQ(F->Go->StackMapRanges[0].Index, -1);
  EXPECT_EQ(F->Go->StackMapRanges[0].Range.Begin, kTextVA + 0x100);
  EXPECT_EQ(F->Go->StackMapRanges[0].Range.End, kTextVA + 0x108);
  EXPECT_EQ(F->Go->StackMapRanges[1].Index, 0);
  EXPECT_EQ(F->Go->StackMapRanges[1].Range.End, kTextVA + 0x120);
  EXPECT_EQ(F->Go->StackMapRanges[2].Index, 2);
  EXPECT_EQ(F->Go->StackMapRanges[2].Range.End, kTextVA + 0x140);
}

TEST(GoStackMaps, RefusesAMapThatClaimsMoreBitmapsThanAFrameCanHave) {
  GoTestImage T;
  std::vector<uint8_t> Oversized = buildStackMap(8, {{0xFF}});
  // `n` alone decides how much the decoder would read, so it is the field a
  // funcdata pointer resolved through the wrong base most easily turns into a
  // large allocation.
  std::memcpy(Oversized.data(), "\xFF\xFF\xFF\x7F", 4);
  const va_t ArgsVA = T.addPayload(Oversized);

  GoFuncSpec Work = makeDeferringFunc("main.work", kTextVA + 0x100);
  Work.FuncData = {ArgsVA};
  T.installPclnTab(
      buildPclnTab(kGo116Magic, {Work}, kTextVA + 0x200).Bytes);

  parseGoExceptions(T.Img);

  const ExceptionFunction *F = findRecord(T.Img.ExceptionMetadata,
                                          kTextVA + 0x100);
  ASSERT_NE(F, nullptr);
  EXPECT_FALSE(F->Go->ArgsPointerMap.has_value());
  EXPECT_EQ(F->ParseStatus, ExceptionParseStatus::Partial);
  EXPECT_TRUE(
      anyDiagnosticContains(F->Diagnostics, "not a readable stackmap"));
}

TEST(GoStackMaps, RefusesAMapWhoseBitmapsAreNotFullyMapped) {
  GoTestImage T;
  // Three bitmaps are declared but only the first one's bytes exist.
  std::vector<uint8_t> Truncated = buildStackMap(8, {{0xFF}});
  std::memcpy(Truncated.data(), "\x03\x00\x00\x00", 4);
  const va_t ArgsVA = T.addPayload(Truncated);
  // Shrink the data segment so the missing bitmap bytes are off the end.
  T.Img.Segments[1].Size = static_cast<size_t>(ArgsVA + Truncated.size() -
                                               kDataVA);
  T.Img.Segments[1].Data.resize(T.Img.Segments[1].Size);

  GoFuncSpec Work = makeDeferringFunc("main.work", kTextVA + 0x100);
  Work.FuncData = {ArgsVA};
  T.installPclnTab(
      buildPclnTab(kGo116Magic, {Work}, kTextVA + 0x200).Bytes);

  parseGoExceptions(T.Img);

  const ExceptionFunction *F = findRecord(T.Img.ExceptionMetadata,
                                          kTextVA + 0x100);
  ASSERT_NE(F, nullptr);
  EXPECT_FALSE(F->Go->ArgsPointerMap.has_value());
  EXPECT_EQ(F->ParseStatus, ExceptionParseStatus::Partial);
}

TEST(GoStackMaps, DropsRangesThatSelectABitmapTheFunctionDoesNotHave) {
  GoTestImage T;
  const va_t ArgsVA = T.addPayload(buildStackMap(4, {{0b1010}}));
  GoFuncSpec Work = makeDeferringFunc("main.work", kTextVA + 0x100);
  PCValueTable StackMapIndex;
  StackMapIndex.Steps = {{0, 0x10}, {3, 0x10}};
  Work.PCData = {std::nullopt, StackMapIndex};
  Work.FuncData = {ArgsVA};
  T.installPclnTab(
      buildPclnTab(kGo116Magic, {Work}, kTextVA + 0x200).Bytes);

  parseGoExceptions(T.Img);

  const ExceptionFunction *F = findRecord(T.Img.ExceptionMetadata,
                                          kTextVA + 0x100);
  ASSERT_NE(F, nullptr);
  // The map itself is sound and stays; only the selection into it is dropped.
  EXPECT_TRUE(F->Go->ArgsPointerMap.has_value());
  EXPECT_TRUE(F->Go->StackMapRanges.empty());
  EXPECT_EQ(F->ParseStatus, ExceptionParseStatus::Partial);
  EXPECT_TRUE(anyDiagnosticContains(F->Diagnostics, "names no bitmap"));
}

TEST(GoStackMaps, AcceptsAMapWithNoBitsAtAll) {
  GoTestImage T;
  const va_t ArgsVA = T.addPayload(buildStackMap(0, {}));
  GoFuncSpec Work = makeDeferringFunc("main.work", kTextVA + 0x100);
  Work.FuncData = {ArgsVA};
  T.installPclnTab(
      buildPclnTab(kGo116Magic, {Work}, kTextVA + 0x200).Bytes);

  parseGoExceptions(T.Img);

  const ExceptionFunction *F = findRecord(T.Img.ExceptionMetadata,
                                          kTextVA + 0x100);
  ASSERT_NE(F, nullptr);
  ASSERT_TRUE(F->Go->ArgsPointerMap.has_value());
  EXPECT_EQ(F->Go->ArgsPointerMap->BitCount, 0u);
  EXPECT_TRUE(F->Go->ArgsPointerMap->Bitmaps.empty());
  EXPECT_EQ(F->ParseStatus, ExceptionParseStatus::Complete);
}

//===----------------------------------------------------------------------===//
// Go 1.2 layout
//===----------------------------------------------------------------------===//

TEST(GoLegacyPclnTab, ReadsTheGo12FunctionTable) {
  GoTestImage T;
  GoFuncSpec Work = makeDeferringFunc("main.work", kTextVA + 0x100);
  Work.FuncData = {std::nullopt, std::nullopt};
  GoFuncSpec Other = makeDeferringFunc("main.other", kTextVA + 0x200);
  Other.DeferReturn = 0;
  Other.FuncData = {std::nullopt, std::nullopt};
  T.installPclnTab(
      buildPclnTab(kGo12Magic, {Work, Other}, kTextVA + 0x300).Bytes);

  EXPECT_TRUE(hasGoRuntimeMetadata(T.Img));
  parseGoExceptions(T.Img);

  const ExceptionInfo &Info = T.Img.ExceptionMetadata;
  ASSERT_TRUE(Info.GoModule.has_value());
  EXPECT_EQ(Info.GoModule->PclnTabMagic, kGo12Magic);
  EXPECT_EQ(Info.GoModule->PclnTabVersion, "go1.2");
  EXPECT_EQ(Info.GoModule->FunctionCount, 2u);
  EXPECT_EQ(Info.GoModule->PcHeaderVA, kPclnVA);
  // Names, pc-value tables, and `_func` records all live at offsets from the
  // head of the table, so all three bases are the header itself.
  EXPECT_EQ(Info.GoModule->FuncNameTabVA, kPclnVA);
  EXPECT_EQ(Info.GoModule->PcTabVA, kPclnVA);
  EXPECT_EQ(Info.GoModule->FuncTabVA, kPclnVA + 16);
  EXPECT_FALSE(Info.GoModule->UsesPreGo112FuncLayout);

  // The table is a symbol table too, and on this layout the entries it names
  // are absolute addresses rather than offsets from a base.
  bool NamedWork = false;
  for (const Symbol &S : T.Img.Symbols)
    if (S.Name == "main.work") {
      NamedWork = true;
      EXPECT_EQ(S.Addr, kTextVA + 0x100);
      EXPECT_EQ(S.Size, 0x100u);
    }
  EXPECT_TRUE(NamedWork);

  const ExceptionFunction *F = findRecord(Info, kTextVA + 0x100);
  ASSERT_NE(F, nullptr);
  EXPECT_EQ(F->CodeRange.Begin, kTextVA + 0x100);
  EXPECT_EQ(F->CodeRange.End, kTextVA + 0x200);
  ASSERT_TRUE(F->Go->DeferReturnOffset.has_value());
  EXPECT_EQ(*F->Go->DeferReturnOffset, 0x20u);
  ASSERT_TRUE(F->Go->FrameSize.has_value());
  EXPECT_EQ(*F->Go->FrameSize, 0x30);
  // The sentinel closes the last function, which is what gives `main.other`
  // an end at all.
  EXPECT_EQ(findRecord(Info, kTextVA + 0x200), nullptr);
}

TEST(GoLegacyPclnTab, ReportsThatTheUnsafePointTableDoesNotExistYet) {
  GoTestImage T;
  GoFuncSpec Work = makeDeferringFunc("main.work", kTextVA + 0x100);
  // Table 0 is a register map index on this layout, never an unsafe point
  // table, so decoding it as one would report async-preemption facts the
  // image never stated.
  PCValueTable RegMapIndex;
  RegMapIndex.Steps = {{-1, 0x40}, {0, 0x40}};
  Work.PCData = {RegMapIndex};
  T.installPclnTab(
      buildPclnTab(kGo12Magic, {Work}, kTextVA + 0x200).Bytes);

  parseGoExceptions(T.Img);

  const ExceptionInfo &Info = T.Img.ExceptionMetadata;
  EXPECT_TRUE(anyDiagnosticContains(Info.Diagnostics, "PCDATA_UnsafePoint"));
  EXPECT_EQ(Info.ParseStatus, ExceptionParseStatus::Partial);
  const ExceptionFunction *F = findRecord(Info, kTextVA + 0x100);
  ASSERT_NE(F, nullptr);
  EXPECT_TRUE(F->Go->UnsafePointRanges.empty());
}

TEST(GoLegacyPclnTab, ReadsThePreGo112RecordShape) {
  GoTestImage T;
  // The older shape has no `deferreturn`, so a record is only kept when the
  // body reaches a runtime entry point the table also names.
  GoFuncSpec Work;
  Work.Name = "main.work";
  Work.EntryVA = kTextVA + 0x100;
  Work.DeferReturn = 0x20; // the legacy frame size field, not a code offset
  Work.PcSP.Steps = {{0x30, 0x80}};
  Work.FuncData = {std::nullopt, std::nullopt};
  GoFuncSpec Panic;
  Panic.Name = "runtime.gopanic";
  Panic.EntryVA = kTextVA + 0x400;
  Panic.FuncData = {std::nullopt};
  T.installPclnTab(buildPclnTab(kGo12Magic, {Work, Panic}, kTextVA + 0x500,
                                /*MinLC=*/1, /*PreGo112Record=*/true)
                       .Bytes);
  T.writeText(kTextVA + 0x110, makeCall(kTextVA + 0x110, kTextVA + 0x400));

  parseGoExceptions(T.Img);

  const ExceptionInfo &Info = T.Img.ExceptionMetadata;
  ASSERT_TRUE(Info.GoModule.has_value());
  EXPECT_TRUE(Info.GoModule->UsesPreGo112FuncLayout);

  const ExceptionFunction *F = findRecord(Info, kTextVA + 0x100);
  ASSERT_NE(F, nullptr);
  ASSERT_EQ(F->Go->Panics.size(), 1u);
  EXPECT_EQ(F->Go->Panics[0].CallVA, kTextVA + 0x110);
  EXPECT_EQ(F->Go->Panics[0].RuntimeName, "runtime.gopanic");
  // The word the newer shape spends on `deferreturn` held the frame size
  // here, and reporting it would name an address in another function.
  EXPECT_FALSE(F->Go->DeferReturnOffset.has_value());
  EXPECT_EQ(F->Go->FuncID, 0);
}

TEST(GoLegacyPclnTab, ProvesWhichPCDataTableHoldsTheStackMapIndex) {
  GoTestImage T;
  const va_t LocalsVA = T.addPayload(buildStackMap(2, {{0b01}, {0b10}}));
  GoFuncSpec Work = makeDeferringFunc("main.work", kTextVA + 0x100);
  // Table 0 names an index the map cannot satisfy, which is what rules it out;
  // table 1 stays inside the map's two bitmaps.
  PCValueTable RegMapIndex;
  RegMapIndex.Steps = {{5, 0x80}};
  PCValueTable StackMapIndex;
  StackMapIndex.Steps = {{0, 0x40}, {1, 0x40}};
  Work.PCData = {RegMapIndex, StackMapIndex};
  Work.FuncData = {std::nullopt, LocalsVA};
  T.installPclnTab(
      buildPclnTab(kGo12Magic, {Work}, kTextVA + 0x200).Bytes);

  parseGoExceptions(T.Img);

  const ExceptionInfo &Info = T.Img.ExceptionMetadata;
  ASSERT_TRUE(Info.GoModule.has_value());
  ASSERT_TRUE(Info.GoModule->StackMapPCDataIndex.has_value());
  EXPECT_EQ(*Info.GoModule->StackMapPCDataIndex, 1u);

  const ExceptionFunction *F = findRecord(Info, kTextVA + 0x100);
  ASSERT_NE(F, nullptr);
  ASSERT_EQ(F->Go->StackMapRanges.size(), 2u);
  EXPECT_EQ(F->Go->StackMapRanges[0].Index, 0);
  EXPECT_EQ(F->Go->StackMapRanges[1].Index, 1);
}

TEST(GoLegacyPclnTab, ReportsNoStackMapRangesWhenBothCandidatesSurvive) {
  GoTestImage T;
  const va_t LocalsVA = T.addPayload(buildStackMap(2, {{0b01}, {0b10}}));
  GoFuncSpec Work = makeDeferringFunc("main.work", kTextVA + 0x100);
  PCValueTable Ambiguous;
  Ambiguous.Steps = {{0, 0x40}, {1, 0x40}};
  Work.PCData = {Ambiguous, Ambiguous};
  Work.FuncData = {std::nullopt, LocalsVA};
  T.installPclnTab(
      buildPclnTab(kGo12Magic, {Work}, kTextVA + 0x200).Bytes);

  parseGoExceptions(T.Img);

  const ExceptionInfo &Info = T.Img.ExceptionMetadata;
  ASSERT_TRUE(Info.GoModule.has_value());
  EXPECT_FALSE(Info.GoModule->StackMapPCDataIndex.has_value());
  EXPECT_TRUE(
      anyDiagnosticContains(Info.Diagnostics, "PCDATA_StackMapIndex"));

  const ExceptionFunction *F = findRecord(Info, kTextVA + 0x100);
  ASSERT_NE(F, nullptr);
  // Nothing proved which table selects a bitmap, but the bitmaps themselves
  // were never in doubt.
  EXPECT_TRUE(F->Go->LocalsPointerMap.has_value());
  EXPECT_TRUE(F->Go->StackMapRanges.empty());
}

TEST(GoLegacyPclnTab, ReadsOpenCodedDeferInfoFromItsPreGo116Index) {
  GoTestImage T;
  const va_t DeferInfoVA = T.addPayload(buildContiguousDeferInfo(0x18, 0x10));
  GoFuncSpec Work = makeDeferringFunc("main.work", kTextVA + 0x100);
  Work.PcSP.Steps = {{0x20, 0x80}};
  // Six entries is the shape only Go 1.14 and later emit, which is what makes
  // index five unambiguous on a table this old.
  Work.FuncData = {std::nullopt, std::nullopt, std::nullopt,
                   std::nullopt, std::nullopt, DeferInfoVA};
  T.installPclnTab(
      buildPclnTab(kGo12Magic, {Work}, kTextVA + 0x200).Bytes);

  parseGoExceptions(T.Img);

  const ExceptionFunction *F = findRecord(T.Img.ExceptionMetadata,
                                          kTextVA + 0x100);
  ASSERT_NE(F, nullptr);
  EXPECT_TRUE(F->Go->UsesOpenCodedDefers);
  ASSERT_TRUE(F->Go->OpenCodedDeferInfo.has_value());
  EXPECT_EQ(F->Go->OpenCodedDeferInfo->DeferBitsOffset, 0x18u);
  EXPECT_EQ(F->Go->OpenCodedDeferInfo->SlotsOffset, 0x10u);
  ASSERT_EQ(F->Go->OpenCodedDefers.size(), 2u);
  EXPECT_EQ(F->Go->OpenCodedDefers[0].ClosureOffset, -0x10);
  EXPECT_EQ(F->Go->OpenCodedDefers[1].ClosureOffset, -0x08);
}

TEST(GoLegacyPclnTab, RefusesAHeaderWhoseFileTableOffsetIsMissing) {
  GoTestImage T;
  GoFuncSpec Work = makeDeferringFunc("main.work", kTextVA + 0x100);
  BuiltPclnTab Tab = buildPclnTab(kGo12Magic, {Work}, kTextVA + 0x200);
  // The slot behind the functab sentinel is the only thing that says where
  // the file table is; a zero there is not a table this decoder can walk.
  Tab.put32(16 + 3 * 8, 0);
  T.installPclnTab(Tab.Bytes);

  EXPECT_FALSE(hasGoRuntimeMetadata(T.Img));
  parseGoExceptions(T.Img);
  EXPECT_FALSE(T.Img.ExceptionMetadata.GoModule.has_value());
}

TEST(GoLegacyPclnTab, StopsAtAFunctabEntryPointingOutsideTheImage) {
  GoTestImage T;
  GoFuncSpec First = makeDeferringFunc("main.first", kTextVA + 0x100);
  GoFuncSpec Second = makeDeferringFunc("main.second", kTextVA + 0x200);
  BuiltPclnTab Tab = buildPclnTab(kGo12Magic, {First, Second},
                                  kTextVA + 0x300);
  // Second entry's funcoff, i.e. the fourth word of the functab.
  Tab.put64(16 + 3 * 8, 0x00F00000);
  T.installPclnTab(Tab.Bytes);

  parseGoExceptions(T.Img);

  const ExceptionInfo &Info = T.Img.ExceptionMetadata;
  ASSERT_TRUE(Info.GoModule.has_value());
  EXPECT_EQ(Info.GoModule->FunctionCount, 1u);
  EXPECT_TRUE(anyDiagnosticContains(Info.Diagnostics, "ended early"));
  EXPECT_EQ(Info.ParseStatus, ExceptionParseStatus::Partial);
}

//===----------------------------------------------------------------------===//
// Open-coded defer records
//
// Go 1.22 changed how `FUNCDATA_OpenCodedDeferInfo` is spelled without
// changing the pclntab magic, so the two layouts share `Go120Magic` and only
// the bytes say which one a record is.  Reading the older spelling as the
// newer one turns a slot count into a frame offset, which is rejected --
// silently costing every function in a pre-1.22 image its open-coded defer
// state, and, because the same read is what confirms the funcdata base, the
// base along with it.
//===----------------------------------------------------------------------===//

/// An image whose functions each carry one open-coded defer record.  Every
/// offset in such a record has to land inside the frame, so the frame size is
/// what bounds how deep a slot a test can describe.
GoTestImage buildDeferInfoImage(const std::vector<std::vector<uint8_t>> &Records,
                                int32_t FrameSize = 0x30) {
  GoTestImage T;
  std::vector<GoFuncSpec> Funcs;
  for (size_t I = 0; I < Records.size(); ++I) {
    const va_t RecordVA = T.addPayload(Records[I]);
    GoFuncSpec Work = makeDeferringFunc("main.work" + std::to_string(I),
                                        kTextVA + 0x100 * (I + 1));
    Work.PcSP.Steps = {{FrameSize, 0x80}};
    Work.FuncData = {std::nullopt, std::nullopt, std::nullopt, std::nullopt,
                     RecordVA};
    Funcs.push_back(std::move(Work));
  }
  T.installPclnTab(
      buildPclnTab(kGo116Magic, Funcs,
                   kTextVA + 0x100 * (Records.size() + 1))
          .Bytes);
  return T;
}

TEST(GoOpenCodedDefers, ReadsTheEnumeratedRecordWrittenBeforeGo122) {
  GoTestImage T = buildDeferInfoImage(
      {buildEnumeratedDeferInfo(0x28, {0x18, 0x10, 0x08})});

  parseGoExceptions(T.Img);

  const ExceptionInfo &Info = T.Img.ExceptionMetadata;
  ASSERT_TRUE(Info.GoModule.has_value());
  EXPECT_EQ(Info.GoModule->OpenCodedDeferLayout,
            GoOpenCodedDeferLayout::Enumerated);

  const ExceptionFunction *F = findRecord(Info, kTextVA + 0x100);
  ASSERT_NE(F, nullptr);
  ASSERT_TRUE(F->Go->OpenCodedDeferInfo.has_value());
  EXPECT_EQ(F->Go->OpenCodedDeferInfo->DeferBitsOffset, 0x28u);
  EXPECT_EQ(F->Go->OpenCodedDeferInfo->SlotsOffset, 0x18u);
  EXPECT_TRUE(F->Go->OpenCodedDeferInfo->SlotCountIsExact);
  ASSERT_EQ(F->Go->OpenCodedDefers.size(), 3u);
  EXPECT_EQ(F->Go->OpenCodedDefers[0].ClosureOffset, -0x18);
  EXPECT_EQ(F->Go->OpenCodedDefers[1].ClosureOffset, -0x10);
  EXPECT_EQ(F->Go->OpenCodedDefers[2].ClosureOffset, -0x08);
  EXPECT_EQ(F->ParseStatus, ExceptionParseStatus::Complete);
}

// The enumerated record can say something the contiguous one cannot, and this
// is it: before Go 1.22 the compiler had no reason to put the closure slots
// next to each other, so a frame can hold two that are half a frame apart.
// Reading such a record as a run would report slots the function never had.
TEST(GoOpenCodedDefers, KeepsSlotsThatAreNotOneRun) {
  GoTestImage T =
      buildDeferInfoImage({buildEnumeratedDeferInfo(0x28, {0x20, 0x08})});

  parseGoExceptions(T.Img);

  const ExceptionFunction *F =
      findRecord(T.Img.ExceptionMetadata, kTextVA + 0x100);
  ASSERT_NE(F, nullptr);
  // Read as a run the same first slot would have reported four.
  ASSERT_EQ(F->Go->OpenCodedDefers.size(), 2u);
  EXPECT_EQ(F->Go->OpenCodedDefers[0].ClosureOffset, -0x20);
  EXPECT_EQ(F->Go->OpenCodedDefers[1].ClosureOffset, -0x08);
}

// The record Go wrote from 1.14 until deferred functions became argumentless
// in 1.18.  Its leading maximum argument frame shifts every field after it, so
// a reader that does not expect it takes that size for the bitmask offset and
// the bitmask offset for a slot count, which is how an image of this vintage
// loses every open-coded defer it has.
TEST(GoOpenCodedDefers, ReadsTheLegacyRecordWrittenBeforeGo118) {
  GoTestImage T = buildDeferInfoImage({buildLegacyEnumeratedDeferInfo(
      /*MaxArgSize=*/0x10, /*DeferBits=*/0x28,
      {LegacyDefer{0x10, 0x18, {}}, LegacyDefer{0x00, 0x08, {}}})});

  parseGoExceptions(T.Img);

  const ExceptionInfo &Info = T.Img.ExceptionMetadata;
  ASSERT_TRUE(Info.GoModule.has_value());
  EXPECT_EQ(Info.GoModule->OpenCodedDeferLayout,
            GoOpenCodedDeferLayout::LegacyEnumerated);

  const ExceptionFunction *F = findRecord(Info, kTextVA + 0x100);
  ASSERT_NE(F, nullptr);
  ASSERT_TRUE(F->Go->OpenCodedDeferInfo.has_value());
  EXPECT_EQ(F->Go->OpenCodedDeferInfo->DeferBitsOffset, 0x28u);
  EXPECT_EQ(F->Go->OpenCodedDeferInfo->SlotsOffset, 0x18u);
  EXPECT_TRUE(F->Go->OpenCodedDeferInfo->SlotCountIsExact);
  ASSERT_EQ(F->Go->OpenCodedDefers.size(), 2u);
  EXPECT_EQ(F->Go->OpenCodedDefers[0].ClosureOffset, -0x18);
  EXPECT_EQ(F->Go->OpenCodedDefers[1].ClosureOffset, -0x08);
  EXPECT_EQ(F->ParseStatus, ExceptionParseStatus::Complete);
}

// An argument list is variable-length filler between one defer's closure slot
// and the next one's, so a reader that does not walk it reads the second
// defer's fields out of the first defer's arguments.
TEST(GoOpenCodedDefers, WalksTheArgumentListSeparatingTwoLegacyDefers) {
  GoTestImage T = buildDeferInfoImage({buildLegacyEnumeratedDeferInfo(
      /*MaxArgSize=*/0x10, /*DeferBits=*/0x28,
      {LegacyDefer{0x10, 0x18, {{0x20, 0x08, 0x00}, {0x28, 0x08, 0x08}}},
       LegacyDefer{0x00, 0x08, {}}})});

  parseGoExceptions(T.Img);

  const ExceptionFunction *F =
      findRecord(T.Img.ExceptionMetadata, kTextVA + 0x100);
  ASSERT_NE(F, nullptr);
  ASSERT_TRUE(F->Go->OpenCodedDeferInfo.has_value());
  ASSERT_EQ(F->Go->OpenCodedDefers.size(), 2u);
  EXPECT_EQ(F->Go->OpenCodedDefers[0].ClosureOffset, -0x18);
  EXPECT_EQ(F->Go->OpenCodedDefers[1].ClosureOffset, -0x08);
}

// Three images with the same magic and three different records: whichever
// spelling the bytes are is the one that has to be read, because the header
// cannot say.  One magic covers the Go 1.22 rewrite outright, and the Go 1.18
// one happened inside the span of another.
TEST(GoOpenCodedDefers, PicksTheLayoutTheRecordsProveNotTheMagic) {
  GoTestImage Legacy = buildDeferInfoImage({buildLegacyEnumeratedDeferInfo(
      0x10, 0x28, {LegacyDefer{0x10, 0x18, {}}, LegacyDefer{0, 0x08, {}}})});
  GoTestImage Old = buildDeferInfoImage(
      {buildEnumeratedDeferInfo(0x28, {0x18, 0x10, 0x08})});
  GoTestImage New = buildDeferInfoImage({buildContiguousDeferInfo(0x28, 0x18)});

  parseGoExceptions(Legacy.Img);
  parseGoExceptions(Old.Img);
  parseGoExceptions(New.Img);

  ASSERT_TRUE(Legacy.Img.ExceptionMetadata.GoModule.has_value());
  ASSERT_TRUE(Old.Img.ExceptionMetadata.GoModule.has_value());
  ASSERT_TRUE(New.Img.ExceptionMetadata.GoModule.has_value());
  EXPECT_EQ(Legacy.Img.ExceptionMetadata.GoModule->PclnTabMagic,
            New.Img.ExceptionMetadata.GoModule->PclnTabMagic);
  EXPECT_EQ(Old.Img.ExceptionMetadata.GoModule->PclnTabMagic,
            New.Img.ExceptionMetadata.GoModule->PclnTabMagic);
  EXPECT_EQ(Legacy.Img.ExceptionMetadata.GoModule->OpenCodedDeferLayout,
            GoOpenCodedDeferLayout::LegacyEnumerated);
  EXPECT_EQ(Old.Img.ExceptionMetadata.GoModule->OpenCodedDeferLayout,
            GoOpenCodedDeferLayout::Enumerated);
  EXPECT_EQ(New.Img.ExceptionMetadata.GoModule->OpenCodedDeferLayout,
            GoOpenCodedDeferLayout::Contiguous);

  // All three name the same first slot, and only the newest cannot say how
  // many follow it.
  const ExceptionFunction *LegacyF =
      findRecord(Legacy.Img.ExceptionMetadata, kTextVA + 0x100);
  const ExceptionFunction *OldF =
      findRecord(Old.Img.ExceptionMetadata, kTextVA + 0x100);
  const ExceptionFunction *NewF =
      findRecord(New.Img.ExceptionMetadata, kTextVA + 0x100);
  ASSERT_NE(LegacyF, nullptr);
  ASSERT_NE(OldF, nullptr);
  ASSERT_NE(NewF, nullptr);
  EXPECT_EQ(LegacyF->Go->OpenCodedDeferInfo->SlotsOffset, 0x18u);
  EXPECT_EQ(OldF->Go->OpenCodedDeferInfo->SlotsOffset, 0x18u);
  EXPECT_EQ(NewF->Go->OpenCodedDeferInfo->SlotsOffset, 0x18u);
  EXPECT_TRUE(LegacyF->Go->OpenCodedDeferInfo->SlotCountIsExact);
  EXPECT_TRUE(OldF->Go->OpenCodedDeferInfo->SlotCountIsExact);
  EXPECT_FALSE(NewF->Go->OpenCodedDeferInfo->SlotCountIsExact);
}

// A frame holding the maximum eight open-coded defers reads the same either
// way: eight is both a slot count and a pointer-aligned slot offset, so the
// older spelling's second word is exactly what the newer one expects there.
// Such a record proves nothing and must not be allowed to outvote one that
// does, which it would if merely parsing counted as evidence.
TEST(GoOpenCodedDefers, LetsOnlyDistinguishingRecordsDecideTheLayout) {
  const std::vector<uint32_t> EightSlots = {0x40, 0x38, 0x30, 0x28,
                                            0x20, 0x18, 0x10, 0x08};
  GoTestImage T = buildDeferInfoImage(
      {
          buildEnumeratedDeferInfo(0x48, EightSlots),
          buildEnumeratedDeferInfo(0x48, EightSlots),
          buildEnumeratedDeferInfo(0x28, {0x20, 0x08}),
      },
      /*FrameSize=*/0x80);

  parseGoExceptions(T.Img);

  const ExceptionInfo &Info = T.Img.ExceptionMetadata;
  ASSERT_TRUE(Info.GoModule.has_value());
  EXPECT_EQ(Info.GoModule->OpenCodedDeferLayout,
            GoOpenCodedDeferLayout::Enumerated);

  // The ambiguous records are then read the way the image was decided to be
  // written, which is the whole point of deciding it once rather than per
  // record: read as a run, each would have reported one slot instead of eight.
  const ExceptionFunction *F = findRecord(Info, kTextVA + 0x100);
  ASSERT_NE(F, nullptr);
  EXPECT_EQ(F->Go->OpenCodedDefers.size(), EightSlots.size());
}

// A slot offset is pointer aligned and a slot count is at most eight, so
// neither reading accepts a second word that is neither.
TEST(GoOpenCodedDefers, ReportsARecordNoLayoutCanRead) {
  GoTestImage T = buildDeferInfoImage({buildContiguousDeferInfo(0x40, 0x0d)});

  parseGoExceptions(T.Img);

  const ExceptionFunction *F =
      findRecord(T.Img.ExceptionMetadata, kTextVA + 0x100);
  ASSERT_NE(F, nullptr);
  EXPECT_TRUE(F->Go->UsesOpenCodedDefers);
  EXPECT_FALSE(F->Go->OpenCodedDeferInfo.has_value());
  EXPECT_EQ(F->ParseStatus, ExceptionParseStatus::Partial);
  EXPECT_TRUE(anyDiagnosticContains(F->Diagnostics,
                                    "does not describe a frame"));
}

//===----------------------------------------------------------------------===//
// Malformed records
//===----------------------------------------------------------------------===//

TEST(GoMalformedRecords, RejectsARecordDeclaringMorePCDataTablesThanExist) {
  GoTestImage T;
  GoFuncSpec Work = makeDeferringFunc("main.work", kTextVA + 0x100);
  BuiltPclnTab Tab = buildPclnTab(kGo116Magic, {Work}, kTextVA + 0x200);
  // `npcdata` sizes the array the funcdata pointers sit behind, so a count
  // this large would walk the decoder off the end of the record.
  Tab.put32(Tab.RecordOffsets[0] + 32, 1000);
  T.installPclnTab(Tab.Bytes);

  parseGoExceptions(T.Img);
  EXPECT_FALSE(T.Img.ExceptionMetadata.GoModule.has_value());
}

TEST(GoMalformedRecords, RejectsARecordDeclaringMoreFuncDataThanExist) {
  GoTestImage T;
  GoFuncSpec Work = makeDeferringFunc("main.work", kTextVA + 0x100);
  BuiltPclnTab Tab = buildPclnTab(kGo116Magic, {Work}, kTextVA + 0x200);
  Tab.put8(Tab.RecordOffsets[0] + 43, 200);
  T.installPclnTab(Tab.Bytes);

  parseGoExceptions(T.Img);
  EXPECT_FALSE(T.Img.ExceptionMetadata.GoModule.has_value());
}

TEST(GoMalformedRecords, RejectsAHeaderWithAnImpossiblePCQuantum) {
  GoTestImage T;
  GoFuncSpec Work = makeDeferringFunc("main.work", kTextVA + 0x100);
  BuiltPclnTab Tab = buildPclnTab(kGo116Magic, {Work}, kTextVA + 0x200);
  Tab.put8(6, 3);
  T.installPclnTab(Tab.Bytes);

  EXPECT_FALSE(hasGoRuntimeMetadata(T.Img));
}

TEST(GoMalformedRecords, RejectsAHeaderWhosePointerSizeContradictsTheImage) {
  GoTestImage T;
  GoFuncSpec Work = makeDeferringFunc("main.work", kTextVA + 0x100);
  BuiltPclnTab Tab = buildPclnTab(kGo116Magic, {Work}, kTextVA + 0x200);
  Tab.put8(7, 4);
  T.installPclnTab(Tab.Bytes);

  EXPECT_FALSE(hasGoRuntimeMetadata(T.Img));
}

TEST(GoMalformedRecords, LeavesAnImageWithNoPclnTabAlone) {
  GoTestImage T;
  parseGoExceptions(T.Img);
  EXPECT_FALSE(T.Img.ExceptionMetadata.GoModule.has_value());
  EXPECT_TRUE(T.Img.ExceptionMetadata.Diagnostics.empty());
  EXPECT_EQ(T.Img.ExceptionMetadata.ParseStatus,
            ExceptionParseStatus::Complete);
}

} // namespace
