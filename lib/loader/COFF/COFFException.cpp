//===- COFFException.cpp - Checked PE exception decoding -----------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "neverd/loader/COFF/COFFException.h"

#include "neverd/Support/BinaryEncoding.h"

#include "llvm/ADT/StringRef.h"
#include "llvm/Support/Win64EH.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <iterator>
#include <limits>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace neverd::coff_loader {
namespace {

using namespace llvm::Win64EH;

constexpr uint32_t MaxLanguageRecords = 1u << 16;
constexpr unsigned MaxPersonalityVeneers = 8;

struct LanguageRecordBudget {
  uint64_t Remaining = MaxLanguageRecords;

  bool consume(uint64_t Count) {
    if (Count > Remaining)
      return false;
    Remaining -= Count;
    return true;
  }
};

void diagnose(ExceptionFunction &F, ExceptionParseStatus Status,
              llvm::StringRef Message) {
  F.ParseStatus = mergeExceptionParseStatus(F.ParseStatus, Status);
  F.Diagnostics.push_back(Message.str());
}

bool addRVA(va_t ImageBase, uint32_t RVA, va_t &Address) {
  if (RVA > InvalidVA - ImageBase)
    return false;
  Address = ImageBase + RVA;
  return true;
}

template <typename T>
std::optional<T> readScalar(const BinaryImage &Img, va_t Address) {
  const uint8_t *P = Img.readVA(Address, sizeof(T));
  if (!P)
    return std::nullopt;
  return readLE<T>(P);
}

bool readBytes(const BinaryImage &Img, va_t Address, size_t Size,
               const uint8_t *&Bytes) {
  Bytes = Img.readVA(Address, Size);
  return Bytes != nullptr;
}

bool isExecutableAddress(const BinaryImage &Img, va_t Address) {
  const Segment *Seg = Img.getSegmentFor(Address);
  return Seg && Seg->isExecutable() && Img.readVA(Address, 1);
}

std::optional<ExceptionAddressRange>
checkedImageRange(va_t ImageBase, uint32_t BeginRVA, uint32_t EndRVA) {
  if (EndRVA <= BeginRVA)
    return std::nullopt;
  va_t Begin = 0;
  va_t End = 0;
  if (!addRVA(ImageBase, BeginRVA, Begin) || !addRVA(ImageBase, EndRVA, End))
    return std::nullopt;
  return ExceptionAddressRange{Begin, End};
}

UnwindOperation makeV1Operation(UnwindOperationKind Kind, uint8_t CodeOffset,
                                uint8_t OpInfo, uint8_t SlotCount,
                                const uint8_t *Bytes) {
  UnwindOperation Op;
  Op.Kind = Kind;
  Op.CodeOffset = CodeOffset;
  Op.OpInfo = OpInfo;
  Op.SlotCount = SlotCount;
  Op.OperandBytes.assign(Bytes, Bytes + static_cast<size_t>(SlotCount) * 2);
  return Op;
}

bool decodeV1V2Operations(ExceptionFunction &F, const uint8_t *Codes,
                          uint8_t Count) {
  size_t I = 0;
  std::optional<uint8_t> PreviousCodeOffset;
  while (I < Count) {
    const uint8_t *Slot = Codes + I * 2;
    uint8_t CodeOffset = Slot[0];
    uint8_t Opcode = Slot[1] & 0x0f;
    uint8_t OpInfo = Slot[1] >> 4;
    uint8_t Slots = 1;
    UnwindOperationKind Kind = UnwindOperationKind::Opaque;

    const bool IsV2Epilog = F.UnwindVersion == 2 && Opcode == UOP_Epilog;
    if ((!IsV2Epilog && CodeOffset > F.PrologueSize) ||
        (F.UnwindVersion == 1 && PreviousCodeOffset &&
         CodeOffset >= *PreviousCodeOffset)) {
      diagnose(F, ExceptionParseStatus::Malformed,
               "invalid x64 unwind-code ordering or prologue offset");
      return false;
    }
    PreviousCodeOffset = CodeOffset;

    auto RequireSlots = [&](uint8_t Required) {
      if (Required > Count - I) {
        diagnose(F, ExceptionParseStatus::Malformed,
                 "truncated x64 unwind operation operands");
        return false;
      }
      Slots = Required;
      return true;
    };

    switch (Opcode) {
    case UOP_PushNonVol:
      Kind = UnwindOperationKind::PushNonVolatile;
      break;
    case UOP_AllocLarge:
      Kind = UnwindOperationKind::AllocateLarge;
      if (OpInfo == 0) {
        if (!RequireSlots(2))
          return false;
      } else if (OpInfo == 1) {
        if (!RequireSlots(3))
          return false;
      } else {
        diagnose(F, ExceptionParseStatus::Malformed,
                 "invalid UWOP_ALLOC_LARGE operand form");
        return false;
      }
      break;
    case UOP_AllocSmall:
      Kind = UnwindOperationKind::AllocateSmall;
      break;
    case UOP_SetFPReg:
      Kind = UnwindOperationKind::SetFramePointer;
      if (OpInfo != 0)
        diagnose(F, ExceptionParseStatus::Partial,
                 "non-zero UWOP_SET_FPREG operation info");
      break;
    case UOP_SaveNonVol:
      Kind = UnwindOperationKind::SaveNonVolatile;
      if (!RequireSlots(2))
        return false;
      break;
    case UOP_SaveNonVolBig:
      Kind = UnwindOperationKind::SaveNonVolatileFar;
      if (!RequireSlots(3))
        return false;
      break;
    case UOP_Epilog:
      if (F.UnwindVersion == 2)
        Kind = UnwindOperationKind::Epilog;
      else
        diagnose(F, ExceptionParseStatus::Partial,
                 "reserved x64 v1 unwind opcode 6");
      break;
    case UOP_SpareCode:
      if (F.UnwindVersion == 2)
        Kind = UnwindOperationKind::Spare;
      else
        diagnose(F, ExceptionParseStatus::Partial,
                 "reserved x64 v1 unwind opcode 7");
      break;
    case UOP_SaveXMM128:
      Kind = UnwindOperationKind::SaveXMM128;
      if (!RequireSlots(2))
        return false;
      break;
    case UOP_SaveXMM128Big:
      Kind = UnwindOperationKind::SaveXMM128Far;
      if (!RequireSlots(3))
        return false;
      break;
    case UOP_PushMachFrame:
      Kind = UnwindOperationKind::PushMachineFrame;
      if (OpInfo > 1) {
        diagnose(F, ExceptionParseStatus::Malformed,
                 "invalid UWOP_PUSH_MACHFRAME operation info");
        return false;
      }
      break;
    default:
      diagnose(F, ExceptionParseStatus::Partial, "unknown x64 unwind opcode");
      break;
    }

    UnwindOperation Op = makeV1Operation(Kind, CodeOffset, OpInfo, Slots, Slot);
    switch (Opcode) {
    case UOP_PushNonVol:
      Op.Register = OpInfo;
      break;
    case UOP_AllocLarge:
      Op.StackOffset = OpInfo == 0 ? uint64_t(readLE<uint16_t>(Slot + 2)) * 8
                                   : readLE<uint32_t>(Slot + 2);
      break;
    case UOP_AllocSmall:
      Op.StackOffset = uint64_t(OpInfo + 1) * 8;
      break;
    case UOP_SaveNonVol:
      Op.Register = OpInfo;
      Op.StackOffset = uint64_t(readLE<uint16_t>(Slot + 2)) * 8;
      break;
    case UOP_SaveNonVolBig:
      Op.Register = OpInfo;
      Op.StackOffset = readLE<uint32_t>(Slot + 2);
      break;
    case UOP_Epilog:
      if (F.UnwindVersion == 2)
        Op.StackOffset = (uint32_t(OpInfo) << 8) | CodeOffset;
      break;
    case UOP_SaveXMM128:
      Op.Register = OpInfo;
      Op.StackOffset = uint64_t(readLE<uint16_t>(Slot + 2)) * 16;
      break;
    case UOP_SaveXMM128Big:
      Op.Register = OpInfo;
      Op.StackOffset = readLE<uint32_t>(Slot + 2);
      break;
    default:
      break;
    }
    F.UnwindOperations.push_back(std::move(Op));
    I += Slots;
  }
  return true;
}

struct V3EpilogDescriptor {
  UnwindEpilog Epilog;
  uint8_t OperationCount = 0;
  std::vector<uint32_t> IPOffsets;
  bool Inherited = false;
};

bool decodeV3WODs(ExceptionFunction &F, const uint8_t *Pool, size_t PoolSize,
                  uint32_t FirstOffset, uint8_t Count,
                  const std::vector<uint32_t> &IPOffsets,
                  std::vector<UnwindOperation> &Out) {
  if (IPOffsets.size() != Count || FirstOffset > PoolSize) {
    diagnose(F, ExceptionParseStatus::Malformed,
             "invalid x64 v3 WOD/IP-offset range");
    return false;
  }

  size_t Offset = FirstOffset;
  for (uint8_t I = 0; I < Count; ++I) {
    if (Offset >= PoolSize) {
      diagnose(F, ExceptionParseStatus::Malformed, "truncated x64 v3 WOD pool");
      return false;
    }
    const uint8_t *WOD = Pool + Offset;
    uint8_t B0 = WOD[0];
    size_t Size = 0;
    UnwindOperation Op;
    Op.CodeOffset = IPOffsets[I];

    if ((B0 & 0x07) == 4) {
      Size = 1;
      Op.Kind = UnwindOperationKind::PushNonVolatile;
      Op.Register = B0 >> 3;
    } else if ((B0 & 0x07) == 5) {
      Size = 5;
      Op.Kind = UnwindOperationKind::SaveNonVolatileFar;
      Op.Register = B0 >> 3;
    } else if ((B0 & 0x07) == 6) {
      Size = 3;
      Op.Kind = UnwindOperationKind::SaveNonVolatile;
      Op.Register = B0 >> 3;
    } else if ((B0 & 0x07) == 7) {
      Size = 1;
      Op.Kind = UnwindOperationKind::PushConsecutiveRegisters;
      Op.Register = B0 >> 3;
      if (Op.Register == 31) {
        diagnose(F, ExceptionParseStatus::Malformed,
                 "x64 v3 consecutive push exceeds register file");
        return false;
      }
    } else if ((B0 & 0x0f) == 8) {
      Size = 1;
      Op.Kind = UnwindOperationKind::AllocateSmall;
      Op.StackOffset = uint64_t((B0 >> 4) + 1) * 8;
    } else if ((B0 & 0x0f) == 9) {
      Size = 5;
      Op.Kind = UnwindOperationKind::SaveXMM128Far;
      Op.Register = B0 >> 4;
    } else if ((B0 & 0x0f) == 10) {
      Size = 3;
      Op.Kind = UnwindOperationKind::SaveXMM128;
      Op.Register = B0 >> 4;
    } else if ((B0 & 0x3f) == 0x20) {
      Size = 2;
      Op.Kind = UnwindOperationKind::PushTwoRegisters;
    } else {
      switch (B0) {
      case 0:
        Size = 2;
        Op.Kind = UnwindOperationKind::SetFramePointer;
        break;
      case 1:
        Size = 5;
        Op.Kind = UnwindOperationKind::AllocateHuge;
        break;
      case 2:
        Size = 3;
        Op.Kind = UnwindOperationKind::AllocateLarge;
        break;
      case 3:
        Size = 2;
        Op.Kind = UnwindOperationKind::PushCanonicalFrame;
        break;
      default:
        diagnose(F, ExceptionParseStatus::Partial,
                 "unknown x64 v3 winding operation descriptor");
        return false;
      }
    }

    if (Size > PoolSize - Offset) {
      diagnose(F, ExceptionParseStatus::Malformed,
               "truncated x64 v3 winding operation descriptor");
      return false;
    }
    Op.SlotCount = static_cast<uint8_t>(Size);
    Op.OperandBytes.assign(WOD, WOD + Size);

    switch (Op.Kind) {
    case UnwindOperationKind::PushTwoRegisters:
      Op.Register = uint16_t((B0 >> 6) | ((WOD[1] & 0x07) << 2));
      Op.OpInfo = WOD[1] >> 3;
      break;
    case UnwindOperationKind::SetFramePointer:
      Op.Register = WOD[1] & 0x0f;
      Op.StackOffset = uint64_t(WOD[1] >> 4) * 16;
      break;
    case UnwindOperationKind::AllocateHuge:
      Op.StackOffset = readLE<uint32_t>(WOD + 1);
      break;
    case UnwindOperationKind::AllocateLarge:
      Op.StackOffset = uint64_t(readLE<uint16_t>(WOD + 1)) * 8;
      break;
    case UnwindOperationKind::SaveNonVolatile:
      Op.StackOffset = uint64_t(readLE<uint16_t>(WOD + 1)) * 8;
      break;
    case UnwindOperationKind::SaveNonVolatileFar:
      Op.StackOffset = readLE<uint32_t>(WOD + 1);
      break;
    case UnwindOperationKind::SaveXMM128:
      Op.StackOffset = uint64_t(readLE<uint16_t>(WOD + 1)) * 16;
      break;
    case UnwindOperationKind::SaveXMM128Far:
      Op.StackOffset = readLE<uint32_t>(WOD + 1);
      break;
    case UnwindOperationKind::PushCanonicalFrame:
      Op.OpInfo = WOD[1];
      break;
    default:
      break;
    }

    Out.push_back(std::move(Op));
    Offset += Size;
  }
  return true;
}

bool decodeV3(ExceptionFunction &F, const BinaryImage &Img, va_t UnwindVA,
              const uint8_t Header[4], va_t &TrailingDataVA) {
  constexpr uint8_t LargeFlag = 0x08;
  constexpr uint8_t ReservedFlag = 0x10;

  const uint8_t PayloadWords = Header[2];
  const uint8_t PrologOps = Header[3] & 0x1f;
  const uint8_t EpilogCount = Header[3] >> 5;
  const uint64_t PayloadSize = uint64_t(PayloadWords) * 2;
  if (PayloadSize > std::numeric_limits<size_t>::max() ||
      UnwindVA > InvalidVA - 4) {
    diagnose(F, ExceptionParseStatus::Malformed,
             "x64 v3 payload size overflows");
    return false;
  }
  const uint8_t *Payload = nullptr;
  if (!readBytes(Img, UnwindVA + 4, static_cast<size_t>(PayloadSize),
                 Payload)) {
    diagnose(F, ExceptionParseStatus::Malformed,
             "truncated x64 v3 unwind payload");
    return false;
  }
  if (F.UnwindFlags & ReservedFlag)
    diagnose(F, ExceptionParseStatus::Malformed,
             "reserved x64 v3 unwind flag is set");

  size_t Cursor = 0;
  const bool Large = (F.UnwindFlags & LargeFlag) != 0;
  if (Large) {
    if (Cursor == PayloadSize) {
      diagnose(F, ExceptionParseStatus::Malformed,
               "missing x64 v3 large-prologue extension");
      return false;
    }
    F.PrologueSize |= uint32_t(Payload[Cursor++]) << 8;
  }

  const size_t PrologIPWidth = Large ? 2 : 1;
  if (uint64_t(PrologOps) * PrologIPWidth > PayloadSize - Cursor) {
    diagnose(F, ExceptionParseStatus::Malformed,
             "truncated x64 v3 prologue IP offsets");
    return false;
  }
  std::vector<uint32_t> PrologIPOffsets;
  PrologIPOffsets.reserve(PrologOps);
  for (uint8_t I = 0; I < PrologOps; ++I) {
    uint32_t Offset =
        Large ? readLE<uint16_t>(Payload + Cursor) : Payload[Cursor];
    Cursor += PrologIPWidth;
    if (Offset > F.PrologueSize) {
      diagnose(F, ExceptionParseStatus::Malformed,
               "x64 v3 prologue IP offset exceeds prologue");
      return false;
    }
    if (!PrologIPOffsets.empty() && Offset >= PrologIPOffsets.back()) {
      diagnose(F, ExceptionParseStatus::Malformed,
               "x64 v3 prologue IP offsets are not strictly descending");
      return false;
    }
    PrologIPOffsets.push_back(Offset);
  }

  if (F.PrologueSize > F.CodeRange.size()) {
    diagnose(F, ExceptionParseStatus::Malformed,
             "x64 v3 prologue leaves its runtime function");
    return false;
  }

  std::vector<V3EpilogDescriptor> Descriptors;
  Descriptors.reserve(EpilogCount);
  std::optional<size_t> LastFullDescriptor;
  int64_t PreviousStart = 0;
  bool DescendingEpilogs = false;
  const int64_t FunctionSize = static_cast<int64_t>(F.CodeRange.size());
  for (uint8_t I = 0; I < EpilogCount; ++I) {
    if (PayloadSize - Cursor < 3) {
      diagnose(F, ExceptionParseStatus::Malformed,
               "truncated x64 v3 epilogue descriptor");
      return false;
    }
    uint8_t First = Payload[Cursor++];
    uint8_t Flags = First & 0x07;
    uint8_t NumOps = First >> 3;
    int16_t Delta = readLE<int16_t>(Payload + Cursor);
    Cursor += 2;
    if (Flags & 0x04) {
      diagnose(F, ExceptionParseStatus::Malformed,
               "reserved x64 v3 epilogue flag is set");
      return false;
    }
    if (I == 0) {
      DescendingEpilogs = Delta < 0;
    } else if (Delta == 0 || (Delta < 0) != DescendingEpilogs) {
      diagnose(F, ExceptionParseStatus::Malformed,
               "x64 v3 epilogue offsets do not use one strict direction");
      return false;
    }

    V3EpilogDescriptor Desc;
    Desc.Epilog.Flags = Flags;
    int64_t Start = 0;
    if (I == 0)
      Start = Delta >= 0 ? Delta : FunctionSize + Delta;
    else
      Start = PreviousStart + Delta;
    if (Start < 0 || Start >= FunctionSize) {
      diagnose(F, ExceptionParseStatus::Malformed,
               "x64 v3 epilogue offset leaves its function");
      return false;
    }
    Desc.Epilog.StartOffset = Start;
    PreviousStart = Start;

    if (NumOps == 0) {
      if (!LastFullDescriptor) {
        diagnose(F, ExceptionParseStatus::Malformed,
                 "x64 v3 epilogue inherits without a full predecessor");
        return false;
      }
      const V3EpilogDescriptor &Full = Descriptors[*LastFullDescriptor];
      if ((Flags & 3u) != (Full.Epilog.Flags & 3u)) {
        diagnose(F, ExceptionParseStatus::Malformed,
                 "x64 v3 inherited epilogue changes effective flags");
        return false;
      }
      Desc.Inherited = true;
      Desc.OperationCount = Full.OperationCount;
      Desc.Epilog.FirstOperationOffset = Full.Epilog.FirstOperationOffset;
      Desc.Epilog.LastInstructionOffset = Full.Epilog.LastInstructionOffset;
      Desc.IPOffsets = Full.IPOffsets;
      Descriptors.push_back(std::move(Desc));
      continue;
    }

    Desc.OperationCount = NumOps;
    const bool LargeEpilog = (Flags & 0x02) != 0;
    const size_t ExtensionSize = LargeEpilog ? 4 : 3;
    const size_t IPWidth = LargeEpilog ? 2 : 1;
    const uint64_t Needed =
        uint64_t(ExtensionSize) + uint64_t(NumOps) * IPWidth;
    if (Needed > PayloadSize - Cursor) {
      diagnose(F, ExceptionParseStatus::Malformed,
               "truncated x64 v3 epilogue extension");
      return false;
    }
    Desc.Epilog.FirstOperationOffset = readLE<uint16_t>(Payload + Cursor);
    Cursor += 2;
    Desc.Epilog.LastInstructionOffset =
        LargeEpilog ? readLE<uint16_t>(Payload + Cursor) : Payload[Cursor];
    Cursor += LargeEpilog ? 2 : 1;
    Desc.IPOffsets.reserve(NumOps);
    for (uint8_t Op = 0; Op < NumOps; ++Op) {
      uint32_t IPOffset =
          LargeEpilog ? readLE<uint16_t>(Payload + Cursor) : Payload[Cursor];
      Cursor += IPWidth;
      if (IPOffset > Desc.Epilog.LastInstructionOffset ||
          (!Desc.IPOffsets.empty() && IPOffset <= Desc.IPOffsets.back())) {
        diagnose(F, ExceptionParseStatus::Malformed,
                 "x64 v3 epilogue IP offsets are invalid");
        return false;
      }
      Desc.IPOffsets.push_back(IPOffset);
    }
    Descriptors.push_back(std::move(Desc));
    LastFullDescriptor = Descriptors.size() - 1;
  }

  const uint8_t *Pool = Payload + Cursor;
  const size_t PoolSize = static_cast<size_t>(PayloadSize) - Cursor;
  if (!decodeV3WODs(F, Pool, PoolSize, 0, PrologOps, PrologIPOffsets,
                    F.UnwindOperations))
    return false;

  for (V3EpilogDescriptor &Desc : Descriptors) {
    if (Desc.Epilog.LastInstructionOffset >=
        static_cast<uint64_t>(FunctionSize - Desc.Epilog.StartOffset)) {
      diagnose(F, ExceptionParseStatus::Malformed,
               "x64 v3 epilogue leaves its runtime function");
      return false;
    }
    // Inherited descriptors already copied their effective fields and IP
    // offsets from the most recent full record.  Re-decode from the shared WOD
    // pool so every normalized epilogue owns a stable operation vector.
    if (!decodeV3WODs(F, Pool, PoolSize, Desc.Epilog.FirstOperationOffset,
                      Desc.OperationCount, Desc.IPOffsets,
                      Desc.Epilog.Operations))
      return false;
    F.Epilogs.push_back(std::move(Desc.Epilog));
  }

  uint64_t TrailingOffset = alignUp(4 + PayloadSize, 4);
  if (TrailingOffset > InvalidVA - UnwindVA) {
    diagnose(F, ExceptionParseStatus::Malformed,
             "x64 v3 trailing-data offset overflows");
    return false;
  }
  TrailingDataVA = UnwindVA + TrailingOffset;
  return true;
}

bool parseTrailingData(ExceptionFunction &F, const BinaryImage &Img,
                       va_t ImageBase, va_t TrailingDataVA) {
  const uint8_t HandlerMask = UNW_ExceptionHandler | UNW_TerminateHandler;
  const bool HasHandler = (F.UnwindFlags & HandlerMask) != 0;
  const bool HasChain = (F.UnwindFlags & UNW_ChainInfo) != 0;
  if (HasHandler && HasChain) {
    diagnose(F, ExceptionParseStatus::Malformed,
             "x64 unwind record combines handler and chain flags");
    return false;
  }
  if (!HasHandler && !HasChain)
    return true;

  if (HasChain) {
    const uint8_t *Bytes = nullptr;
    if (!readBytes(Img, TrailingDataVA, 12, Bytes)) {
      diagnose(F, ExceptionParseStatus::Malformed,
               "truncated chained x64 runtime function");
      return false;
    }
    uint32_t BeginRVA = readLE<uint32_t>(Bytes);
    uint32_t EndRVA = readLE<uint32_t>(Bytes + 4);
    F.ChainedUnwindInfoRVA = readLE<uint32_t>(Bytes + 8);
    F.ChainedPrimaryRange = checkedImageRange(ImageBase, BeginRVA, EndRVA);
    if (!F.ChainedPrimaryRange) {
      diagnose(F, ExceptionParseStatus::Malformed,
               "invalid chained x64 runtime-function range");
      return false;
    }
    F.Kind = RuntimeFunctionKind::Chained;
    return true;
  }

  auto HandlerRVA = readScalar<uint32_t>(Img, TrailingDataVA);
  if (!HandlerRVA || *HandlerRVA == 0 ||
      !addRVA(ImageBase, *HandlerRVA, F.PersonalityVA)) {
    diagnose(F, ExceptionParseStatus::Malformed, "invalid x64 personality RVA");
    return false;
  }
  if (TrailingDataVA > InvalidVA - sizeof(uint32_t)) {
    diagnose(F, ExceptionParseStatus::Malformed,
             "x64 handler-data address overflows");
    return false;
  }
  F.HandlerDataVA = TrailingDataVA + sizeof(uint32_t);
  F.Personality = ExceptionPersonality::Unknown;
  if (!isExecutableAddress(Img, F.PersonalityVA))
    diagnose(F, ExceptionParseStatus::Partial,
             "x64 personality RVA is not mapped executable code");
  return true;
}

std::string directNameAt(const BinaryImage &Img, va_t Address) {
  if (const Import *Imp = Img.findImportAt(Address); Imp && !Imp->Name.empty())
    return Imp->Name;
  if (const Export *Exp = Img.findExportAt(Address); Exp && !Exp->Name.empty())
    return Exp->Name;
  if (const Symbol *Sym = Img.findSymbolAt(Address); Sym && !Sym->Name.empty())
    return Sym->Name;
  return {};
}

std::optional<va_t> addSignedOffset(va_t Base, int64_t Offset) {
  if (Offset < 0) {
    uint64_t Distance = static_cast<uint64_t>(-(Offset + 1)) + 1;
    if (Distance > Base)
      return std::nullopt;
    return Base - Distance;
  }
  if (static_cast<uint64_t>(Offset) > InvalidVA - Base)
    return std::nullopt;
  return Base + static_cast<uint64_t>(Offset);
}

int64_t signExtend(uint64_t Value, unsigned Bits) {
  uint64_t Sign = uint64_t(1) << (Bits - 1);
  return static_cast<int64_t>((Value ^ Sign) - Sign);
}

std::optional<va_t> decodeAArch64Veneer(const BinaryImage &Img, va_t Current,
                                        std::string &ImportName) {
  const uint8_t *Bytes = Img.readVA(Current, 12);
  if (!Bytes)
    return std::nullopt;
  uint32_t First = readLE<uint32_t>(Bytes);

  // Direct B imm26 veneer.
  if ((First & 0xfc000000u) == 0x14000000u) {
    int64_t Disp = signExtend(uint64_t(First & 0x03ffffffu) << 2, 28);
    return addSignedOffset(Current, Disp);
  }

  uint32_t Second = readLE<uint32_t>(Bytes + 4);
  uint32_t Third = readLE<uint32_t>(Bytes + 8);
  if ((First & 0x9f000000u) == 0x90000000u) {
    unsigned Reg = First & 0x1fu;
    uint64_t Imm21 = ((First >> 29) & 3u) | ((First >> 3) & 0x1ffffcu);
    int64_t PageDisp = signExtend(Imm21, 21) << 12;
    auto Page = addSignedOffset(Current & ~va_t(0xfff), PageDisp);
    if (!Page)
      return std::nullopt;

    // adrp Xn; ldr Xt, [Xn, #imm12*8]; br Xt
    if ((Second & 0xffc00000u) == 0xf9400000u &&
        ((Second >> 5) & 0x1fu) == Reg &&
        (Third & 0xfffffc1fu) == 0xd61f0000u &&
        ((Third >> 5) & 0x1fu) == (Second & 0x1fu)) {
      uint64_t Offset = uint64_t((Second >> 10) & 0xfffu) * 8;
      if (Offset > InvalidVA - *Page)
        return std::nullopt;
      va_t Slot = *Page + Offset;
      ImportName = directNameAt(Img, Slot);
      if (!ImportName.empty())
        return Current;
      if (auto Target = readScalar<uint64_t>(Img, Slot))
        return *Target;
      return std::nullopt;
    }

    // adrp Xn; add Xn, Xn, #imm12{, lsl #12}; br Xn
    if ((Second & 0xff000000u) == 0x91000000u && (Second & 0x1fu) == Reg &&
        ((Second >> 5) & 0x1fu) == Reg &&
        (Third & 0xfffffc1fu) == 0xd61f0000u && ((Third >> 5) & 0x1fu) == Reg) {
      uint64_t Offset = (Second >> 10) & 0xfffu;
      if ((Second & (1u << 22)) != 0)
        Offset <<= 12;
      if (Offset <= InvalidVA - *Page)
        return *Page + Offset;
    }
  }

  // ldr Xt, literal; br Xt.  Import libraries sometimes name the literal IAT
  // slot, while static veneers contain an absolute target pointer there.
  if ((First & 0xff000000u) == 0x58000000u &&
      (Second & 0xfffffc1fu) == 0xd61f0000u &&
      ((Second >> 5) & 0x1fu) == (First & 0x1fu)) {
    int64_t Disp = signExtend(uint64_t((First >> 5) & 0x7ffffu) << 2, 21);
    auto Slot = addSignedOffset(Current, Disp);
    if (!Slot)
      return std::nullopt;
    ImportName = directNameAt(Img, *Slot);
    if (!ImportName.empty())
      return Current;
    if (auto Target = readScalar<uint64_t>(Img, *Slot))
      return *Target;
  }
  return std::nullopt;
}

std::optional<va_t> decodeARMBranchVeneer(const BinaryImage &Img,
                                          va_t Current) {
  bool Thumb = (Current & 1u) != 0;
  va_t CodeVA = Current & ~va_t(1);
  if (!Thumb) {
    const uint8_t *Code = Img.readVA(CodeVA, 4);
    if (!Code)
      return std::nullopt;
    uint32_t Insn = readLE<uint32_t>(Code);
    if ((Insn & 0x0f000000u) != 0x0a000000u)
      return std::nullopt;
    int64_t Disp = signExtend(uint64_t(Insn & 0x00ffffffu) << 2, 26);
    return addSignedOffset(CodeVA + 8, Disp);
  }

  const uint8_t *Code = Img.readVA(CodeVA, 4);
  if (!Code)
    return std::nullopt;
  uint16_t First = readLE<uint16_t>(Code);
  if ((First & 0xf800u) == 0xe000u) {
    int64_t Disp = signExtend(uint64_t(First & 0x7ffu) << 1, 12);
    auto Target = addSignedOffset(CodeVA + 4, Disp);
    return Target ? std::optional<va_t>(*Target | 1u) : std::nullopt;
  }
  uint16_t Second = readLE<uint16_t>(Code + 2);
  if ((First & 0xf800u) != 0xf000u || (Second & 0xd000u) != 0x9000u)
    return std::nullopt;
  uint32_t S = (First >> 10) & 1u;
  uint32_t J1 = (Second >> 13) & 1u;
  uint32_t J2 = (Second >> 11) & 1u;
  uint32_t I1 = !(J1 ^ S);
  uint32_t I2 = !(J2 ^ S);
  uint32_t Imm25 = (S << 24) | (I1 << 23) | (I2 << 22) |
                   ((First & 0x3ffu) << 12) | ((Second & 0x7ffu) << 1);
  auto Target = addSignedOffset(CodeVA + 4, signExtend(Imm25, 25));
  return Target ? std::optional<va_t>(*Target | 1u) : std::nullopt;
}

std::pair<va_t, std::string> resolvePersonality(const BinaryImage &Img,
                                                va_t Start) {
  va_t Current = Start;
  std::vector<va_t> Seen;
  for (unsigned Depth = 0; Depth < MaxPersonalityVeneers; ++Depth) {
    if (std::find(Seen.begin(), Seen.end(), Current) != Seen.end())
      break;
    Seen.push_back(Current);
    if (std::string Name = directNameAt(Img, Current); !Name.empty())
      return {Current, std::move(Name)};

    if (Img.Arch == Arch::AArch64) {
      std::string ImportName;
      auto Target = decodeAArch64Veneer(Img, Current, ImportName);
      if (!ImportName.empty())
        return {Current, std::move(ImportName)};
      if (!Target)
        break;
      Current = *Target;
      continue;
    }
    if (Img.Arch == Arch::ARM) {
      if (std::string Name = directNameAt(Img, Current & ~va_t(1));
          !Name.empty())
        return {Current & ~va_t(1), std::move(Name)};
      auto Target = decodeARMBranchVeneer(Img, Current);
      if (!Target)
        break;
      Current = *Target;
      continue;
    }
    if (Img.Arch != Arch::X64)
      break;
    const uint8_t *Code = Img.readVA(Current, 6);
    if (!Code || Current > InvalidVA - 6)
      break;
    if (Code[0] == 0xe9) {
      int32_t Rel = readLE<int32_t>(Code + 1);
      va_t NextIP = Current + 5;
      auto Target = addSignedOffset(NextIP, Rel);
      if (!Target)
        break;
      Current = *Target;
      continue;
    }
    if (Code[0] == 0xff && Code[1] == 0x25) {
      int32_t Disp = readLE<int32_t>(Code + 2);
      va_t NextIP = Current + 6;
      auto Slot = addSignedOffset(NextIP, Disp);
      if (!Slot)
        break;
      if (std::string Name = directNameAt(Img, *Slot); !Name.empty())
        return {Current, std::move(Name)};
    }
    break;
  }
  return {Current, {}};
}

ExceptionPersonality classifyPersonality(llvm::StringRef Name) {
  if (size_t Bang = Name.rfind('!'); Bang != llvm::StringRef::npos)
    Name = Name.drop_front(Bang + 1);
  while (Name.consume_front("__imp_"))
    ;
  while (Name.consume_front("_"))
    ;
  if (size_t At = Name.find('@'); At != llvm::StringRef::npos)
    Name = Name.take_front(At);

  if (Name == "C_specific_handler")
    return ExceptionPersonality::CSpecificHandler;
  if (Name == "CxxFrameHandler3")
    return ExceptionPersonality::CxxFrameHandler3;
  if (Name == "CxxFrameHandler4")
    return ExceptionPersonality::CxxFrameHandler4;
  if (Name == "GSHandlerCheck_SEH")
    return ExceptionPersonality::GSHandlerCheckSEH;
  if (Name == "GSHandlerCheck_EH")
    return ExceptionPersonality::GSHandlerCheckEH;
  if (Name == "GSHandlerCheck_EH4")
    return ExceptionPersonality::GSHandlerCheckEH4;
  return ExceptionPersonality::Unknown;
}

bool parseSEH(ExceptionFunction &F, const BinaryImage &Img) {
  auto Count = readScalar<uint32_t>(Img, F.HandlerDataVA);
  if (!Count || *Count > MaxLanguageRecords) {
    diagnose(F, ExceptionParseStatus::Malformed,
             "invalid __C_specific_handler scope count");
    return false;
  }
  const uint64_t ByteSize = uint64_t(*Count) * 16;
  if (ByteSize > std::numeric_limits<size_t>::max() ||
      F.HandlerDataVA > InvalidVA - 4) {
    diagnose(F, ExceptionParseStatus::Malformed,
             "SEH scope table size overflows");
    return false;
  }
  const uint8_t *Records = nullptr;
  if (!readBytes(Img, F.HandlerDataVA + 4, static_cast<size_t>(ByteSize),
                 Records)) {
    diagnose(F, ExceptionParseStatus::Malformed,
             "truncated __C_specific_handler scope table");
    return false;
  }

  SEHExceptionInfo Info;
  Info.Scopes.reserve(*Count);
  for (uint32_t I = 0; I < *Count; ++I) {
    const uint8_t *R = Records + uint64_t(I) * 16;
    uint32_t BeginRVA = readLE<uint32_t>(R);
    uint32_t EndRVA = readLE<uint32_t>(R + 4);
    uint32_t FilterRVA = readLE<uint32_t>(R + 8);
    uint32_t JumpRVA = readLE<uint32_t>(R + 12);
    auto Range = checkedImageRange(Img.Base, BeginRVA, EndRVA);
    if (!Range || !F.CodeRange.contains(*Range)) {
      diagnose(F, ExceptionParseStatus::Malformed,
               "SEH guarded range leaves its runtime function");
      continue;
    }

    SEHScopeRecord Scope;
    Scope.GuardedRange = *Range;
    if (JumpRVA == 0) {
      Scope.Kind = SEHScopeKind::Finally;
      if (FilterRVA == 0 ||
          !addRVA(Img.Base, FilterRVA, Scope.FilterOrFinallyVA) ||
          !isExecutableAddress(Img, Scope.FilterOrFinallyVA)) {
        Scope.ParseStatus = ExceptionParseStatus::Malformed;
        diagnose(F, ExceptionParseStatus::Malformed,
                 "invalid SEH finally target");
        continue;
      }
      Scope.HandlerVA = Scope.FilterOrFinallyVA;
    } else {
      Scope.Kind =
          FilterRVA == 1 ? SEHScopeKind::CatchAll : SEHScopeKind::Filter;
      if (FilterRVA > 1 &&
          (!addRVA(Img.Base, FilterRVA, Scope.FilterOrFinallyVA) ||
           !isExecutableAddress(Img, Scope.FilterOrFinallyVA))) {
        Scope.ParseStatus = ExceptionParseStatus::Malformed;
        diagnose(F, ExceptionParseStatus::Malformed,
                 "invalid SEH filter target");
        continue;
      }
      if (!addRVA(Img.Base, JumpRVA, Scope.HandlerVA) ||
          !isExecutableAddress(Img, Scope.HandlerVA)) {
        Scope.ParseStatus = ExceptionParseStatus::Malformed;
        diagnose(F, ExceptionParseStatus::Malformed,
                 "invalid SEH handler target");
        continue;
      }
      Scope.ContinuationVA = Scope.HandlerVA;
    }
    Info.Scopes.push_back(std::move(Scope));
  }
  F.SEH = std::move(Info);
  return F.ParseStatus != ExceptionParseStatus::Malformed;
}

bool readRVAField(va_t ImageBase, const uint8_t *P, va_t &Out) {
  uint32_t RVA = readLE<uint32_t>(P);
  if (RVA == 0) {
    Out = 0;
    return true;
  }
  return addRVA(ImageBase, RVA, Out);
}

class FH4Reader {
public:
  FH4Reader(const BinaryImage &Image, va_t Address)
      : Img(Image), Cursor(Address) {}

