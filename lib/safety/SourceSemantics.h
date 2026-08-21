//===- SourceSemantics.h - Input-source call semantics --------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#ifndef NEVERD_LIB_SAFETY_SOURCESEMANTICS_H
#define NEVERD_LIB_SAFETY_SOURCESEMANTICS_H

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

inline std::optional<std::string> readMappedCString(const BinaryImage *Img,
                                                    const MedVar &Address) {
  constexpr size_t MaxFormatBytes = 4096;
  if (!Img || !Address.isConst() ||
      (Address.ConstVal == 0 && !isExactAddressProvenance(Address.Provenance)))
    return std::nullopt;
  const Segment *Seg = Img->getSegmentFor(Address.ConstVal);
  if (!Seg || !Seg->isReadable() || Address.ConstVal < Seg->VA)
    return std::nullopt;
  const uint64_t RawOffset = Address.ConstVal - Seg->VA;
  if (RawOffset >= Seg->Data.size())
    return std::nullopt;
  const size_t Offset = static_cast<size_t>(RawOffset);
  const size_t Available = std::min(MaxFormatBytes, Seg->Data.size() - Offset);
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
parseScanfOutputs(llvm::StringRef Format, unsigned FixedCount,
                  size_t ArgCount) {
  if (FixedCount == 0 || FixedCount > ArgCount)
    return std::nullopt;
  int NextArg = static_cast<int>(FixedCount);
  uint32_t AssignmentCount = 0;
  bool InputExtentMayVary = false;
  ParsedScanfOutputs Outputs;
  for (size_t I = 0; I < Format.size(); ++I) {
    if (Format[I] != '%') {
      InputExtentMayVary |=
          std::isspace(static_cast<unsigned char>(Format[I])) != 0;
      continue;
    }
    if (++I >= Format.size())
      return std::nullopt;
    if (Format[I] == '%')
      continue;

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

    bool HasLength = false;
    if (Format[I] == 'h' || Format[I] == 'l') {
      const char Length = Format[I++];
      HasLength = true;
      if (I < Format.size() && Format[I] == Length)
        ++I;
    } else if (llvm::StringRef("jztL").contains(Format[I])) {
      HasLength = true;
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
      continue;
    }
    if (NextArg < 0 || static_cast<size_t>(NextArg) >= ArgCount)
      return std::nullopt;
    if (Conversion != 'n')
      ++AssignmentCount;
    if (Conversion == 'n' && InputExtentMayVary)
      Outputs.ScalarArgs.push_back({NextArg, AssignmentCount});
    else if (Conversion == 'c')
      Outputs.UnboundedTextArgs.push_back({NextArg, AssignmentCount});
    else if ((Conversion == 's' || IsScanSet) && HasLength)
      Outputs.UnboundedTextArgs.push_back({NextArg, AssignmentCount});
    else if ((Conversion == 's' || IsScanSet) && !HasWidth && !HasLength)
      Outputs.UnboundedTextArgs.push_back({NextArg, AssignmentCount});
    else if ((Conversion == 's' || IsScanSet) && HasWidth && !HasLength) {
      if (Width == std::numeric_limits<uint64_t>::max())
        return std::nullopt;
      Outputs.BoundedTextArgs.push_back({NextArg, AssignmentCount, Width});
    } else if (Conversion != 's' && Conversion != 'n' && !IsScanSet)
      Outputs.ScalarArgs.push_back({NextArg, AssignmentCount});
    ++NextArg;
    InputExtentMayVary |= ConversionHasVariableExtent;
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
  std::optional<ParsedScanfOutputs> Outputs =
      parseScanfOutputs(*Format, FixedCount, Args.size());
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
