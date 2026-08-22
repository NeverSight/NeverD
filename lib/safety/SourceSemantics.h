//===- SourceSemantics.h - Input-source call semantics --------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#ifndef NEVERD_LIB_SAFETY_SOURCESEMANTICS_H
#define NEVERD_LIB_SAFETY_SOURCESEMANTICS_H

#include "neverd/ir/TargetRegInfo.h"
#include "neverd/ir/med/MedIR.h"
#include "neverd/libc/LibCNames.h"
#include "neverd/loader/BinaryImageModel.h"
#include "neverd/safety/SinkCatalog.h"

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/StringRef.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace neverd::safety::detail {

enum class FormattedSourceKind : uint8_t {
  ExternalInput,
  DerivedInput,
};

struct FormattedOutput {
  int ArgIndex = -1;
  uint32_t RequiredAssignments = 0;
  uint16_t Bytes = 0;
  bool AssignmentExecutionObservable = true;
};

struct BoundedTextOutput {
  int ArgIndex = -1;
  uint32_t RequiredAssignments = 0;
  uint64_t MaxChars = 0;
};

struct FormattedSourceOutputs {
  FormattedSourceKind Kind = FormattedSourceKind::ExternalInput;
  int InputArg = -1;
  std::vector<FormattedOutput> UnboundedTextArgs;
  std::vector<BoundedTextOutput> BoundedTextArgs;
  std::vector<FormattedOutput> ScalarArgs;
};

struct ParsedScanfOutputs {
  std::vector<FormattedOutput> UnboundedTextArgs;
  std::vector<BoundedTextOutput> BoundedTextArgs;
  std::vector<FormattedOutput> ScalarArgs;
};

inline std::optional<std::pair<std::string, FormattedSourceKind>>
formattedSourceName(llvm::StringRef StatedName) {
  std::string Name = SinkCatalog::normalize(StatedName);
  llvm::StringRef Base = Name;
  for (llvm::StringRef Prefix :
       {llvm::StringRef("isoc99_"), llvm::StringRef("isoc23_")})
    if (Base.consume_front(Prefix))
      break;
  if (Base == "scanf" || Base == "fscanf")
    return std::pair{Base.str(), FormattedSourceKind::ExternalInput};
  if (Base == "sscanf")
    return std::pair{Base.str(), FormattedSourceKind::DerivedInput};
  return std::nullopt;
}

inline std::optional<uint64_t> canonicalConstantValue(const MedVar &Value) {
  if (!Value.isConst() || Value.Size == 0 || Value.Size > sizeof(uint64_t))
    return std::nullopt;
  const uint32_t BitWidth = uint32_t(Value.Size) * 8;
  if (BitWidth >= 64)
    return Value.ConstVal;
  return Value.ConstVal & ((uint64_t(1) << BitWidth) - 1);
}

inline std::optional<std::string> readMappedCString(const BinaryImage *Img,
                                                    const MedVar &Address) {
  constexpr size_t MaxFormatBytes = 4096;
  if (!Img)
    return std::nullopt;
  const uint16_t PointerBytes = getTargetRegInfo(Img->Arch).PointerSize;
  if (PointerBytes == 0 || Address.Size != PointerBytes)
    return std::nullopt;
  const std::optional<uint64_t> AddressValue = canonicalConstantValue(Address);
  if (!AddressValue ||
      (*AddressValue == 0 && !isExactAddressProvenance(Address.Provenance)))
    return std::nullopt;
  const bool HasExactOwner = isExactAddressProvenance(Address.Provenance) &&
                             Address.AddressOwnerVA != InvalidVA;
  const Segment *Seg = Img->getSegmentFor(HasExactOwner ? Address.AddressOwnerVA
                                                        : *AddressValue);
  if (!Seg || !Seg->isReadable() || *AddressValue < Seg->VA)
    return std::nullopt;
  uint64_t OwnerAvailable = std::numeric_limits<uint64_t>::max();
  if (HasExactOwner) {
    if (!Seg->contains(*AddressValue))
      return std::nullopt;
    if (const Section *Owner = Img->getSectionFor(Address.AddressOwnerVA)) {
      if (!Owner->isReadable() || !Owner->contains(*AddressValue))
        return std::nullopt;
      OwnerAvailable = Owner->Size - (*AddressValue - Owner->VA);
    } else if (Img->segmentHasReadableSectionMetadata(*Seg)) {
      return std::nullopt;
    }
  }
  const uint64_t RawOffset = *AddressValue - Seg->VA;
  if (RawOffset >= Seg->Data.size())
    return std::nullopt;
  const size_t Offset = static_cast<size_t>(RawOffset);
  const uint64_t SegmentAvailable = Seg->Data.size() - Offset;
  const size_t Available = static_cast<size_t>(std::min<uint64_t>(
      MaxFormatBytes, std::min(OwnerAvailable, SegmentAvailable)));
  const uint8_t *Begin = Seg->Data.data() + Offset;
  const uint8_t *End = std::find(Begin, Begin + Available, uint8_t(0));
  if (End == Begin + Available)
    return std::nullopt;
  return std::string(reinterpret_cast<const char *>(Begin),
                     static_cast<size_t>(End - Begin));
}

inline bool isDigit(char C) {
  return std::isdigit(static_cast<unsigned char>(C)) != 0;
}

inline std::optional<ParsedScanfOutputs>
parseScanfOutputs(llvm::StringRef Format, unsigned FixedCount, size_t ArgCount,
                  uint16_t PointerBytes, uint16_t LongBytes,
                  uint16_t WideCharBytes) {
  if (FixedCount == 0 || FixedCount > ArgCount)
    return std::nullopt;
  int NextArg = static_cast<int>(FixedCount);
  uint32_t AssignmentCount = 0;
  bool InputExtentMayVary = false;
  bool AssignmentCountProvesNextDirective = false;
  ParsedScanfOutputs Outputs;
  for (size_t I = 0; I < Format.size(); ++I) {
    if (Format[I] != '%') {
      InputExtentMayVary |=
          std::isspace(static_cast<unsigned char>(Format[I])) != 0;
      AssignmentCountProvesNextDirective = false;
      continue;
    }
    if (++I >= Format.size())
      return std::nullopt;
    if (Format[I] == '%') {
      AssignmentCountProvesNextDirective = false;
      continue;
    }

    size_t PositionalEnd = I;
    while (PositionalEnd < Format.size() && isDigit(Format[PositionalEnd]))
      ++PositionalEnd;
    if (PositionalEnd < Format.size() && Format[PositionalEnd] == '$')
      return std::nullopt;

    bool Suppressed = false;
    if (Format[I] == '*') {
      Suppressed = true;
      if (++I >= Format.size())
        return std::nullopt;
    }

    bool HasWidth = false;
    uint64_t Width = 0;
    while (I < Format.size() && isDigit(Format[I])) {
      HasWidth = true;
      const uint64_t Digit = static_cast<uint64_t>(Format[I] - '0');
      if (Width > (std::numeric_limits<uint64_t>::max() - Digit) / 10)
        return std::nullopt;
      Width = Width * 10 + Digit;
      ++I;
    }
    if (I >= Format.size() || Format[I] == 'm' || (HasWidth && Width == 0))
      return std::nullopt;

    enum class LengthKind : uint8_t {
      None,
      Char,
      Short,
      Long,
      LongLong,
      IntMax,
      Size,
      PtrDiff,
      LongDouble,
    } Length = LengthKind::None;
    if (Format[I] == 'h' || Format[I] == 'l') {
      const char LengthChar = Format[I++];
      if (I < Format.size() && Format[I] == LengthChar) {
        ++I;
        Length = LengthChar == 'h' ? LengthKind::Char : LengthKind::LongLong;
      } else {
        Length = LengthChar == 'h' ? LengthKind::Short : LengthKind::Long;
      }
    } else if (llvm::StringRef("jztL").contains(Format[I])) {
      Length = Format[I] == 'j'   ? LengthKind::IntMax
               : Format[I] == 'z' ? LengthKind::Size
               : Format[I] == 't' ? LengthKind::PtrDiff
                                  : LengthKind::LongDouble;
      ++I;
    }
    if (I >= Format.size())
      return std::nullopt;

    const char Conversion = Format[I];
    const bool IsScanSet = Conversion == '[';
    if (!llvm::StringRef("diouxXaAeEfFgGcspn[").contains(Conversion))
      return std::nullopt;
    if (IsScanSet) {
      size_t Close = I + 1;
      if (Close < Format.size() && Format[Close] == '^')
        ++Close;
      if (Close < Format.size() && Format[Close] == ']')
        ++Close;
      Close = Format.find(']', Close);
      if (Close == llvm::StringRef::npos)
        return std::nullopt;
      I = Close;
    }

    const bool ConversionHasVariableExtent =
        Conversion != 'c' && Conversion != 'n';
    if (Suppressed) {
      InputExtentMayVary |= ConversionHasVariableExtent;
      AssignmentCountProvesNextDirective = false;
      continue;
    }
    if (NextArg < 0 || static_cast<size_t>(NextArg) >= ArgCount)
      return std::nullopt;
    if (Conversion != 'n')
      ++AssignmentCount;
    auto scalarBytes = [&]() -> uint16_t {
      if (Conversion == 'p')
        return PointerBytes;
      if (llvm::StringRef("aAeEfFgG").contains(Conversion)) {
        if (Length == LengthKind::None)
          return 4;
        if (Length == LengthKind::Long)
          return 8;
        return 0;
      }
      switch (Length) {
      case LengthKind::None:
        return 4;
      case LengthKind::Char:
        return 1;
      case LengthKind::Short:
        return 2;
      case LengthKind::Long:
        return LongBytes;
      case LengthKind::LongLong:
      case LengthKind::IntMax:
        return 8;
      case LengthKind::Size:
      case LengthKind::PtrDiff:
        return PointerBytes;
      case LengthKind::LongDouble:
        return 0;
      }
      return 0;
    };
    if (Conversion == 'n' && InputExtentMayVary)
      Outputs.ScalarArgs.push_back({NextArg, AssignmentCount, scalarBytes(),
                                    AssignmentCountProvesNextDirective});
    else if (Conversion == 'c') {
      Outputs.UnboundedTextArgs.push_back({NextArg, AssignmentCount});
      uint64_t UnitBytes = 0;
      if (Length == LengthKind::None)
        UnitBytes = 1;
      else if (Length == LengthKind::Long)
        UnitBytes = WideCharBytes;
      const uint64_t Characters = HasWidth ? Width : 1;
      if (UnitBytes != 0 &&
          Characters <= std::numeric_limits<uint16_t>::max() / UnitBytes)
        Outputs.ScalarArgs.push_back(
            {NextArg, AssignmentCount,
             static_cast<uint16_t>(Characters * UnitBytes)});
    } else if ((Conversion == 's' || IsScanSet) && Length != LengthKind::None)
      Outputs.UnboundedTextArgs.push_back({NextArg, AssignmentCount});
    else if ((Conversion == 's' || IsScanSet) && !HasWidth &&
             Length == LengthKind::None)
      Outputs.UnboundedTextArgs.push_back({NextArg, AssignmentCount});
    else if ((Conversion == 's' || IsScanSet) && HasWidth &&
             Length == LengthKind::None) {
      if (Width == std::numeric_limits<uint64_t>::max())
        return std::nullopt;
      Outputs.BoundedTextArgs.push_back({NextArg, AssignmentCount, Width});
    } else if (Conversion != 's' && Conversion != 'n' && !IsScanSet)
      Outputs.ScalarArgs.push_back({NextArg, AssignmentCount, scalarBytes()});
    ++NextArg;
    InputExtentMayVary |= ConversionHasVariableExtent;
    if (Conversion != 'n')
      AssignmentCountProvesNextDirective = true;
  }
  return Outputs;
}

inline std::optional<FormattedSourceOutputs>
recoverFormattedSourceOutputs(const BinaryImage *Img,
                              llvm::StringRef StatedName,
                              llvm::ArrayRef<MedVar> Args) {
  std::optional<std::pair<std::string, FormattedSourceKind>> Source =
      formattedSourceName(StatedName);
  if (!Source)
    return std::nullopt;
  const unsigned FixedCount = libc::varArgFixedCount(Source->first);
  if (FixedCount == 0 || FixedCount > Args.size())
    return std::nullopt;
  const unsigned FormatArg = FixedCount - 1;
  std::optional<std::string> Format = readMappedCString(Img, Args[FormatArg]);
  if (!Format)
    return std::nullopt;
  const uint16_t PointerBytes = getTargetRegInfo(Img->Arch).PointerSize;
  if (PointerBytes == 0 || Args.front().Size != PointerBytes)
    return std::nullopt;
  const uint16_t LongBytes =
      Img && Img->Format == BinaryFormat::COFF ? uint16_t(4) : PointerBytes;
  const uint16_t WideCharBytes =
      !Img                                ? uint16_t(0)
      : Img->Format == BinaryFormat::COFF ? uint16_t(2)
      : Img->Format == BinaryFormat::ELF || Img->Format == BinaryFormat::MachO
          ? uint16_t(4)
          : uint16_t(0);
  std::optional<ParsedScanfOutputs> Outputs = parseScanfOutputs(
      *Format, FixedCount, Args.size(), PointerBytes, LongBytes, WideCharBytes);
  if (!Outputs)
    return std::nullopt;
  FormattedSourceOutputs Result;
  Result.Kind = Source->second;
  Result.InputArg = Result.Kind == FormattedSourceKind::DerivedInput ? 0 : -1;
  Result.UnboundedTextArgs = std::move(Outputs->UnboundedTextArgs);
  Result.BoundedTextArgs = std::move(Outputs->BoundedTextArgs);
  Result.ScalarArgs = std::move(Outputs->ScalarArgs);
  return Result;
}

} // namespace neverd::safety::detail

#endif // NEVERD_LIB_SAFETY_SOURCESEMANTICS_H