  va_t position() const { return Cursor; }

  bool readByte(uint8_t &Value) {
    if (BytesRead >= MaxBytes || Cursor == InvalidVA)
      return false;
    const uint8_t *P = Img.readVA(Cursor, 1);
    if (!P)
      return false;
    Value = *P;
    ++Cursor;
    ++BytesRead;
    return true;
  }

  bool readUInt32(uint32_t &Value) {
    if (BytesRead > MaxBytes - sizeof(uint32_t) ||
        Cursor > InvalidVA - sizeof(uint32_t))
      return false;
    const uint8_t *P = Img.readVA(Cursor, sizeof(uint32_t));
    if (!P)
      return false;
    Value = readLE<uint32_t>(P);
    Cursor += sizeof(uint32_t);
    BytesRead += sizeof(uint32_t);
    return true;
  }

  /// FH4 integers use a low-bit length tag: 0, 01, 011, 0111, or
  /// 1111 followed by a full little-endian word.  Reject overlong forms so
  /// corrupt data has one deterministic interpretation.
  bool readCompressedUInt(uint32_t &Value) {
    uint8_t First = 0;
    if (!readByte(First))
      return false;
    unsigned Extra = 0;
    unsigned InitialShift = 0;
    uint32_t Minimum = 0;
    if ((First & 1u) == 0) {
      Value = First >> 1;
      return true;
    }
    if ((First & 3u) == 1) {
      Extra = 1;
      InitialShift = 2;
      Minimum = 1u << 7;
    } else if ((First & 7u) == 3) {
      Extra = 2;
      InitialShift = 3;
      Minimum = 1u << 14;
    } else if ((First & 15u) == 7) {
      Extra = 3;
      InitialShift = 4;
      Minimum = 1u << 21;
    } else {
      if (First != 15)
        return false;
      if (!readUInt32(Value))
        return false;
      return Value >= (1u << 28);
    }

    Value = First >> InitialShift;
    unsigned Shift = 8 - InitialShift;
    for (unsigned I = 0; I < Extra; ++I, Shift += 8) {
      uint8_t Next = 0;
      if (!readByte(Next))
        return false;
      Value |= uint32_t(Next) << Shift;
    }
    return Value >= Minimum;
  }

private:
  static constexpr uint64_t MaxBytes = 1u << 20;
  const BinaryImage &Img;
  va_t Cursor = 0;
  uint64_t BytesRead = 0;
};

bool readFH4ImageAddress(FH4Reader &Reader, va_t ImageBase, va_t &Address,
                         bool AllowZero = true) {
  uint32_t RVA = 0;
  if (!Reader.readUInt32(RVA) || (!AllowZero && RVA == 0))
    return false;
  if (RVA == 0) {
    Address = 0;
    return true;
  }
  return addRVA(ImageBase, RVA, Address);
}

bool parseFH4HandlerMap(ExceptionFunction &F, const BinaryImage &Img,
                        va_t HandlerMapVA, CxxTryBlock &Try,
                        LanguageRecordBudget &Budget) {
  FH4Reader Reader(Img, HandlerMapVA);
  uint32_t Count = 0;
  if (!Reader.readCompressedUInt(Count) || Count > MaxLanguageRecords) {
    diagnose(F, ExceptionParseStatus::Malformed,
             "invalid FH4 handler-map count");
    return false;
  }
  if (!Budget.consume(Count)) {
    diagnose(F, ExceptionParseStatus::Partial,
             "FH4 aggregate language graph exceeds decode budget");
    return false;
  }
  Try.Handlers.reserve(Count);
  for (uint32_t I = 0; I < Count; ++I) {
    uint8_t Header = 0;
    if (!Reader.readByte(Header) || (Header & 0xc0u) != 0) {
      diagnose(F, ExceptionParseStatus::Malformed,
               "invalid FH4 handler header");
      return false;
    }
    unsigned ContinuationCount = (Header >> 4) & 3u;
    if (ContinuationCount == 3) {
      diagnose(F, ExceptionParseStatus::Malformed,
               "reserved FH4 continuation count");
      return false;
    }

    CxxCatchHandler Catch;
    uint32_t Value = 0;
    if ((Header & 1u) != 0) {
      if (!Reader.readCompressedUInt(Catch.Adjectives)) {
        diagnose(F, ExceptionParseStatus::Malformed,
                 "truncated FH4 catch adjectives");
        return false;
      }
    }
    if ((Header & 2u) != 0) {
      if (!readFH4ImageAddress(Reader, Img.Base, Catch.TypeDescriptorVA)) {
        diagnose(F, ExceptionParseStatus::Malformed,
                 "invalid FH4 type-descriptor RVA");
        return false;
      }
      if (Catch.TypeDescriptorVA != 0 &&
          !Img.readVA(Catch.TypeDescriptorVA, 1)) {
        diagnose(F, ExceptionParseStatus::Malformed,
                 "FH4 type descriptor is not mapped");
        return false;
      }
    }
    if ((Header & 4u) != 0) {
      if (!Reader.readCompressedUInt(Value)) {
        diagnose(F, ExceptionParseStatus::Malformed,
                 "truncated FH4 catch-object displacement");
        return false;
      }
      Catch.CatchObjectOffset = static_cast<int32_t>(Value);
    }
    if (!readFH4ImageAddress(Reader, Img.Base, Catch.HandlerVA,
                             /*AllowZero=*/false) ||
        !isExecutableAddress(Img, Catch.HandlerVA)) {
      diagnose(F, ExceptionParseStatus::Malformed,
               "invalid FH4 catch handler RVA");
      return false;
    }

    Catch.ContinuationVAs.reserve(ContinuationCount);
    for (unsigned J = 0; J < ContinuationCount; ++J) {
      va_t Continuation = 0;
      if ((Header & 8u) != 0) {
        if (!readFH4ImageAddress(Reader, Img.Base, Continuation,
                                 /*AllowZero=*/false)) {
          diagnose(F, ExceptionParseStatus::Malformed,
                   "invalid FH4 image-relative continuation");
          return false;
        }
      } else {
        uint32_t Offset = 0;
        if (!Reader.readCompressedUInt(Offset) ||
            Offset > InvalidVA - F.CodeRange.Begin) {
          diagnose(F, ExceptionParseStatus::Malformed,
                   "invalid FH4 function-relative continuation");
          return false;
        }
        Continuation = F.CodeRange.Begin + Offset;
      }
      if (!isExecutableAddress(Img, Continuation)) {
        diagnose(F, ExceptionParseStatus::Malformed,
                 "FH4 continuation is not executable");
        return false;
      }
      Catch.ContinuationVAs.push_back(Continuation);
    }
    Try.Handlers.push_back(std::move(Catch));
  }
  return true;
}

bool parseFH4(ExceptionFunction &F, const BinaryImage &Img) {
  auto FuncInfoRVA = readScalar<uint32_t>(Img, F.HandlerDataVA);
  va_t FuncInfoVA = 0;
  if (!FuncInfoRVA || *FuncInfoRVA == 0 ||
      !addRVA(Img.Base, *FuncInfoRVA, FuncInfoVA)) {
    diagnose(F, ExceptionParseStatus::Malformed,
             "invalid FH4 FuncInfo reference");
    return false;
  }

  FH4Reader HeaderReader(Img, FuncInfoVA);
  uint8_t Header = 0;
  if (!HeaderReader.readByte(Header) || (Header & 0x80u) != 0) {
    diagnose(F, ExceptionParseStatus::Malformed, "invalid FH4 FuncInfo header");
    return false;
  }

  CxxExceptionInfo Info;
  LanguageRecordBudget Budget;
  Info.NativeEncoding = CxxExceptionInfo::Encoding::FH4;
  Info.Flags = Header;
  Info.IsCatchFunclet = (Header & 1u) != 0;
  Info.IsSeparated = (Header & 2u) != 0;
  Info.IsSynchronous = (Header & 0x20u) != 0;
  Info.IsNoExcept = (Header & 0x40u) != 0;

  if ((Header & 4u) != 0 && !HeaderReader.readCompressedUInt(Info.BBTFlags)) {
    diagnose(F, ExceptionParseStatus::Malformed, "truncated FH4 BBT flags");
    return false;
  }

  va_t UnwindMapVA = 0;
  va_t TryMapVA = 0;
  va_t IPMapVA = 0;
  if (((Header & 8u) != 0 &&
       !readFH4ImageAddress(HeaderReader, Img.Base, UnwindMapVA,
                            /*AllowZero=*/false)) ||
      ((Header & 0x10u) != 0 &&
       !readFH4ImageAddress(HeaderReader, Img.Base, TryMapVA,
                            /*AllowZero=*/false)) ||
      !readFH4ImageAddress(HeaderReader, Img.Base, IPMapVA,
                           /*AllowZero=*/false)) {
    diagnose(F, ExceptionParseStatus::Malformed, "invalid FH4 map reference");
    return false;
  }
  if (Info.IsCatchFunclet &&
      !HeaderReader.readCompressedUInt(Info.FrameOffset)) {
    diagnose(F, ExceptionParseStatus::Malformed,
             "truncated FH4 catch-frame displacement");
    return false;
  }
  if (Info.IsSeparated) {
    F.Cxx = std::move(Info);
    diagnose(F, ExceptionParseStatus::Partial,
             "separated FH4 code segments are retained but not normalized");
    return true;
  }

  if (UnwindMapVA != 0) {
    FH4Reader Reader(Img, UnwindMapVA);
    uint32_t Count = 0;
    if (!Reader.readCompressedUInt(Count) || Count > MaxLanguageRecords) {
      diagnose(F, ExceptionParseStatus::Malformed,
               "invalid FH4 unwind-map count");
      return false;
    }
    if (!Budget.consume(Count)) {
      diagnose(F, ExceptionParseStatus::Partial,
               "FH4 aggregate language graph exceeds decode budget");
      return false;
    }
    Info.MaxState = Count;
    Info.UnwindMap.reserve(Count);
    std::vector<va_t> EntryStarts;
    EntryStarts.reserve(Count);
    for (uint32_t I = 0; I < Count; ++I) {
      va_t EntryStart = Reader.position();
      uint32_t Encoded = 0;
      if (!Reader.readCompressedUInt(Encoded)) {
        diagnose(F, ExceptionParseStatus::Malformed,
                 "truncated FH4 unwind entry");
        return false;
      }
      uint32_t Kind = Encoded & 3u;
      uint32_t NextOffset = Encoded >> 2;
      CxxUnwindAction Action;
      if (NextOffset != 0) {
        if (NextOffset > EntryStart) {
          diagnose(F, ExceptionParseStatus::Malformed,
                   "FH4 unwind predecessor underflows");
          return false;
        }
        va_t Target = EntryStart - NextOffset;
        auto It = std::find(EntryStarts.begin(), EntryStarts.end(), Target);
        if (It == EntryStarts.end()) {
          diagnose(F, ExceptionParseStatus::Malformed,
                   "FH4 unwind predecessor is not an entry boundary");
          return false;
        }
        Action.ToState =
            static_cast<int32_t>(std::distance(EntryStarts.begin(), It));
      }

      uint32_t ObjectOffset = 0;
      switch (Kind) {
      case 0:
        Action.Kind = CxxUnwindAction::ActionKind::None;
        break;
      case 1:
        Action.Kind = CxxUnwindAction::ActionKind::DestructorWithObject;
        if (!readFH4ImageAddress(Reader, Img.Base, Action.ActionVA,
                                 /*AllowZero=*/false) ||
            !Reader.readCompressedUInt(ObjectOffset)) {
          diagnose(F, ExceptionParseStatus::Malformed,
                   "truncated FH4 object destructor action");
          return false;
        }
        Action.ObjectOffset = static_cast<int32_t>(ObjectOffset);
        break;
      case 2:
        Action.Kind = CxxUnwindAction::ActionKind::DestructorWithObjectPointer;
        if (!readFH4ImageAddress(Reader, Img.Base, Action.ActionVA,
                                 /*AllowZero=*/false) ||
            !Reader.readCompressedUInt(ObjectOffset)) {
          diagnose(F, ExceptionParseStatus::Malformed,
                   "truncated FH4 pointer destructor action");
          return false;
        }
        Action.ObjectOffset = static_cast<int32_t>(ObjectOffset);
        break;
      case 3:
        Action.Kind = CxxUnwindAction::ActionKind::Direct;
        if (!readFH4ImageAddress(Reader, Img.Base, Action.ActionVA,
                                 /*AllowZero=*/false)) {
          diagnose(F, ExceptionParseStatus::Malformed,
                   "truncated FH4 direct unwind action");
          return false;
        }
        break;
      }
      if (Action.ActionVA != 0 && !isExecutableAddress(Img, Action.ActionVA)) {
        diagnose(F, ExceptionParseStatus::Malformed,
                 "FH4 unwind action is not executable");
        return false;
      }
      EntryStarts.push_back(EntryStart);
      Info.UnwindMap.push_back(Action);
    }
  }

  if (TryMapVA != 0) {
    FH4Reader Reader(Img, TryMapVA);
    uint32_t Count = 0;
    if (!Reader.readCompressedUInt(Count) || Count > MaxLanguageRecords) {
      diagnose(F, ExceptionParseStatus::Malformed, "invalid FH4 try-map count");
      return false;
    }
    if (!Budget.consume(Count)) {
      diagnose(F, ExceptionParseStatus::Partial,
               "FH4 aggregate language graph exceeds decode budget");
      return false;
    }
    Info.TryBlocks.reserve(Count);
    for (uint32_t I = 0; I < Count; ++I) {
      uint32_t TryLow = 0;
      uint32_t TryHigh = 0;
      uint32_t CatchHigh = 0;
      va_t HandlerMapVA = 0;
      if (!Reader.readCompressedUInt(TryLow) ||
          !Reader.readCompressedUInt(TryHigh) ||
          !Reader.readCompressedUInt(CatchHigh) ||
          TryLow > std::numeric_limits<int32_t>::max() ||
          TryHigh > std::numeric_limits<int32_t>::max() ||
          CatchHigh > std::numeric_limits<int32_t>::max() ||
          !readFH4ImageAddress(Reader, Img.Base, HandlerMapVA,
                               /*AllowZero=*/false)) {
        diagnose(F, ExceptionParseStatus::Malformed,
                 "invalid FH4 try-map entry");
        return false;
      }
      CxxTryBlock Try;
      Try.TryLow = static_cast<int32_t>(TryLow);
      Try.TryHigh = static_cast<int32_t>(TryHigh);
      Try.CatchHigh = static_cast<int32_t>(CatchHigh);
      if (!parseFH4HandlerMap(F, Img, HandlerMapVA, Try, Budget))
        return false;
      Info.TryBlocks.push_back(std::move(Try));
    }
  }

  {
    FH4Reader Reader(Img, IPMapVA);
    uint32_t Count = 0;
    if (!Reader.readCompressedUInt(Count) || Count == 0 ||
        Count > MaxLanguageRecords) {
      diagnose(F, ExceptionParseStatus::Malformed,
               "invalid FH4 IP-to-state count");
      return false;
    }
    if (!Budget.consume(Count)) {
      diagnose(F, ExceptionParseStatus::Partial,
               "FH4 aggregate language graph exceeds decode budget");
      return false;
    }
    Info.IPMap.reserve(Count);
    va_t IP = F.CodeRange.Begin;
    for (uint32_t I = 0; I < Count; ++I) {
      uint32_t Delta = 0;
      uint32_t EncodedState = 0;
      if (!Reader.readCompressedUInt(Delta) ||
          !Reader.readCompressedUInt(EncodedState) || Delta > InvalidVA - IP) {
        diagnose(F, ExceptionParseStatus::Malformed,
                 "truncated FH4 IP-to-state entry");
        return false;
      }
      IP += Delta;
      if (!F.CodeRange.contains(IP) && IP != F.CodeRange.End) {
        diagnose(F, ExceptionParseStatus::Malformed,
                 "FH4 IP-to-state entry leaves its runtime function");
        return false;
      }
      if (EncodedState >
          static_cast<uint32_t>(std::numeric_limits<int32_t>::max()) + 1u) {
        diagnose(F, ExceptionParseStatus::Malformed,
                 "FH4 state number exceeds signed range");
        return false;
      }
      int32_t State =
          EncodedState == 0 ? -1 : static_cast<int32_t>(EncodedState - 1);
      Info.IPMap.push_back({IP, State});
    }
  }

  if (!Info.hasValidStateGraph()) {
    diagnose(F, ExceptionParseStatus::Malformed,
             "invalid FH4 exception state graph");
    return false;
  }
  F.Cxx = std::move(Info);
  return true;
}

bool parseFH3(ExceptionFunction &F, const BinaryImage &Img) {
  auto FuncInfoRVA = readScalar<uint32_t>(Img, F.HandlerDataVA);
  va_t FuncInfoVA = 0;
  if (!FuncInfoRVA || *FuncInfoRVA == 0 ||
      !addRVA(Img.Base, *FuncInfoRVA, FuncInfoVA)) {
    diagnose(F, ExceptionParseStatus::Malformed,
             "invalid C++ FuncInfo3 reference");
    return false;
  }
  const uint8_t *FI = nullptr;
  if (!readBytes(Img, FuncInfoVA, 40, FI)) {
    diagnose(F, ExceptionParseStatus::Malformed, "truncated C++ FuncInfo3");
    return false;
  }

  CxxExceptionInfo Info;
  std::vector<ExceptionAddressRange> FunctionGroupRanges{F.CodeRange};
  for (const ExceptionFunction &Candidate : Img.ExceptionMetadata.Functions) {
    if (&Candidate == &F || Candidate.Kind != RuntimeFunctionKind::Primary ||
        Candidate.PersonalityVA != F.PersonalityVA ||
        Candidate.HandlerDataVA == 0 || !Candidate.CodeRange.isValid())
      continue;
    auto CandidateFuncInfoRVA =
        readScalar<uint32_t>(Img, Candidate.HandlerDataVA);
    if (CandidateFuncInfoRVA && *CandidateFuncInfoRVA == *FuncInfoRVA)
      FunctionGroupRanges.push_back(Candidate.CodeRange);
  }
  std::sort(FunctionGroupRanges.begin(), FunctionGroupRanges.end(),
            [](const ExceptionAddressRange &A, const ExceptionAddressRange &B) {
              return std::tie(A.Begin, A.End) < std::tie(B.Begin, B.End);
            });
  FunctionGroupRanges.erase(
      std::unique(
          FunctionGroupRanges.begin(), FunctionGroupRanges.end(),
          [](const ExceptionAddressRange &A, const ExceptionAddressRange &B) {
            return A.Begin == B.Begin && A.End == B.End;
          }),
      FunctionGroupRanges.end());
  Info.IsSeparated = FunctionGroupRanges.size() > 1;
  auto IsFunctionGroupAddress = [&](va_t Address) {
    auto It = std::upper_bound(
        FunctionGroupRanges.begin(), FunctionGroupRanges.end(), Address,
        [](va_t Value, const ExceptionAddressRange &Range) {
          return Value < Range.Begin;
        });
    if (It == FunctionGroupRanges.begin())
      return false;
    --It;
    return It->contains(Address) || Address == It->End;
  };
  Info.Magic = readLE<uint32_t>(FI);
  int32_t MaxState = readLE<int32_t>(FI + 4);
  uint32_t UnwindMapRVA = readLE<uint32_t>(FI + 8);
  uint32_t TryCount = readLE<uint32_t>(FI + 12);
  uint32_t TryMapRVA = readLE<uint32_t>(FI + 16);
  uint32_t IPCount = readLE<uint32_t>(FI + 20);
  uint32_t IPMapRVA = readLE<uint32_t>(FI + 24);
  Info.UnwindHelpOffset = readLE<int32_t>(FI + 28);
  if (!readRVAField(Img.Base, FI + 32, Info.ESTypeListVA)) {
    diagnose(F, ExceptionParseStatus::Malformed,
             "C++ ESTypeList RVA overflows");
    return false;
  }
  Info.Flags = readLE<uint32_t>(FI + 36);
  Info.IsSynchronous = (Info.Flags & 1u) != 0;
  Info.IsNoExcept = (Info.Flags & 4u) != 0;

  if (Info.Magic != 0x19930522) {
    if (Info.Magic == 0x19930520 || Info.Magic == 0x19930521)
      diagnose(F, ExceptionParseStatus::Partial,
               "legacy C++ FuncInfo3 magic has version-specific fields");
    else {
      diagnose(F, ExceptionParseStatus::Malformed,
               "unknown C++ FuncInfo3 magic");
      return false;
    }
  }
  if (MaxState < 0 || static_cast<uint32_t>(MaxState) > MaxLanguageRecords ||
      TryCount > MaxLanguageRecords || IPCount > MaxLanguageRecords) {
    diagnose(F, ExceptionParseStatus::Malformed,
             "C++ FuncInfo3 count exceeds decode budget");
    return false;
  }
  Info.MaxState = static_cast<uint32_t>(MaxState);
  LanguageRecordBudget Budget;
  if (!Budget.consume(Info.MaxState) || !Budget.consume(TryCount) ||
      !Budget.consume(IPCount)) {
    diagnose(F, ExceptionParseStatus::Partial,
             "FH3 aggregate language graph exceeds decode budget");
    return false;
  }

  if (Info.MaxState != 0) {
    va_t MapVA = 0;
    if (UnwindMapRVA == 0 || !addRVA(Img.Base, UnwindMapRVA, MapVA)) {
      diagnose(F, ExceptionParseStatus::Malformed,
               "invalid C++ unwind-map RVA");
      return false;
    }
    const uint8_t *Map = nullptr;
    uint64_t Bytes = uint64_t(Info.MaxState) * 8;
    if (Bytes > std::numeric_limits<size_t>::max() ||
        !readBytes(Img, MapVA, static_cast<size_t>(Bytes), Map)) {
      diagnose(F, ExceptionParseStatus::Malformed, "truncated C++ unwind map");
      return false;
    }
    Info.UnwindMap.reserve(Info.MaxState);
    for (uint32_t I = 0; I < Info.MaxState; ++I) {
      const uint8_t *E = Map + uint64_t(I) * 8;
      CxxUnwindAction Action;
      Action.ToState = readLE<int32_t>(E);
      uint32_t ActionRVA = readLE<uint32_t>(E + 4);
      if (ActionRVA == 0)
        Action.Kind = CxxUnwindAction::ActionKind::None;
      if (ActionRVA != 0 && !addRVA(Img.Base, ActionRVA, Action.ActionVA)) {
        diagnose(F, ExceptionParseStatus::Malformed,
                 "C++ unwind action RVA overflows");
        return false;
      }
      if (Action.ActionVA != 0 && !isExecutableAddress(Img, Action.ActionVA)) {
        diagnose(F, ExceptionParseStatus::Malformed,
                 "C++ unwind action is not mapped executable code");
        return false;
      }
      Info.UnwindMap.push_back(Action);
    }
  }

  if (TryCount != 0) {
    va_t MapVA = 0;
    if (TryMapRVA == 0 || !addRVA(Img.Base, TryMapRVA, MapVA)) {
      diagnose(F, ExceptionParseStatus::Malformed, "invalid C++ try-map RVA");
      return false;
    }
    const uint8_t *Map = nullptr;
    uint64_t Bytes = uint64_t(TryCount) * 20;
    if (Bytes > std::numeric_limits<size_t>::max() ||
        !readBytes(Img, MapVA, static_cast<size_t>(Bytes), Map)) {
      diagnose(F, ExceptionParseStatus::Malformed, "truncated C++ try map");
      return false;
    }
    Info.TryBlocks.reserve(TryCount);
    for (uint32_t I = 0; I < TryCount; ++I) {
      const uint8_t *E = Map + uint64_t(I) * 20;
      CxxTryBlock Try;
      Try.TryLow = readLE<int32_t>(E);
      Try.TryHigh = readLE<int32_t>(E + 4);
      Try.CatchHigh = readLE<int32_t>(E + 8);
      uint32_t CatchCount = readLE<uint32_t>(E + 12);
      uint32_t HandlerMapRVA = readLE<uint32_t>(E + 16);
      if (CatchCount > MaxLanguageRecords) {
        diagnose(F, ExceptionParseStatus::Malformed,
                 "C++ catch count exceeds decode budget");
        return false;
      }
      if (!Budget.consume(CatchCount)) {
        diagnose(F, ExceptionParseStatus::Partial,
                 "FH3 aggregate language graph exceeds decode budget");
        return false;
      }
      if (CatchCount != 0) {
        va_t HandlerMapVA = 0;
        if (HandlerMapRVA == 0 ||
            !addRVA(Img.Base, HandlerMapRVA, HandlerMapVA)) {
          diagnose(F, ExceptionParseStatus::Malformed,
                   "invalid C++ handler-map RVA");
          return false;
        }
        const uint8_t *Handlers = nullptr;
        uint64_t HandlerBytes = uint64_t(CatchCount) * 20;
        if (HandlerBytes > std::numeric_limits<size_t>::max() ||
            !readBytes(Img, HandlerMapVA, static_cast<size_t>(HandlerBytes),
                       Handlers)) {
          diagnose(F, ExceptionParseStatus::Malformed,
                   "truncated C++ handler map");
          return false;
        }
        Try.Handlers.reserve(CatchCount);
        for (uint32_t J = 0; J < CatchCount; ++J) {
          const uint8_t *H = Handlers + uint64_t(J) * 20;
          CxxCatchHandler Catch;
          Catch.Adjectives = readLE<uint32_t>(H);
          if (!readRVAField(Img.Base, H + 4, Catch.TypeDescriptorVA)) {
            diagnose(F, ExceptionParseStatus::Malformed,
                     "C++ type-descriptor RVA overflows");
            return false;
          }
          if (Catch.TypeDescriptorVA != 0 &&
              !Img.readVA(Catch.TypeDescriptorVA, 1)) {
            diagnose(F, ExceptionParseStatus::Malformed,
                     "C++ type descriptor is not mapped");
            return false;
          }
          Catch.CatchObjectOffset = readLE<int32_t>(H + 8);
          if (!readRVAField(Img.Base, H + 12, Catch.HandlerVA)) {
            diagnose(F, ExceptionParseStatus::Malformed,
                     "C++ catch handler RVA overflows");
            return false;
          }
          if (Catch.HandlerVA == 0 ||
              !isExecutableAddress(Img, Catch.HandlerVA)) {
            diagnose(F, ExceptionParseStatus::Malformed,
                     "C++ catch handler is not mapped executable code");
            return false;
          }
          Catch.ParentFrameOffset = readLE<int32_t>(H + 16);
          Try.Handlers.push_back(std::move(Catch));
        }
      }
      Info.TryBlocks.push_back(std::move(Try));
    }
  }

  for (const CxxTryBlock &Try : Info.TryBlocks)
    for (const CxxCatchHandler &Catch : Try.Handlers)
      Info.IsCatchFunclet |= Catch.HandlerVA == F.CodeRange.Begin;

  if (IPCount != 0) {
    va_t MapVA = 0;
    if (IPMapRVA == 0 || !addRVA(Img.Base, IPMapRVA, MapVA)) {
      diagnose(F, ExceptionParseStatus::Malformed,
               "invalid C++ IP-to-state RVA");
      return false;
    }
    const uint8_t *Map = nullptr;
    uint64_t Bytes = uint64_t(IPCount) * 8;
    if (Bytes > std::numeric_limits<size_t>::max() ||
        !readBytes(Img, MapVA, static_cast<size_t>(Bytes), Map)) {
      diagnose(F, ExceptionParseStatus::Malformed,
               "truncated C++ IP-to-state map");
      return false;
    }
    Info.IPMap.reserve(IPCount);
    for (uint32_t I = 0; I < IPCount; ++I) {
      const uint8_t *E = Map + uint64_t(I) * 8;
      CxxIPState State;
      uint32_t IPRVA = readLE<uint32_t>(E);
      if (!addRVA(Img.Base, IPRVA, State.IP)) {
        diagnose(F, ExceptionParseStatus::Malformed,
                 "C++ IP-to-state address overflows");
        return false;
      }
      if (!IsFunctionGroupAddress(State.IP)) {
        diagnose(F, ExceptionParseStatus::Malformed,
                 "C++ IP-to-state entry 0x" + llvm::utohexstr(State.IP) +
                     " leaves its FuncInfo function group");
        return false;
      }
      State.State = readLE<int32_t>(E + 4);
      Info.IPMap.push_back(State);
    }
  }

  if (!Info.hasValidStateGraph()) {
    diagnose(F, ExceptionParseStatus::Malformed,
             "invalid C++ exception state graph");
    return false;
  }
  F.Cxx = std::move(Info);
  return true;
}

bool parseGSCookie(ExceptionFunction &F, const BinaryImage &Img,
                   va_t CookieVA) {
  GSCookieInfo Cookie;
  const uint8_t *Header = Img.readVA(CookieVA, sizeof(uint32_t));
  if (!Header) {
    Cookie.ParseStatus = ExceptionParseStatus::Malformed;
    diagnose(F, ExceptionParseStatus::Malformed, "truncated GS handler header");
    F.GSCookie = std::move(Cookie);
    return false;
  }
  uint32_t Flags = readLE<uint32_t>(Header);
  Cookie.HasExceptionHandler = (Flags & 1u) != 0;
  Cookie.HasUnwindHandler = (Flags & 2u) != 0;
  Cookie.HasAlignment = (Flags & 4u) != 0;
  Cookie.CookieOffset = static_cast<int32_t>(Flags & ~7u);
  size_t Size = sizeof(uint32_t);
  if (Cookie.HasAlignment) {
    const uint8_t *Alignment = Img.readVA(CookieVA, 3 * sizeof(uint32_t));
    if (!Alignment) {
      Cookie.ParseStatus = ExceptionParseStatus::Malformed;
      diagnose(F, ExceptionParseStatus::Malformed,
               "truncated aligned GS handler data");
      F.GSCookie = std::move(Cookie);
      return false;
    }
    Cookie.AlignmentBaseOffset = readLE<int32_t>(Alignment + 4);
    Cookie.Alignment = readLE<uint32_t>(Alignment + 8);
    if (Cookie.Alignment == 0 ||
        (Cookie.Alignment & (Cookie.Alignment - 1)) != 0) {
      Cookie.ParseStatus = ExceptionParseStatus::Malformed;
      diagnose(F, ExceptionParseStatus::Malformed,
               "invalid GS stack alignment");
      F.GSCookie = std::move(Cookie);
      return false;
    }
    Size = 3 * sizeof(uint32_t);
  }
  const uint8_t *Payload = Img.readVA(CookieVA, Size);
  if (!Payload) {
    Cookie.ParseStatus = ExceptionParseStatus::Malformed;
    diagnose(F, ExceptionParseStatus::Malformed,
             "truncated GS handler payload");
    F.GSCookie = std::move(Cookie);
    return false;
  }
  Cookie.Payload.assign(Payload, Payload + Size);
  Cookie.ParseStatus = ExceptionParseStatus::Complete;
  F.GSCookie = std::move(Cookie);
  return true;
}

std::optional<va_t> sehGSCookieAddress(const ExceptionFunction &F,
                                       const BinaryImage &Img) {
  auto Count = readScalar<uint32_t>(Img, F.HandlerDataVA);
  if (!Count || *Count > MaxLanguageRecords)
    return std::nullopt;
  uint64_t ScopeBytes = uint64_t(*Count) * 16;
  if (F.HandlerDataVA > InvalidVA - sizeof(uint32_t) ||
      ScopeBytes > InvalidVA - (F.HandlerDataVA + sizeof(uint32_t)))
    return std::nullopt;
  return F.HandlerDataVA + sizeof(uint32_t) + ScopeBytes;
}

} // namespace

