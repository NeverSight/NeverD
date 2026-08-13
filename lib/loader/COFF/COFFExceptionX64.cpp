//===- COFFExceptionX64.cpp - AMD64 unwind decoding -----------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "COFFExceptionDetail.h"

#include "neverd/loader/COFF/COFFException.h"
#include "neverd/support/BinaryEncoding.h"

#include "llvm/Support/Win64EH.h"

#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <utility>
#include <vector>

namespace neverd::coff_loader {
namespace {

using namespace llvm::Win64EH;

using detail::addRVA;
using detail::checkedImageRange;
using detail::diagnose;
using detail::isExecutableAddress;
using detail::readBytes;
using detail::readScalar;

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
         CodeOffset > *PreviousCodeOffset)) {
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
    if (CodeBytes != 0 && !readBytes(Img, F.UnwindInfoVA + 4,
                                     static_cast<size_t>(CodeBytes), Codes)) {
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

} // namespace neverd::coff_loader
