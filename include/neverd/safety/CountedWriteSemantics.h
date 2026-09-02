//===- CountedWriteSemantics.h - Counted-write ABI policy ------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Header-only, link-layer-neutral policy for exact counted-write call
/// families.  Both the safety analysis and the LLVM emitter consume this one
/// decision so name decoration, aliases, operand roles, and wchar width cannot
/// diverge across pipeline stages.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_SAFETY_COUNTEDWRITESEMANTICS_H
#define NEVERD_SAFETY_COUNTEDWRITESEMANTICS_H

#include "neverd/Common.h"

#include "llvm/ADT/StringRef.h"

#include <cstdint>
#include <optional>
#include <string>

namespace neverd::counted_write {

enum class SemanticKind : uint32_t {
  Memcpy = 1,
  Memmove = 2,
  Memset = 3,
  Bzero = 4,
  Bcopy = 5,
};

struct Layout {
  SemanticKind Kind = SemanticKind::Memcpy;
  uint32_t DestinationOperandIndex = 0;
  uint32_t LengthOperandIndex = 0;
  bool UsesWideElements = false;
};

struct Semantics {
  SemanticKind Kind = SemanticKind::Memcpy;
  uint32_t DestinationOperandIndex = 0;
  uint32_t LengthOperandIndex = 0;
  uint32_t ElementBytes = 1;

  friend bool operator==(const Semantics &Left, const Semantics &Right) {
    return Left.Kind == Right.Kind &&
           Left.DestinationOperandIndex == Right.DestinationOperandIndex &&
           Left.LengthOperandIndex == Right.LengthOperandIndex &&
           Left.ElementBytes == Right.ElementBytes;
  }
  friend bool operator!=(const Semantics &Left, const Semantics &Right) {
    return !(Left == Right);
  }
};

/// Normalize ordinary C ABI decoration without folding semantic aliases.
inline std::string normalizeCABIName(llvm::StringRef StatedName) {
  llvm::StringRef Name = StatedName;
  if (const size_t Bang = Name.rfind('!'); Bang != llvm::StringRef::npos)
    Name = Name.drop_front(Bang + 1);
  while (Name.starts_with("___imp_"))
    Name = Name.drop_front();
  while (Name.starts_with("__imp_"))
    Name = Name.drop_front(6);
  Name = stripLeadingUnderscores(Name);
  if (const size_t At = Name.rfind('@');
      At != llvm::StringRef::npos && At > 0 && At + 1 < Name.size()) {
    bool IsStdcallSuffix = true;
    for (const char C : Name.drop_front(At + 1))
      if (C < '0' || C > '9') {
        IsStdcallSuffix = false;
        break;
      }
    if (IsStdcallSuffix) {
      Name = Name.take_front(At);
      if (Name.starts_with('@'))
        Name = Name.drop_front();
      if (Name.ends_with('@'))
        Name = Name.drop_back();
    }
  }
  return Name.str();
}

/// Fold ABI aliases to the established analysis-family name.  Operand-layout
/// consumers must use classifyLayout(): AAPCS memset belongs to the memset
/// family but places its length at a different emitted argument index.
/// Fortified spellings stay distinct because other safety code also consumes
/// their capacity operand, even though their write layout is shared.
inline std::string canonicalSemanticName(llvm::StringRef StatedName) {
  std::string Name = normalizeCABIName(StatedName);
  if (Name == "aeabi_memcpy" || Name == "aeabi_memcpy4" ||
      Name == "aeabi_memcpy8")
    return "memcpy";
  if (Name == "aeabi_memmove" || Name == "aeabi_memmove4" ||
      Name == "aeabi_memmove8")
    return "memmove";
  if (Name == "aeabi_memset" || Name == "aeabi_memset4" ||
      Name == "aeabi_memset8")
    return "memset";
  if (Name == "aeabi_memclr" || Name == "aeabi_memclr4" ||
      Name == "aeabi_memclr8")
    return "bzero";
  return Name;
}

/// Classify the name-dependent part of v1.  Wide element size is deliberately
/// deferred to classify(), where the authenticated binary format is present.
inline std::optional<Layout> classifyLayout(llvm::StringRef StatedName) {
  const std::string ABIName = normalizeCABIName(StatedName);
  if (ABIName == "aeabi_memset" || ABIName == "aeabi_memset4" ||
      ABIName == "aeabi_memset8")
    return Layout{SemanticKind::Memset, 0, 1, false};
  if (ABIName == "aeabi_memclr" || ABIName == "aeabi_memclr4" ||
      ABIName == "aeabi_memclr8")
    return Layout{SemanticKind::Bzero, 0, 1, false};
  const std::string Name = canonicalSemanticName(StatedName);
  if (Name == "memcpy" || Name == "memcpy_chk")
    return Layout{SemanticKind::Memcpy, 0, 2, false};
  if (Name == "memmove" || Name == "memmove_chk")
    return Layout{SemanticKind::Memmove, 0, 2, false};
  if (Name == "wmemcpy")
    return Layout{SemanticKind::Memcpy, 0, 2, true};
  if (Name == "wmemmove")
    return Layout{SemanticKind::Memmove, 0, 2, true};
  if (Name == "memset" || Name == "memset_chk")
    return Layout{SemanticKind::Memset, 0, 2, false};
  if (Name == "bzero")
    return Layout{SemanticKind::Bzero, 0, 1, false};
  if (Name == "bcopy")
    return Layout{SemanticKind::Bcopy, 1, 2, false};
  return std::nullopt;
}

/// Resolve the full emitted-call contract.  A wide operation is rejected when
/// its wchar width is not fixed by one of the native publication formats.
inline std::optional<Semantics> classify(llvm::StringRef StatedName,
                                         BinaryFormat Format) {
  const std::optional<Layout> Shape = classifyLayout(StatedName);
  if (!Shape)
    return std::nullopt;
  uint32_t ElementBytes = 1;
  if (Shape->UsesWideElements) {
    switch (Format) {
    case BinaryFormat::COFF:
      ElementBytes = 2;
      break;
    case BinaryFormat::ELF:
    case BinaryFormat::MachO:
      ElementBytes = 4;
      break;
    default:
      return std::nullopt;
    }
  }
  return Semantics{Shape->Kind, Shape->DestinationOperandIndex,
                   Shape->LengthOperandIndex, ElementBytes};
}

/// Validate a decoded metadata value against the closed v1 semantic domain.
inline bool isValid(const Semantics &Value) {
  if (Value.DestinationOperandIndex == Value.LengthOperandIndex ||
      (Value.ElementBytes != 1 && Value.ElementBytes != 2 &&
       Value.ElementBytes != 4))
    return false;
  switch (Value.Kind) {
  case SemanticKind::Memcpy:
  case SemanticKind::Memmove:
    return Value.DestinationOperandIndex == 0 && Value.LengthOperandIndex == 2;
  case SemanticKind::Memset:
    return Value.ElementBytes == 1 && Value.DestinationOperandIndex == 0 &&
           (Value.LengthOperandIndex == 1 || Value.LengthOperandIndex == 2);
  case SemanticKind::Bzero:
    return Value.ElementBytes == 1 && Value.DestinationOperandIndex == 0 &&
           Value.LengthOperandIndex == 1;
  case SemanticKind::Bcopy:
    return Value.ElementBytes == 1 && Value.DestinationOperandIndex == 1 &&
           Value.LengthOperandIndex == 2;
  }
  return false;
}

} // namespace neverd::counted_write

#endif // NEVERD_SAFETY_COUNTEDWRITESEMANTICS_H