ExceptionFunction decodeX64ExceptionFunction(const BinaryImage &Img,
                                             va_t ImageBase,
                                             uint32_t RuntimeFunctionRVA,
                                             uint32_t BeginRVA, uint32_t EndRVA,
                                             uint32_t UnwindInfoRVA) {
  ExceptionFunction F;
  F.RuntimeFunctionRVA = RuntimeFunctionRVA;
  F.UnwindInfoRVA = UnwindInfoRVA;
  auto Range = checkedImageRange(ImageBase, BeginRVA, EndRVA);
  if (!Range) {
    diagnose(F, ExceptionParseStatus::Malformed,
             "invalid x64 runtime-function range");
    return F;
  }
  F.CodeRange = *Range;
  const Segment *BeginSegment = Img.getSegmentFor(F.CodeRange.Begin);
  const Segment *EndSegment = Img.getSegmentFor(F.CodeRange.End - 1);
  if (!BeginSegment || BeginSegment != EndSegment ||
      !BeginSegment->isExecutable() ||
      F.CodeRange.size() > std::numeric_limits<size_t>::max() ||
      !Img.readVA(F.CodeRange.Begin, static_cast<size_t>(F.CodeRange.size())))
    diagnose(F, ExceptionParseStatus::Partial,
             "x64 runtime-function range is not wholly mapped executable "
             "code");

  if (UnwindInfoRVA == 0 || !addRVA(ImageBase, UnwindInfoRVA, F.UnwindInfoVA)) {
    diagnose(F, ExceptionParseStatus::Malformed, "invalid x64 unwind-info RVA");
    return F;
  }
  const uint8_t *Header = nullptr;
  if (!readBytes(Img, F.UnwindInfoVA, 4, Header)) {
    diagnose(F, ExceptionParseStatus::Malformed,
             "truncated x64 unwind-info header");
    return F;
  }

  F.UnwindVersion = Header[0] & 0x07;
  F.UnwindFlags = Header[0] >> 3;
  F.PrologueSize = Header[1];
  F.FrameRegister = Header[3] & 0x0f;
  F.FrameOffset = uint32_t(Header[3] >> 4) * 16;

  va_t TrailingDataVA = 0;
  if (F.UnwindVersion == 1 || F.UnwindVersion == 2) {
    F.Encoding = F.UnwindVersion == 1 ? ExceptionEncoding::X64UnwindV1
                                      : ExceptionEncoding::X64UnwindV2;
    if ((F.UnwindFlags & ~uint8_t(7)) != 0 ||
        F.PrologueSize > F.CodeRange.size()) {
      diagnose(F, ExceptionParseStatus::Malformed,
               "invalid x64 v1/v2 flags or prologue extent");
      return F;
    }
    uint8_t Count = Header[2];
    uint64_t CodeBytes = uint64_t(Count) * 2;
    if (F.UnwindInfoVA > InvalidVA - 4 ||
        CodeBytes > std::numeric_limits<size_t>::max()) {
      diagnose(F, ExceptionParseStatus::Malformed,
               "x64 unwind-code size overflows");
      return F;
    }
    const uint8_t *Codes = nullptr;
    if (!readBytes(Img, F.UnwindInfoVA + 4, static_cast<size_t>(CodeBytes),
                   Codes)) {
      diagnose(F, ExceptionParseStatus::Malformed,
               "truncated x64 unwind-code array");
      return F;
    }
    if (!decodeV1V2Operations(F, Codes, Count))
      return F;
    uint64_t TrailingOffset = alignUp(4 + CodeBytes, 4);
    if (TrailingOffset > InvalidVA - F.UnwindInfoVA) {
      diagnose(F, ExceptionParseStatus::Malformed,
               "x64 trailing-data offset overflows");
      return F;
    }
    TrailingDataVA = F.UnwindInfoVA + TrailingOffset;
  } else if (F.UnwindVersion == 3) {
    F.Encoding = ExceptionEncoding::X64UnwindV3;
    // V3 repurposes byte 3 for operation/epilogue counts; it has no v1 frame
    // register nibble in the fixed header.
    F.FrameRegister = 0;
    F.FrameOffset = 0;
    if (!decodeV3(F, Img, F.UnwindInfoVA, Header, TrailingDataVA))
      return F;
  } else {
    F.Encoding = ExceptionEncoding::Unknown;
    diagnose(F, ExceptionParseStatus::Partial,
             "unsupported x64 unwind-info version");
    return F;
  }

  parseTrailingData(F, Img, ImageBase, TrailingDataVA);
  uint64_t NativeSize = TrailingDataVA - F.UnwindInfoVA;
  if (F.UnwindFlags & UNW_ChainInfo)
    NativeSize += 12;
  else if (F.UnwindFlags & (UNW_ExceptionHandler | UNW_TerminateHandler))
    NativeSize += 4;
  if (NativeSize <= std::numeric_limits<size_t>::max()) {
    if (const uint8_t *Native =
            Img.readVA(F.UnwindInfoVA, static_cast<size_t>(NativeSize)))
      F.NativeUnwindBytes.assign(Native, Native + NativeSize);
  }
  return F;
}

