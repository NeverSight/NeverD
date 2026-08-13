//===- GoRuntimeEHTestsDetail.h - Go pclntab test harness -------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
//
// The byte-buffer builder, the synthetic Go image, and the pclntab and
// funcdata assemblers shared by the GoRuntime* translation units.
// Every free definition is `inline` so the TUs link together.
//
//===----------------------------------------------------------------------===//

#ifndef NEVERD_UNITTESTS_LIFT_LANGUAGE_GORUNTIMEEHTESTSDETAIL_H
#define NEVERD_UNITTESTS_LIFT_LANGUAGE_GORUNTIMEEHTESTSDETAIL_H

#include "gtest/gtest.h"

#include "neverd/loader/Go/GoRuntimeEH.h"

#include <array>
#include <cstring>
#include <optional>
#include <string>
#include <vector>

#include "neverd/loader/BinaryImage.h"

#include <cstddef>
#include <cstdint>

namespace neverd::go_eh_test {

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

inline constexpr va_t kTextVA = 0x400000;
inline constexpr size_t kTextSize = 0x2000;
inline constexpr va_t kDataVA = 0x500000;
inline constexpr size_t kDataSize = 0x8000;
/// The `pclntab` goes at the start of the data segment; funcdata payloads,
/// which it addresses by relocated pointer, go well past whatever it needs.
inline constexpr va_t kPclnVA = kDataVA;
inline constexpr va_t kPayloadVA = kDataVA + 0x4000;

inline constexpr uint32_t kGo12Magic = 0xFFFFFFFBu;
inline constexpr uint32_t kGo116Magic = 0xFFFFFFFAu;

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
inline std::vector<uint8_t> makeCall(va_t SiteVA, va_t TargetVA) {
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

inline void appendPCValueTable(ByteBuilder &B, const PCValueTable &T) {
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
inline BuiltPclnTab buildPclnTab(uint32_t Magic, const std::vector<GoFuncSpec> &Funcs,
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
inline std::vector<uint8_t>
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
inline std::vector<uint8_t> buildContiguousDeferInfo(uint32_t DeferBits,
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
inline std::vector<uint8_t> buildEnumeratedDeferInfo(uint32_t DeferBits,
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
inline std::vector<uint8_t>
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

inline const ExceptionFunction *findRecord(const ExceptionInfo &Info, va_t EntryVA) {
  for (const ExceptionFunction &F : Info.Functions)
    if (F.Go && F.Go->EntryVA == EntryVA)
      return &F;
  return nullptr;
}

inline bool anyDiagnosticContains(const std::vector<std::string> &Diagnostics,
                           const std::string &Needle) {
  for (const std::string &D : Diagnostics)
    if (D.find(Needle) != std::string::npos)
      return true;
  return false;
}

/// A function that has exceptional control flow purely because it declares a
/// `deferreturn`, which is the cheapest way to make the decoder keep a record.
inline GoFuncSpec makeDeferringFunc(const std::string &Name, va_t EntryVA) {
  GoFuncSpec F;
  F.Name = Name;
  F.EntryVA = EntryVA;
  F.DeferReturn = 0x20;
  F.PcSP.Steps = {{0x30, 0x80}};
  return F;
}

} // namespace neverd::go_eh_test

#endif // NEVERD_UNITTESTS_LIFT_LANGUAGE_GORUNTIMEEHTESTSDETAIL_H
