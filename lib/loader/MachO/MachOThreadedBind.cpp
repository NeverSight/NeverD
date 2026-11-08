//===- MachOThreadedBind.cpp - Original arm64e chains --------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Collects ordinal-table bytecode and validates all chain storage before
/// publishing either unsigned pointer values or authenticated signing recipes.
//===----------------------------------------------------------------------===//

#include "MachOThreadedBind.h"

#include "MachOArm64eFixups.h"

#include "neverd/loader/Limits.h"
#include "neverd/loader/MachO/MachOArm64e.h"
#include "neverd/loader/MachO/MachOThreadedBind.h"
#include "neverd/object/SectionNames.h"
#include "neverd/support/BinaryEncoding.h"

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/BinaryFormat/MachO.h"
#include "llvm/Support/LEB128.h"

#include <algorithm>
#include <cstring>
#include <iterator>
#include <limits>
#include <optional>
#include <set>
#include <utility>
#include <vector>

namespace neverd::macho_loader {
namespace {
using namespace llvm::MachO;

struct BindOperation {
  uint8_t Opcode = 0;
  uint8_t Immediate = 0;
  uint64_t Value = 0;
  uint64_t Skip = 0;
  int64_t Addend = 0;
  llvm::StringRef Symbol;
};

/// Decode operands even during mode detection: symbol or LEB payload bytes
/// may contain the threaded opcode without selecting that format.
std::optional<BindOperation> readOperation(const uint8_t *&P,
                                           const uint8_t *End) {
  if (P == End)
    return std::nullopt;
  BindOperation Op;
  Op.Opcode = *P & BIND_OPCODE_MASK;
  Op.Immediate = *P++ & BIND_IMMEDIATE_MASK;
  const auto readULEB = [&](uint64_t &Value) {
    unsigned Size = 0;
    const char *Error = nullptr;
    Value = llvm::decodeULEB128(P, &Size, End, &Error);
    if (Error || !Size)
      return false;
    P += Size;
    return true;
  };
  switch (Op.Opcode) {
  case BIND_OPCODE_DONE:
  case BIND_OPCODE_SET_DYLIB_ORDINAL_IMM:
  case BIND_OPCODE_SET_DYLIB_SPECIAL_IMM:
  case BIND_OPCODE_SET_TYPE_IMM:
  case BIND_OPCODE_DO_BIND:
  case BIND_OPCODE_DO_BIND_ADD_ADDR_IMM_SCALED:
    break;
  case BIND_OPCODE_SET_DYLIB_ORDINAL_ULEB:
  case BIND_OPCODE_SET_SEGMENT_AND_OFFSET_ULEB:
  case BIND_OPCODE_ADD_ADDR_ULEB:
  case BIND_OPCODE_DO_BIND_ADD_ADDR_ULEB:
    if (!readULEB(Op.Value))
      return std::nullopt;
    break;
  case BIND_OPCODE_DO_BIND_ULEB_TIMES_SKIPPING_ULEB:
    if (!readULEB(Op.Value) || !readULEB(Op.Skip))
      return std::nullopt;
    break;
  case BIND_OPCODE_SET_ADDEND_SLEB: {
    unsigned Size = 0;
    const char *Error = nullptr;
    Op.Addend = llvm::decodeSLEB128(P, &Size, End, &Error);
    if (Error || !Size)
      return std::nullopt;
    P += Size;
    break;
  }
  case BIND_OPCODE_SET_SYMBOL_TRAILING_FLAGS_IMM: {
    const auto *Term = static_cast<const uint8_t *>(std::memchr(P, 0, End - P));
    if (!Term)
      return std::nullopt;
    Op.Symbol = llvm::StringRef(reinterpret_cast<const char *>(P), Term - P);
    P = Term + 1;
    break;
  }
  case arm64e::kThreadedOpcode:
    if (Op.Immediate == arm64e::kThreadedTableSize) {
      if (!readULEB(Op.Value))
        return std::nullopt;
    } else if (Op.Immediate != arm64e::kThreadedApply) {
      return std::nullopt;
    }
    break;
  default:
    return std::nullopt;
  }
  return Op;
}

} // namespace

threaded::StreamKind threaded::classify(llvm::ArrayRef<uint8_t> Bytes,
                                        bool Lazy) {
  StreamKind Kind;
  const uint8_t *P = Bytes.begin();
  size_t Operations = 0;
  while (P != Bytes.end()) {
    Kind.Threaded |= (*P & BIND_OPCODE_MASK) == arm64e::kThreadedOpcode;
    auto Op = readOperation(P, Bytes.end());
    if (!Op || ++Operations > limits::loader::kMachOThreadedMaxOpcodes) {
      Kind.Malformed = true;
      break;
    }
    if (Op->Opcode == BIND_OPCODE_DONE && !Lazy)
      break;
  }
  return Kind;
}

std::optional<threaded::Program> threaded::decode(llvm::ArrayRef<uint8_t> Bytes,
                                                  size_t LibraryCount) {
  Program Plan;
  std::optional<uint64_t> TableSize;
  std::optional<int64_t> Library;
  std::optional<unsigned> Segment;
  llvm::StringRef Symbol;
  uint8_t Type = 0;
  uint8_t Flags = 0;
  int64_t Addend = 0;
  uint64_t Offset = 0;
  const auto charge = [&](size_t Bytes) {
    if (Bytes > limits::loader::kMachOThreadedMaxCopiedBytes - Plan.CopiedBytes)
      return false;
    Plan.CopiedBytes += Bytes;
    return true;
  };
  const uint8_t *P = Bytes.begin();
  size_t Operations = 0;
  bool Done = false;
  while (P != Bytes.end()) {
    auto Op = readOperation(P, Bytes.end());
    if (!Op || ++Operations > limits::loader::kMachOThreadedMaxOpcodes)
      return std::nullopt;
    switch (Op->Opcode) {
    case BIND_OPCODE_DONE:
      if (Op->Immediate ||
          std::any_of(P, Bytes.end(), [](uint8_t Byte) { return Byte != 0; }))
        return std::nullopt;
      Done = true;
      P = Bytes.end();
      break;
    case BIND_OPCODE_SET_DYLIB_ORDINAL_IMM:
      Library = Op->Immediate;
      break;
    case BIND_OPCODE_SET_DYLIB_ORDINAL_ULEB:
      if (Op->Immediate ||
          Op->Value > uint64_t(std::numeric_limits<int64_t>::max()))
        return std::nullopt;
      Library = static_cast<int64_t>(Op->Value);
      break;
    case BIND_OPCODE_SET_DYLIB_SPECIAL_IMM:
      Library = Op->Immediate
                    ? static_cast<int8_t>(BIND_OPCODE_MASK | Op->Immediate)
                    : BIND_SPECIAL_DYLIB_SELF;
      break;
    case BIND_OPCODE_SET_SYMBOL_TRAILING_FLAGS_IMM:
      if (Op->Immediate & ~(BIND_SYMBOL_FLAGS_WEAK_IMPORT |
                            BIND_SYMBOL_FLAGS_NON_WEAK_DEFINITION))
        return std::nullopt;
      Symbol = Op->Symbol;
      Flags = Op->Immediate;
      break;
    case BIND_OPCODE_SET_TYPE_IMM:
      Type = Op->Immediate;
      break;
    case BIND_OPCODE_SET_ADDEND_SLEB:
      if (Op->Immediate)
        return std::nullopt;
      Addend = Op->Addend;
      break;
    case BIND_OPCODE_SET_SEGMENT_AND_OFFSET_ULEB:
      Segment = Op->Immediate;
      Offset = Op->Value;
      break;
    case BIND_OPCODE_DO_BIND: {
      if (Op->Immediate || !TableSize || Plan.Imports.size() >= *TableSize ||
          !Library || *Library < BIND_SPECIAL_DYLIB_WEAK_LOOKUP ||
          (*Library > 0 && uint64_t(*Library) > LibraryCount) ||
          Type != BIND_TYPE_POINTER || Symbol.empty())
        return std::nullopt;
      if (!charge(Symbol.size()))
        return std::nullopt;
      Plan.Imports.push_back({Symbol.str(), *Library, Addend, Flags});
      break;
    }
    case arm64e::kThreadedOpcode:
      if (Op->Immediate == arm64e::kThreadedTableSize) {
        if (TableSize || Op->Value > arm64e::kThreadedMaxOrdinalCount)
          return std::nullopt;
        TableSize = Op->Value;
        Plan.Imports.reserve(*TableSize);
      } else {
        if (!TableSize || Plan.Imports.size() != *TableSize || !Segment ||
            Plan.Roots.size() >= limits::loader::kMachODyldBindMaxBindSlots)
          return std::nullopt;
        Plan.Roots.push_back({*Segment, Offset});
      }
      break;
    default:
      // Address-advancing classic bind opcodes do not construct an ordinal
      // table or declare a threaded root.
      return std::nullopt;
    }
  }
  if (!Done || !TableSize || Plan.Imports.size() != *TableSize)
    return std::nullopt;
  return Plan;
}

namespace detail {
namespace {
std::optional<std::vector<Arm64eFixup>>
walk(const threaded::Program &Plan, llvm::ArrayRef<Arm64eImportRecord> Imports,
     size_t CopiedBytes, va_t Base, const BinaryImage &Img) {
  const auto Format = *arm64e::chainFormat(DYLD_CHAINED_PTR_ARM64E);
  std::vector<Arm64eFixup> Pending;
  std::set<va_t> Seen;
  std::set<unsigned> CheckedOwners;
  for (const auto &Root : Plan.Roots) {
    if (Root.SegmentIndex >= Img.Segments.size())
      return std::nullopt;
    const auto &Owner = Img.Segments[Root.SegmentIndex];
    if (!Owner.isWritable() || Owner.isExecutable() ||
        Owner.Size > InvalidVA - Owner.VA)
      return std::nullopt;
    if (CheckedOwners.insert(Root.SegmentIndex).second) {
      for (size_t I = 0; I < Img.Segments.size(); ++I) {
        const auto &Other = Img.Segments[I];
        if (Other.Name == section_names::macho::LinkeditSeg &&
            Root.SegmentIndex >= I)
          return std::nullopt;
        if (&Other == &Owner || !Other.Size)
          continue;
        if (Other.Size > InvalidVA - Other.VA ||
            (Owner.VA < Other.VA + Other.Size &&
             Other.VA < Owner.VA + Owner.Size))
          return std::nullopt;
      }
    }
    uint64_t Offset = Root.Offset;
    for (;;) {
      if (Offset % Format.Stride || Offset > InvalidVA - Owner.VA ||
          !rangeInBounds(Offset, arm64e::kPointerBytes, Owner.Size) ||
          !rangeInBounds(Offset, arm64e::kPointerBytes, Owner.FileSz) ||
          Pending.size() >= limits::loader::kMachODyldBindMaxBindSlots)
        return std::nullopt;
      const va_t Slot = Owner.VA + Offset;
      const auto Next = Seen.lower_bound(Slot);
      if (Slot % Format.Stride || arm64e::kPointerBytes > InvalidVA - Slot ||
          Img.getSegmentFor(Slot) != &Owner ||
          (Next != Seen.end() && *Next - Slot < arm64e::kPointerBytes) ||
          (Next != Seen.begin() &&
           Slot - *std::prev(Next) < arm64e::kPointerBytes) ||
          Img.hasMachOAuthenticatedPointerIn(Slot, arm64e::kPointerBytes) ||
          Img.MachOResolvedChainedPointerSlots.count(Slot))
        return std::nullopt;
      Seen.insert(Next, Slot);
      const uint8_t *Storage = Img.readVA(Slot, arm64e::kPointerBytes);
      if (!Storage)
        return std::nullopt;
      auto Fixup = decodeArm64eFixup(readLE<uint64_t>(Storage), Format, Slot,
                                     Base, Imports, Img);
      if (!Fixup)
        return std::nullopt;
      // Indirect-symbol tables may already describe this slot. Reuse the
      // image's canonical evidence merge without mutating it during planning.
      // A prior dyld record instead denotes a second fixup owner.
      auto Existing = Img.collectImportStorageSlot(Slot);
      if (!Existing.Conflicts.empty() || Img.DyldBindSlots.count(Slot))
        return std::nullopt;
      if (Fixup->Import) {
        if (!BinaryImage::mergeImportStorageSlot(
                Existing.Slots, Existing.Conflicts, Slot, Fixup->Import->Name,
                Fixup->Addend, ImportStorageEvidence::LoaderBind))
          return std::nullopt;
      } else if (!Existing.Slots.empty()) {
        return std::nullopt;
      }
      if (Fixup->Authentication) {
        const auto &Auth = *Fixup->Authentication;
        const size_t Bytes = Auth.Symbol.size() + Auth.Module.size();
        if (Bytes > limits::loader::kMachOThreadedMaxCopiedBytes - CopiedBytes)
          return std::nullopt;
        CopiedBytes += Bytes;
      }
      const uint64_t Delta = uint64_t(Fixup->Next) * Format.Stride;
      Pending.push_back(std::move(*Fixup));
      if (!Delta)
        break;
      if (Delta > InvalidVA - Offset)
        return std::nullopt;
      Offset += Delta;
    }
  }
  return Pending;
}
} // namespace

bool parseThreadedBindStreams(const uint8_t *BasePtr, size_t FileSize,
                              const DyldInfoOffsets &Info, va_t TextVMAddr,
                              BinaryImage &Img) {
  const auto fail = [&]() {
    Img.MachOChainedFixupsError = arm64e::kMalformedThreadedBinds;
    return true;
  };
  const auto bytes =
      [&](uint32_t Off,
          uint32_t Size) -> std::optional<llvm::ArrayRef<uint8_t>> {
    if (!Size)
      return llvm::ArrayRef<uint8_t>();
    if (!Off || !rangeInBounds(Off, Size, FileSize))
      return std::nullopt;
    return llvm::ArrayRef<uint8_t>(BasePtr + Off, Size);
  };
  const auto Bind = bytes(Info.BindOff, Info.BindSize);
  const auto Lazy = bytes(Info.LazyBindOff, Info.LazyBindSize);
  if (Info.Ambiguous && Img.isArm64e())
    return fail();
  if (!Bind || !Lazy)
    return Img.isArm64e() ? fail() : false;
  const auto Kind = threaded::classify(*Bind);
  const auto LazyKind = threaded::classify(*Lazy, true);
  if (LazyKind.Threaded || ((Kind.Malformed || LazyKind.Malformed) &&
                            (Kind.Threaded || Img.isArm64e())))
    return fail();
  if (!Kind.Threaded)
    return false;
  if (!Img.isArm64e() || !Img.is64Bit() || Img.IsRelocatable ||
      Img.MachOHasChainedFixups || Img.MachOChainedFixupsAmbiguous ||
      TextVMAddr == InvalidVA || !Lazy->empty() || Info.RebaseSize ||
      Info.WeakBindSize)
    return fail();
  auto Plan = threaded::decode(*Bind, Img.MachODylibReferences.size());
  if (!Plan)
    return fail();
  std::vector<Arm64eImportRecord> Imports;
  Imports.reserve(Plan->Imports.size());
  size_t CopiedBytes = Plan->CopiedBytes;
  for (auto &Record : Plan->Imports) {
    llvm::StringRef Module;
    llvm::StringRef DisplayModule = kExternModule;
    bool Weak = Record.Flags & BIND_SYMBOL_FLAGS_WEAK_IMPORT;
    if (Record.Ordinal > 0) {
      const auto &Reference = Img.MachODylibReferences[Record.Ordinal - 1];
      Module = Reference.Name;
      if (Module.empty())
        return fail();
      DisplayModule = Module;
      Weak |= Reference.Weak;
    }
    const size_t Extra = Module.size() + DisplayModule.size();
    if (Extra > limits::loader::kMachOThreadedMaxCopiedBytes - CopiedBytes)
      return fail();
    CopiedBytes += Extra;
    Imports.push_back({std::move(Record.Name), Module.str(),
                       DisplayModule.str(), Record.Addend, Weak, true,
                       Record.Ordinal});
  }
  auto Fixups = walk(*Plan, Imports, CopiedBytes, TextVMAddr, Img);
  if (!Fixups)
    return fail();

  std::set<std::string> Names;
  for (const auto &Import : Img.Imports)
    Names.insert(Import.Name);
  for (const auto &Record : Imports) {
    if (!Names.insert(Record.Name).second)
      continue;
    Import Entry;
    Entry.Name = Record.Name;
    Entry.Module = Record.DisplayModule;
    Img.Imports.push_back(std::move(Entry));
  }
  publishArm64eFixups(*Fixups, Img);
  Img.MachOHasChainedFixups = true;
  return true;
}

} // namespace detail
} // namespace neverd::macho_loader