void resolveExceptionHandlers(BinaryImage &Img) {
  for (ExceptionFunction &F : Img.ExceptionMetadata.Functions) {
    if (F.PersonalityVA == 0)
      continue;
    auto [ResolvedVA, Name] = resolvePersonality(Img, F.PersonalityVA);
    F.PersonalityName = Name;
    F.Personality = classifyPersonality(Name);
    if (F.Personality == ExceptionPersonality::Unknown) {
      diagnose(F, ExceptionParseStatus::Partial,
               "unknown Windows language personality");
      Img.ExceptionMetadata.ParseStatus = mergeExceptionParseStatus(
          Img.ExceptionMetadata.ParseStatus, F.ParseStatus);
      continue;
    }
    if (!isExecutableAddress(Img, ResolvedVA))
      diagnose(F, ExceptionParseStatus::Partial,
               "resolved personality is not executable");

    switch (F.Personality) {
    case ExceptionPersonality::CSpecificHandler:
      parseSEH(F, Img);
      break;
    case ExceptionPersonality::CxxFrameHandler3:
      parseFH3(F, Img);
      break;
    case ExceptionPersonality::CxxFrameHandler4:
      parseFH4(F, Img);
      break;
    case ExceptionPersonality::GSHandlerCheckSEH: {
      if (!parseSEH(F, Img))
        break;
      std::optional<va_t> CookieVA = sehGSCookieAddress(F, Img);
      if (!CookieVA) {
        diagnose(F, ExceptionParseStatus::Malformed,
                 "GS SEH payload address overflows");
        break;
      }
      parseGSCookie(F, Img, *CookieVA);
      break;
    }
    case ExceptionPersonality::GSHandlerCheckEH:
      if (parseFH3(F, Img)) {
        if (F.HandlerDataVA > InvalidVA - sizeof(uint32_t))
          diagnose(F, ExceptionParseStatus::Malformed,
                   "GS FH3 payload address overflows");
        else
          parseGSCookie(F, Img, F.HandlerDataVA + sizeof(uint32_t));
      }
      break;
    case ExceptionPersonality::GSHandlerCheckEH4:
      if (parseFH4(F, Img)) {
        if (F.HandlerDataVA > InvalidVA - sizeof(uint32_t))
          diagnose(F, ExceptionParseStatus::Malformed,
                   "GS FH4 payload address overflows");
        else
          parseGSCookie(F, Img, F.HandlerDataVA + sizeof(uint32_t));
      }
      break;
    default:
      break;
    }
    Img.ExceptionMetadata.ParseStatus = mergeExceptionParseStatus(
        Img.ExceptionMetadata.ParseStatus, F.ParseStatus);
  }
  Img.ExceptionMetadata.rebuildIndex();
}

} // namespace neverd::coff_loader
