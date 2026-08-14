//===- DwarfEHFrame.cpp - Regenerated DWARF unwind records ---------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "neverd/backend/codegen/DwarfEHFrame.h"

#include "neverd/support/BinaryEncoding.h"
#include "neverd/support/DwarfEH.h"

#include "llvm/ADT/StringRef.h"
#include "llvm/Support/Errc.h"

#include <cstddef>
#include <cstdint>
#include <iterator>
#include <limits>
#include <map>
#include <optional>
#include <set>
#include <vector>

namespace neverd {

using namespace dweh;

namespace {

/// DW_EH_PE_signed: a signed integer whose width is the target address size.
/// It has no fixed width without the CIE's address-size context, so the shared
/// getEncodedSize helper deliberately cannot size it.
constexpr uint8_t kSignedPointerFormat = 0x08;

llvm::Error parseError(const char *Message) {
  return llvm::createStringError(llvm::errc::invalid_argument,
                                 "DWARF EH frame: %s", Message);
}

bool readULEB(llvm::ArrayRef<uint8_t> Bytes, size_t &Cursor, uint64_t &Value) {
  Value = 0;
  for (unsigned Shift = 0; Shift < 64; Shift += 7) {
    if (Cursor >= Bytes.size())
      return false;
    const uint8_t Byte = Bytes[Cursor++];
    if (Shift == 63 && (Byte & 0x7f) > 1)
      return false;
    Value |= static_cast<uint64_t>(Byte & 0x7f) << Shift;
    if ((Byte & 0x80) == 0)
      return true;
  }
  return false;
}

bool skipSLEB(llvm::ArrayRef<uint8_t> Bytes, size_t &Cursor) {
  for (unsigned I = 0; I < 10; ++I) {
    if (Cursor >= Bytes.size())
      return false;
    const uint8_t Byte = Bytes[Cursor++];
    if (I == 9) {
      if ((Byte & 0x80) != 0)
        return false;
      const uint8_t Payload = Byte & 0x7f;
      return Payload == 0 || Payload == 0x7f;
    }
    if ((Byte & 0x80) == 0)
      return true;
  }
  return false;
}

bool readCIEEncoding(llvm::ArrayRef<uint8_t> Record, size_t Cursor,
                     bool Is64BitAddress, uint8_t &Encoding,
                     bool &HasAugmentationData) {
  Encoding = Absptr;
  HasAugmentationData = false;
  if (Cursor >= Record.size())
    return false;
  const uint8_t Version = Record[Cursor++];
  if (Version != 1 && Version != 3 && Version != 4)
    return false;

  const size_t AugmentationBegin = Cursor;
  while (Cursor < Record.size() && Record[Cursor] != 0)
    ++Cursor;
  if (Cursor >= Record.size())
    return false;
  const llvm::StringRef Augmentation(
      reinterpret_cast<const char *>(Record.data() + AugmentationBegin),
      Cursor - AugmentationBegin);
  ++Cursor;

  if (Version == 4) {
    if (Cursor + 2 > Record.size())
      return false;
    const uint8_t AddressSize = Record[Cursor++];
    const uint8_t SegmentSelectorSize = Record[Cursor++];
    if (AddressSize != (Is64BitAddress ? 8 : 4) || SegmentSelectorSize != 0)
      return false;
  }

  uint64_t Scratch = 0;
  if (!readULEB(Record, Cursor, Scratch) || !skipSLEB(Record, Cursor))
    return false;
  // Version 1 predates the ULEB form and stores the return-address register in
  // exactly one byte.  Versions 3 and 4 use ULEB128.
  if (Version == 1) {
    if (Cursor >= Record.size())
      return false;
    ++Cursor;
  } else if (!readULEB(Record, Cursor, Scratch)) {
    return false;
  }
  if (Augmentation.empty())
    return true;
  if (Augmentation.front() != 'z')
    return false;
  HasAugmentationData = true;

  uint64_t AugmentationSize = 0;
  if (!readULEB(Record, Cursor, AugmentationSize) ||
      AugmentationSize > Record.size() - Cursor)
    return false;
  const size_t AugmentationEnd = Cursor + AugmentationSize;
  for (char Kind : Augmentation.drop_front()) {
    switch (Kind) {
    case 'L':
      if (Cursor >= AugmentationEnd)
        return false;
      ++Cursor;
      break;
    case 'P': {
      if (Cursor >= AugmentationEnd)
        return false;
      const uint8_t PersonalityEncoding = Record[Cursor++];
      const uint8_t PersonalityFormat = getFormat(PersonalityEncoding);
      const size_t Size = PersonalityFormat == Absptr ||
                                  PersonalityFormat == kSignedPointerFormat
                              ? (Is64BitAddress ? 8 : 4)
                              : getEncodedSize(PersonalityEncoding);
      if (Size == 0 || Size > AugmentationEnd - Cursor)
        return false;
      Cursor += Size;
      break;
    }
    case 'R':
      if (Cursor >= AugmentationEnd)
        return false;
      Encoding = Record[Cursor++];
      break;
    case 'S':
    case 'B':
    case 'G':
      break;
    default:
      return false;
    }
  }
  const uint8_t Format = getFormat(Encoding);
  const bool SupportedFormat =
      Format == Absptr || Format == kSignedPointerFormat || Format == Udata2 ||
      Format == Udata4 || Format == Udata8 || Format == Sdata2 ||
      Format == Sdata4 || Format == Sdata8;
  const uint8_t Application = getApplication(Encoding);
  return Cursor == AugmentationEnd && Encoding != Omit &&
         (Encoding & Indirect) == 0 && SupportedFormat &&
         (Application == AbsoluteApp || Application == PCRel);
}

struct EncodedValue {
  uint64_t Unsigned = 0;
  int64_t Signed = 0;
  bool IsSigned = false;
  size_t Size = 0;
};

std::optional<EncodedValue> readFixedValue(llvm::ArrayRef<uint8_t> Bytes,
                                           size_t Cursor, uint8_t Encoding,
                                           bool Is64BitAddress) {
  EncodedValue Value;
  const uint8_t Format = getFormat(Encoding);
  Value.Size = Format == Absptr || Format == kSignedPointerFormat
                   ? (Is64BitAddress ? 8 : 4)
                   : getEncodedSize(Encoding);
  if (Cursor > Bytes.size() || Value.Size == 0 ||
      Value.Size > Bytes.size() - Cursor)
    return std::nullopt;

  switch (Format) {
  case Absptr:
  case Udata4:
    Value.Unsigned = readLE<uint32_t>(Bytes.data() + Cursor);
    if (Value.Size == 8)
      Value.Unsigned = readLE<uint64_t>(Bytes.data() + Cursor);
    break;
  case Udata2:
    Value.Unsigned = readLE<uint16_t>(Bytes.data() + Cursor);
    break;
  case Udata8:
    Value.Unsigned = readLE<uint64_t>(Bytes.data() + Cursor);
    break;
  case Sdata2:
    Value.IsSigned = true;
    Value.Signed =
        static_cast<int16_t>(readLE<uint16_t>(Bytes.data() + Cursor));
    break;
  case Sdata4:
    Value.IsSigned = true;
    Value.Signed =
        static_cast<int32_t>(readLE<uint32_t>(Bytes.data() + Cursor));
    break;
  case Sdata8:
    Value.IsSigned = true;
    Value.Signed =
        static_cast<int64_t>(readLE<uint64_t>(Bytes.data() + Cursor));
    break;
  case kSignedPointerFormat:
    Value.IsSigned = true;
    Value.Signed =
        Value.Size == 8
            ? static_cast<int64_t>(readLE<uint64_t>(Bytes.data() + Cursor))
            : static_cast<int32_t>(readLE<uint32_t>(Bytes.data() + Cursor));
    break;
  default:
    return std::nullopt;
  }
  // Darwin encodes FDE initial locations as address-width PC-relative
  // pointers (`DW_EH_PE_pcrel | DW_EH_PE_absptr`).  Although absptr has no
  // explicit signed-format bit, the relocation result is a two's-complement
  // delta and is commonly negative because __eh_frame follows __text.
  if (Format == Absptr && getApplication(Encoding) == PCRel) {
    Value.IsSigned = true;
    Value.Signed = Value.Size == 8 ? static_cast<int64_t>(Value.Unsigned)
                                   : static_cast<int32_t>(Value.Unsigned);
  }
  return Value;
}

bool addSigned(uint64_t Base, int64_t Offset, uint64_t &Result) {
  if (Offset >= 0) {
    const uint64_t Positive = static_cast<uint64_t>(Offset);
    if (Positive > std::numeric_limits<uint64_t>::max() - Base)
      return false;
    Result = Base + Positive;
    return true;
  }
  const uint64_t Magnitude = static_cast<uint64_t>(-(Offset + 1)) + 1;
  if (Magnitude > Base)
    return false;
  Result = Base - Magnitude;
  return true;
}

} // namespace

llvm::Expected<std::vector<DwarfEHFrameRecord>>
decodeDwarfEHFrameRecords(llvm::ArrayRef<uint8_t> Bytes, uint64_t BaseVA,
                          bool Is64BitAddress) {
  std::vector<DwarfEHFrameRecord> Records;
  struct CIEInfo {
    uint8_t Encoding = Absptr;
    bool HasAugmentationData = false;
  };
  std::map<uint64_t, CIEInfo> CIEs;
  std::set<uint64_t> FDEBegins;
  std::map<uint64_t, uint64_t> FDERanges;

  uint64_t Offset = 0;
  while (Offset < Bytes.size()) {
    if (!rangeInBounds(Offset, sizeof(uint32_t), Bytes.size()))
      return parseError("truncated record length");
    const uint32_t Length = readLE<uint32_t>(Bytes.data() + Offset);
    if (Length == 0) {
      for (uint8_t Byte : Bytes.drop_front(Offset))
        if (Byte != 0)
          return parseError("nonzero bytes follow the terminator");
      break;
    }

    const bool Extended = Length == std::numeric_limits<uint32_t>::max();
    const uint64_t LengthFieldSize =
        Extended ? sizeof(uint32_t) + sizeof(uint64_t) : sizeof(uint32_t);
    if (!rangeInBounds(Offset, LengthFieldSize, Bytes.size()))
      return parseError("truncated extended record length");
    const uint64_t Payload =
        Extended ? readLE<uint64_t>(Bytes.data() + Offset + sizeof(uint32_t))
                 : Length;
    if (Payload > Bytes.size() - Offset - LengthFieldSize)
      return parseError("record extends past the section");
    const uint64_t RecordEnd = Offset + LengthFieldSize + Payload;

    const uint64_t IdOffset = Offset + LengthFieldSize;
    const uint64_t IdSize = Extended ? sizeof(uint64_t) : sizeof(uint32_t);
    if (!rangeInBounds(IdOffset, IdSize, RecordEnd))
      return parseError("truncated CIE pointer");
    const uint64_t Id = Extended ? readLE<uint64_t>(Bytes.data() + IdOffset)
                                 : readLE<uint32_t>(Bytes.data() + IdOffset);
    if (Id == 0) {
      CIEInfo Info;
      llvm::ArrayRef<uint8_t> Record = Bytes.take_front(RecordEnd);
      if (!readCIEEncoding(Record, IdOffset + IdSize, Is64BitAddress,
                           Info.Encoding, Info.HasAugmentationData))
        return parseError("malformed CIE augmentation");
      if (!CIEs.emplace(Offset, Info).second)
        return parseError("duplicate CIE offset");
    } else {
      if (Id > IdOffset)
        return parseError("CIE pointer underflows the section");
      const uint64_t CIEOffset = IdOffset - Id;
      auto CIE = CIEs.find(CIEOffset);
      if (CIE == CIEs.end())
        return parseError("FDE names an unknown CIE");

      const size_t InitialOffset = static_cast<size_t>(IdOffset + IdSize);
      auto Initial = readFixedValue(Bytes.take_front(RecordEnd), InitialOffset,
                                    CIE->second.Encoding, Is64BitAddress);
      if (!Initial)
        return parseError("invalid FDE initial location");
      uint64_t BeginVA = 0;
      switch (getApplication(CIE->second.Encoding)) {
      case AbsoluteApp:
        if (Initial->IsSigned && Initial->Signed < 0)
          return parseError("negative absolute initial location");
        BeginVA = Initial->IsSigned ? static_cast<uint64_t>(Initial->Signed)
                                    : Initial->Unsigned;
        break;
      case PCRel: {
        if (InitialOffset > std::numeric_limits<uint64_t>::max() - BaseVA)
          return parseError("initial-location field address overflows");
        const uint64_t FieldVA = BaseVA + InitialOffset;
        if (Initial->IsSigned) {
          if (!addSigned(FieldVA, Initial->Signed, BeginVA))
            return parseError("initial location overflows");
        } else {
          if (Initial->Unsigned >
              std::numeric_limits<uint64_t>::max() - FieldVA)
            return parseError("initial location overflows");
          BeginVA = FieldVA + Initial->Unsigned;
        }
        break;
      }
      default:
        return parseError("unsupported initial-location application");
      }

      const size_t RangeOffset = InitialOffset + Initial->Size;
      auto Range =
          readFixedValue(Bytes.take_front(RecordEnd), RangeOffset,
                         getFormat(CIE->second.Encoding), Is64BitAddress);
      if (!Range || (Range->IsSigned && Range->Signed <= 0))
        return parseError("invalid FDE address range");
      const uint64_t RangeSize = Range->IsSigned
                                     ? static_cast<uint64_t>(Range->Signed)
                                     : Range->Unsigned;
      if (RangeSize == 0 ||
          RangeSize > std::numeric_limits<uint64_t>::max() - BeginVA)
        return parseError("FDE address range overflows");
      size_t BodyCursor = RangeOffset + Range->Size;
      if (CIE->second.HasAugmentationData) {
        uint64_t AugmentationSize = 0;
        if (!readULEB(Bytes.take_front(RecordEnd), BodyCursor,
                      AugmentationSize) ||
            AugmentationSize > RecordEnd - BodyCursor)
          return parseError("malformed FDE augmentation");
      }
      const uint64_t EndVA = BeginVA + RangeSize;
      if (!FDEBegins.insert(BeginVA).second)
        return parseError("duplicate FDE initial location");
      auto NextRange = FDERanges.lower_bound(BeginVA);
      if ((NextRange != FDERanges.end() && NextRange->first < EndVA) ||
          (NextRange != FDERanges.begin() &&
           std::prev(NextRange)->second > BeginVA))
        return parseError("overlapping FDE address ranges");
      FDERanges.emplace(BeginVA, EndVA);
      if (Offset > std::numeric_limits<uint64_t>::max() - BaseVA)
        return parseError("FDE record address overflows");
      Records.push_back({BeginVA, EndVA, BaseVA + Offset});
    }

    if (RecordEnd <= Offset)
      return parseError("record cursor did not advance");
    Offset = RecordEnd;
  }
  return Records;
}

} // namespace neverd
