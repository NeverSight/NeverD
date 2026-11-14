//===- NdOpEmulatorMIPS.cpp - MIPS scalar execution ----------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Exact ordinary-memory merge loads and explicit unpredictable definitions.
//===----------------------------------------------------------------------===//

#include "neverd/ArchSupport.h"
#include "neverd/Limits.h"
#include "neverd/ir/intrinsics/MIPS/MIPSDSP.h"
#include "neverd/ir/low/MIPS/MIPSCachePlatform.h"
#include "neverd/ir/low/MIPS/MIPSIntrinsicShapes.h"
#include "neverd/ir/low/MIPS/MIPSMSAExecution.h"
#include "neverd/ir/low/MIPS/MIPSScalarFPExecution.h"
#include "neverd/ir/low/NdOpEmulator.h"
#include "neverd/lift/MIPS/MIPSRegs.h"

#include "llvm/ADT/APInt.h"
#include "llvm/Support/MathExtras.h"

#include <algorithm>
#include <array>

namespace neverd {

bool NdOpEmulator::setMIPSReservationModel(uint32_t GranuleBytes) {
  return setMIPSReservationModel(GranuleBytes, false);
}

bool NdOpEmulator::setMIPSReservationModel(uint32_t GranuleBytes,
                                           bool PairedTransfers) {
  if (!isMIPS(Img.Arch) || GranuleBytes < 4 ||
      !llvm::isPowerOf2_32(GranuleBytes))
    return false;
  MIPSReservationGranule = GranuleBytes;
  MIPSPairedTransfers = PairedTransfers;
  MIPSReservationKnown = false;
  MIPSReservation.reset();
  return true;
}

void NdOpEmulator::clearMIPSReservation() {
  MIPSReservationKnown = true;
  MIPSReservation.reset();
}

bool NdOpEmulator::stepMIPSSystem(const LowOp &Op, MIPSSystemContext &Context) {
  if (!isMIPS(Img.Arch) || !mipsScalarLowShapeIsValid(Op, Img.Arch))
    return false;
  const auto Id = static_cast<Intrinsic>(Op.Inputs[0].Offset);
  const bool Wide = isMIPS64(Img.Arch);
  const bool R6 = Img.MIPS && Img.MIPS->ISARevision == 6;
  if (mips::isScalarFPControl(Id)) {
    if (!Img.MIPS || (!R6 && !Img.MIPS->hasScalarFP64ABI(Wide)) ||
        !Context.ScalarFPU ||
        !Context.ScalarFPU->supportsRevision(Img.MIPS->ISARevision) ||
        !Context.FCSR || Context.FloatingPointException)
      return false;
    const bool Write = Id == Intrinsic::MIPSWriteFPControl;
    if (Write && !knownOperandBytes(Op.Inputs[2], 0, 4))
      return false;
    auto Result = Context.ScalarFPU->control(
        Op.Inputs[1].Offset, Write,
        Write ? uint32_t(readOperand(Op.Inputs[2])) : 0, *Context.FCSR);
    if (!Result)
      return false;
    Context.FCSR = Result->Control;
    Context.FloatingPointException = !Result->Destination;
    if (!Result->Destination)
      return false;
    if (!Write)
      writeOutput(Op.Output, *Result->Destination);
    return true;
  }
  if (mips::isScalarFPArithmetic(Id)) {
    if (!Img.MIPS || (!R6 && !Img.MIPS->hasScalarFP64ABI(Wide)) ||
        !mips::scalarFPAvailable(Id, Img.MIPS->ISARevision) ||
        !Context.ScalarFPU ||
        !Context.ScalarFPU->supportsRevision(Img.MIPS->ISARevision) ||
        !Context.FCSR || Context.FloatingPointException)
      return false;
    std::array<uint64_t, 3> Inputs;
    const auto &F = mips::scalarFPOperation(Id);
    const unsigned Count = F.Arity + unsigned(F.writesCondition());
    for (unsigned I = 0; I < Count; ++I) {
      const auto &V = Op.Inputs[I + 1];
      if (!knownOperandBytes(V, 0, V.Size))
        return false;
      Inputs[I] = readOperand(V);
    }
    auto Result = Context.ScalarFPU->execute(
        Id, *Context.FCSR, llvm::ArrayRef(Inputs).take_front(Count));
    if (!Result)
      return false;
    Context.FCSR = Result->Control;
    Context.FloatingPointException = !Result->Destination;
    if (!Result->Destination)
      return false;
    if (!F.writesCondition())
      writeOutput(Op.Output, *Result->Destination);
    return true;
  }
  if (Wide && !R6 && !mips::isMSANativeVector(Id) && !mips::isMSAControl(Id))
    return false;
  if (Id == Intrinsic::MIPSExecutionHazardBarrier) {
    Context.ExecutionHazardPending = false;
    return true;
  }
  if (mips::isCacheIntrinsic(Id)) {
    const bool Prefetch = Id == Intrinsic::MIPSPrefetch;
    if (!Context.Cache || Context.ExecutionHazardPending ||
        (!Prefetch && !Context.CP0Usable) ||
        !knownOperandBytes(Op.Inputs[2], 0, Op.Inputs[2].Size) ||
        (R6 && Prefetch && Op.Inputs[1].Offset >= 24))
      return false;
    const auto VA = readOperand(Op.Inputs[2]);
    const auto T = Wide ? Context.Cache->translateDataRead64(VA)
                        : Context.Cache->translateDataRead(uint32_t(VA));
    if (!T)
      return false;
    if (T->Mapping == MIPSCacheMapping::AddressException)
      return Prefetch;
    if (Prefetch && T->Mapping == MIPSCacheMapping::Uncached)
      return true;
    const uint8_t Code = Op.Inputs[1].Offset;
    const unsigned Cache = Code & 3, Function = Code >> 2;
    auto Action = MIPSCacheAction::Prefetch;
    if (!Prefetch) {
      if (Function >= 4 && T->Mapping == MIPSCacheMapping::Uncached)
        return false;
      switch (Function) {
      case 0:
        Action = Cache == 0 ? MIPSCacheAction::IndexInvalidate
                            : MIPSCacheAction::IndexWritebackInvalidate;
        break;
      case 1:
        Action = MIPSCacheAction::IndexLoadTag;
        break;
      case 2:
        Action = MIPSCacheAction::IndexStoreTag;
        break;
      case 4:
        Action = MIPSCacheAction::HitInvalidate;
        break;
      case 5:
        Action = Cache == 0 ? MIPSCacheAction::Fill
                            : MIPSCacheAction::HitWritebackInvalidate;
        break;
      case 6:
        Action = Cache == 0 ? MIPSCacheAction::ImplementationSpecific
                            : MIPSCacheAction::HitWriteback;
        break;
      case 7:
        Action = Cache <= 1 ? MIPSCacheAction::FetchAndLock
                            : MIPSCacheAction::ImplementationSpecific;
        break;
      default:
        Action = MIPSCacheAction::ImplementationSpecific;
        break;
      }
    }
    if (!Context.Cache->apply(
            {Op.Addr, VA, *T, Code, Action, uint8_t(R6 ? 6 : 2)}))
      return false;
    // Cache eviction and coherent maintenance can invalidate an outstanding
    // reservation. The generic board interface cannot certify the live line.
    if (MIPSReservation) {
      MIPSReservation.reset();
      MIPSReservationKnown = false;
    }
    return true;
  }
  if (mips::isMSADivision(Id) && !Context.MSAFloatingPointException &&
      Img.MIPS && Img.MIPS->HasMSA && executeMIPSScalar(Op))
    return true;
  if (mips::isMSANativeVector(Id)) {
    if (!Img.MIPS || !Img.MIPS->HasMSA || Img.MIPS->ISARevision != 5 ||
        !Context.MSAFloat || !Context.MSAImplementation ||
        (*Context.MSAImplementation & (1u << 16)) || !Context.MSAControl ||
        Context.MSAFloatingPointException)
      return false;
    std::array<MIPSVector, 3> Inputs;
    const unsigned N = mips::msaNativeOperation(Id).Arity;
    for (unsigned I = 0; I < N; ++I) {
      const auto &V = Op.Inputs[I + 1];
      if (!knownOperandBytes(V, 0, 16))
        return false;
      auto Bytes = readOperandBytes(V);
      std::copy_n(Bytes.begin(), 16, Inputs[I].begin());
    }
    const auto R = Context.MSAFloat->execute(
        Id, *Context.MSAImplementation, *Context.MSAControl,
        llvm::ArrayRef(Inputs).take_front(N));
    if (!R)
      return false;
    if (mips::isMSADivision(Id)) {
      if (!R->Destination || R->Control != *Context.MSAControl)
        return false;
      writeOutputBytes(Op.Output, *R->Destination);
      return true;
    }
    Context.MSAControl = R->Control;
    Context.MSAFloatingPointException = !R->Destination;
    if (!R->Destination)
      return false;
    writeOutputBytes(Op.Output, *R->Destination);
    return true;
  }
  if (mips::isMSAControl(Id)) {
    if (!Img.MIPS || !Img.MIPS->HasMSA || Img.MIPS->ISARevision != 5 ||
        Context.MSAFloatingPointException)
      return false;
    const unsigned Selector = Op.Inputs[1].Offset;
    if (Selector >= 2 && Selector <= 7) {
      const bool Read = Id == Intrinsic::MIPSReadMSAControl;
      // MSAAccess and MSARequest ignore writes even when WRP is implemented.
      if (!Read && (Selector == 2 || Selector == 5))
        return true;
      if (!Context.MSAImplementation)
        return false;
      if (*Context.MSAImplementation & (1u << 16)) {
        if (!Context.CP0Usable || !Context.MSAPartition)
          return false;
        auto &P = *Context.MSAPartition;
        if (Read) {
          const uint32_t Views[] = {P.Access,  P.Save, P.Modify,
                                    P.Request, P.Map,  P.Unmap};
          writeOutput(Op.Output, Views[Selector - 2]);
        } else {
          if (!knownOperandBytes(Op.Inputs[2], 0, 4))
            return false;
          const uint32_t Value = readOperand(Op.Inputs[2]);
          if (Selector == 3)
            P.Save = Value;
          else if (Selector == 4)
            P.Modify = Value;
          else {
            // WRP map/unmap bits 31:5 are reserved and must be written zero.
            if (Value >= 32)
              return false;
            const unsigned Index = Value;
            const uint32_t Bit = uint32_t(1) << Index;
            if (Selector == 6) {
              P.Map = Index;
              if ((P.Access & Bit) || !P.FreePhysical)
                return true;
              --P.FreePhysical;
              P.Access |= Bit;
            } else {
              P.Unmap = Index;
              if (!(P.Access & Bit))
                return true;
              ++P.FreePhysical;
              P.Access &= ~Bit;
            }
            // A newly assigned physical WR has no proved incoming contents.
            const auto Reg = mipsreg::w(Index);
            Registers.erase(Reg);
            WideRegisters.erase(Reg);
            UnknownBits.erase(Reg);
          }
        }
        return true;
      }
    }
    if (Id == Intrinsic::MIPSReadMSAControl) {
      auto Value = Selector == 0   ? Context.MSAImplementation
                   : Selector == 1 ? Context.MSAControl
                                   : std::optional<uint32_t>(0);
      if (!Value)
        return false;
      writeOutput(Op.Output, *Value);
    } else if (Selector == 1) {
      if (!knownOperandBytes(Op.Inputs[2], 0, 4))
        return false;
      // A bound implementation supplies the writable FS/NX feature bits.
      if (!Context.MSAControlWritableMask)
        return false;
      const uint32_t Value = uint32_t(readOperand(Op.Inputs[2])) &
                             *Context.MSAControlWritableMask & 0x0167ffffu;
      Context.MSAControl = Value;
      Context.MSAFloatingPointException =
          ((Value >> 12) & ((Value >> 7 & 31) | 32) & 63) != 0;
      return !Context.MSAFloatingPointException;
    }
    return true;
  }
  if (!mips::isSystemIntrinsic(Id) || Context.ExecutionHazardPending)
    return false;
  if (Id == Intrinsic::MIPSReadHardwareRegister ||
      Id == Intrinsic::MIPSReadR6HardwareRegister) {
    const bool Selected = Id == Intrinsic::MIPSReadR6HardwareRegister;
    if (Selected && !R6)
      return false;
    const unsigned Index = Op.Inputs[1].Offset & 31,
                   Selector = Op.Inputs[1].Offset >> 5;
    if ((!Context.CP0Usable && !(Context.HWREna & (uint32_t(1) << Index))) ||
        !Context.Hardware || Context.Hardware->Index != Index ||
        Context.Hardware->Selector != Selector)
      return false;
    uint64_t Value = Context.Hardware->Value;
    if (Selected && Index == 5 && (Value > 1 || (MIPSPairedTransfers && Value)))
      return false;
    if (Wide && Index < 4)
      Value = uint64_t(int64_t(int32_t(Value)));
    writeOutput(Op.Output, Value);
    Context.Hardware.reset();
    return true;
  }
  if (const auto C = mips::cp0Transfer(Id)) {
    if (!Context.CP0Usable ||
        (!R6 && mips::cp0RequiresMT(Op.Inputs[1].Offset) &&
         !Context.ImplementedMT))
      return false;
    const uint8_t Key = Op.Inputs[1].Offset;
    const bool Write = C->Write;
    if (C->RS != 0 && C->RS != 4) {
      if (!R6 || !Context.CP0)
        return false;
      const auto View = C->Bytes == 4 ? MIPSCP0ExtendedView::HighWord
                                      : MIPSCP0ExtendedView::Doubleword;
      if (Write) {
        if (!knownOperandBytes(Op.Inputs[2], 0, C->Bytes) ||
            !Context.CP0->writeExtended(Key, View, readOperand(Op.Inputs[2]),
                                        Context))
          return false;
        Context.ExecutionHazardPending = true;
      } else {
        const auto V = Context.CP0->readExtended(Key, View, Context);
        if (!V)
          return false;
        writeOutput(Op.Output, *V);
      }
      return true;
    }
    // EPC and ErrorEPC are full-width on n64. Their word writes, including
    // any architectural sign extension, belong to the same board as DMTC0.
    if ((Wide && Key != 12) || (R6 && mips::cp0RequiresMT(Key)) ||
        !mips::localCP0Word(Key, Write)) {
      if (!Context.CP0)
        return false;
      if (Write) {
        if (!knownOperandBytes(Op.Inputs[2], 0, 4) ||
            !Context.CP0->writeWord(Key, readOperand(Op.Inputs[2]), Context))
          return false;
        Context.ExecutionHazardPending = true;
      } else {
        const auto Observed = Context.CP0->readWord(Key, Context);
        if (!Observed)
          return false;
        writeOutput(Op.Output, *Observed);
      }
      return true;
    }
    std::optional<uint32_t> *Value = nullptr;
    switch (Op.Inputs[1].Offset) {
#define NEVERD_MIPS_CP0(Index, Selector, Name, Writable)                       \
  case ((Selector << 5) | Index):                                              \
    Value = &Context.Name;                                                     \
    break;
#include "neverd/ir/intrinsics/MIPS/MIPSCP0.def"
    }
    // The shared shape contract has already admitted this exact word view.
    if (Id == Intrinsic::MIPSReadCP0Word) {
      if (!*Value)
        return false;
      writeOutput(Op.Output, **Value);
    } else {
      if (!knownOperandBytes(Op.Inputs[2], 0, 4))
        return false;
      *Value = uint32_t(readOperand(Op.Inputs[2]));
      Context.ExecutionHazardPending = true;
    }
    return true;
  }
  if (mips::isMTIntrinsic(Id)) {
    const bool Thread =
        Id == Intrinsic::MIPSDisableMT || Id == Intrinsic::MIPSEnableMT;
    auto &Control = Thread ? Context.VPEControl : Context.MVPControl;
    if (!Context.CP0Usable || !Context.ImplementedMT || !Control ||
        (!Thread && !Context.VPEConf0))
      return false;
    const uint32_t Old = *Control;
    if (Thread || (*Context.VPEConf0 & 2)) {
      const uint32_t Mask = Thread ? 1u << 15 : 1;
      const bool Enable =
          Id == Intrinsic::MIPSEnableMT || Id == Intrinsic::MIPSEnableVPE;
      Control = Enable ? Old | Mask : Old & ~Mask;
      if (*Control != Old)
        (Thread ? Context.ThreadIssueHazard : Context.ProcessorIssueHazard) =
            true;
    }
    writeOutput(Op.Output, Old);
    Context.ExecutionHazardPending = true;
    return true;
  }
  if (!Context.CP0Usable || !Context.Status)
    return false;
  const uint32_t Old = *Context.Status;
  writeOutput(Op.Output, Old);
  Context.Status =
      (Old & ~uint32_t(1)) | (Id == Intrinsic::MIPSEnableInterrupts ? 1u : 0u);
  Context.ExecutionHazardPending = true;
  return true;
}

bool NdOpEmulator::executeMIPSScalar(const LowOp &Op) {
  if (!mipsScalarLowShapeIsValid(Op, Img.Arch))
    return false;
  const auto Id = static_cast<Intrinsic>(Op.Inputs[0].Offset);
  if (mips::isSystemIntrinsic(Id) || mips::isCacheIntrinsic(Id) ||
      mips::isMSAControl(Id) || mips::isMSAFloat(Id) ||
      mips::isScalarFPArithmetic(Id) || mips::isScalarFPControl(Id))
    return false; // Explicit, single-step context required in either mode.
  if (mips::isMSADivision(Id)) {
    const auto &A = Op.Inputs[1], &B = Op.Inputs[2];
    if (!knownOperandBytes(A, 0, 16) || !knownOperandBytes(B, 0, 16))
      return false;
    const auto X = readOperandBytes(A), Y = readOperandBytes(B);
    const auto Index = unsigned(Id) - unsigned(Intrinsic::MIPSMSADIV_S_B);
    const unsigned Bytes = 1u << (Index & 3);
    MIPSVector Output{};
    const auto Lane = [&](const std::vector<uint8_t> &Data, unsigned Start) {
      uint64_t V = 0;
      for (unsigned J = 0; J != Bytes; ++J)
        V |= uint64_t(Data[Start + J]) << (J * 8);
      return llvm::APInt(Bytes * 8, V);
    };
    for (unsigned I = 0; I != 16; I += Bytes) {
      const auto Numerator = Lane(X, I), Divisor = Lane(Y, I);
      if (!Divisor)
        return false; // Requires the issuing CPU's unpredictable lane result.
      const bool Rem = Index & 8, Unsigned = Index & 4;
      const auto V =
          (Unsigned ? (Rem ? Numerator.urem(Divisor) : Numerator.udiv(Divisor))
                    : (Rem ? Numerator.srem(Divisor) : Numerator.sdiv(Divisor)))
              .getZExtValue();
      for (unsigned J = 0; J != Bytes; ++J)
        Output[I + J] = V >> (J * 8);
    }
    writeOutputBytes(Op.Output, Output);
    return true;
  }
  if (Id == Intrinsic::MIPSPause)
    return MIPSReservationGranule && MIPSReservationKnown && !MIPSReservation;
  if (const auto R = mips::reservationTransfer(Id)) {
    const auto &Base = Op.Inputs[1];
    if (!MIPSReservationGranule || (R->Paired && !MIPSPairedTransfers) ||
        !knownOperandBytes(Base, 0, Base.Size))
      return false;
    const uint64_t Address = readOperand(Base);
    const uint16_t Bytes = R->bytes();
    if (Address & (Bytes - 1))
      return false;
    // Failed SC still translates the entire naturally aligned access and checks
    // write permission. This also applies to write overlays in lenient mode.
    for (unsigned I = 0; I != Bytes; ++I) {
      const uint64_t At = Address + I;
      const auto *Mapped = Img.getSegmentFor(At);
      if (!Mapped || At < Mapped->VA || At - Mapped->VA >= Mapped->Size ||
          (R->Store ? !Mapped->isWritable() : !Mapped->isReadable()))
        return false;
    }
    if (!R->Store) {
      auto Value = loadMemoryBytes(Address, Bytes);
      if (!Value)
        return false;
      if (!archLittleEndian(Img.Arch))
        std::reverse(Value->begin(), Value->end());
      writeOutputBytes(Op.Output, *Value);
      MIPSReservation = MIPSLinkedAccess{Address, Bytes, R->Paired};
      MIPSReservationKnown = true;
      if (CollectLoads && int(LoadLog.size()) < limits::kMaxLoadRecords)
        LoadLog.push_back({Address, Bytes});
      return true;
    }
    if (!MIPSReservationKnown)
      return false;
    const bool Success = MIPSReservation && MIPSReservation->Address == Address;
    if (Success) {
      // Mixing a linked load and a different conditional store at the same
      // address is architecturally unpredictable, even when their sizes agree.
      if (MIPSReservation->Bytes != Bytes ||
          MIPSReservation->Paired != R->Paired)
        return false;
      std::array<uint8_t, 16> Data;
      for (unsigned I = 0; I < (R->Paired ? 2u : 1u); ++I) {
        const auto &Source = Op.Inputs[2 + I];
        if (!knownOperandBytes(Source, 0, R->ElementBytes))
          return false;
        const uint64_t V = readOperand(Source);
        for (unsigned J = 0; J != R->ElementBytes; ++J) {
          const unsigned Byte = I * R->ElementBytes + J;
          Data[archLittleEndian(Img.Arch) ? Byte : Bytes - 1 - Byte] =
              V >> (8 * J);
        }
      }
      if (!storeMemoryBytes(Address, llvm::ArrayRef(Data).take_front(Bytes)))
        return false;
    }
    clearMIPSReservation();
    writeOutput(Op.Output, Success);
    return true;
  }
  // Sequential ordinary-memory execution has no pending accesses or
  // execution hazards. Retain the operation in IR and native source; this
  // observation does not model devices, speculation or multiple processors.
  if (mips::isBarrier(Id))
    return true;
  if (mips::isDSPControlIntrinsic(Id)) {
    const auto State = NdVar::reg(mipsreg::DSPControl, 4);
    const auto Source =
        Id == Intrinsic::MIPSWriteDSPControl ? Op.Inputs[1] : State;
    if (!availableOperandBytes(Source, 0, 4))
      return false;
    const auto Mask = mips::dspControlMask(63, isMIPS64(Img.Arch));
    auto Value = uint32_t(readOperand(Source)) & Mask;
    auto Unknown = uint32_t(unknownOperandBits(Source)) & Mask;
    if (mips::isDSPHalfCompare(Id)) {
      if (!knownOperandBytes(Op.Inputs[1], 0, 4) ||
          !knownOperandBytes(Op.Inputs[2], 0, 4))
        return false;
      Value &= ~0x0f000000u;
      Unknown = (Unknown & ~0x0f000000u) | 0x0c000000u;
      for (unsigned I = 0; I != 2; ++I) {
        const auto A = int16_t(readOperand(Op.Inputs[1]) >> (16 * I));
        const auto B = int16_t(readOperand(Op.Inputs[2]) >> (16 * I));
        const bool Match = Id == Intrinsic::MIPSCmpEqPH   ? A == B
                           : Id == Intrinsic::MIPSCmpLtPH ? A < B
                                                          : A <= B;
        Value |= uint32_t(Match) << (24 + I);
      }
    } else if (Id == Intrinsic::MIPSMthlipPosition) {
      const auto Pos = Value & 63;
      if ((Unknown & 63) || Pos > 32)
        Unknown |= 63;
      else
        Value = (Value & ~63u) | ((Pos + 32) & 63);
    }
    writeOutput(Id == Intrinsic::MIPSReadDSPControl ? Op.Output : State, Value,
                Unknown);
    return true;
  }
  if (Id == Intrinsic::MIPSRequireTakenBranch)
    return knownOperandBytes(Op.Inputs[1], 0, 1) && readOperand(Op.Inputs[1]);
  if (Id == Intrinsic::MIPSExtractResult ||
      Id == Intrinsic::MIPSRelease6DivideResult) {
    if (!knownOperandBytes(Op.Inputs[1], 0, 1))
      return false;
    if (readOperand(Op.Inputs[1]))
      writeOutput(Op.Output, 0,
                  llvm::maskTrailingOnes<uint64_t>(Op.Output.Size * 8));
    else {
      if (!availableOperandBytes(Op.Inputs[2], 0, Op.Output.Size))
        return false;
      writeOutput(Op.Output, readOperand(Op.Inputs[2]),
                  unknownOperandBits(Op.Inputs[2]));
    }
    return true;
  }
  if (Id == Intrinsic::MIPSAccumulatorAfterWrite) {
    const auto &History = Op.Inputs[1], &Old = Op.Inputs[2];
    if (knownOperandBytes(History, 0, 1) && readOperand(History) == 0 &&
        availableOperandBytes(Old, 0, Old.Size))
      writeOutput(Op.Output, readOperand(Old), unknownOperandBits(Old));
    else
      writeOutput(Op.Output, 0,
                  llvm::maskTrailingOnes<uint64_t>(Op.Output.Size * 8));
    return true;
  }
  if (Id == Intrinsic::MIPSUnpredictable) {
    writeOutput(Op.Output, 0,
                llvm::maskTrailingOnes<uint64_t>(Op.Output.Size * 8));
    return true;
  }
  if (!knownOperandBytes(Op.Inputs[1], 0, Op.Inputs[1].Size))
    return false;
  const uint64_t First = readOperand(Op.Inputs[1]);
  if (mips::isWordDivision(Id)) {
    if (!knownOperandBytes(Op.Inputs[2], 0, 4))
      return false;
    const uint32_t Second = readOperand(Op.Inputs[2]);
    if (!Second) {
      writeOutput(Op.Output, 0, UINT64_MAX);
      return true;
    }
    uint32_t Quotient, Remainder;
    if (Id == Intrinsic::MIPSDivWord) {
      const auto SignedWord = [](uint64_t V) -> int64_t {
        return (V & UINT64_C(0x80000000)) ? int64_t(V) - (INT64_C(1) << 32)
                                          : int64_t(V);
      };
      const int64_t A = SignedWord(First), B = SignedWord(Second);
      Quotient = uint32_t(A / B);
      Remainder = uint32_t(A % B);
    } else {
      Quotient = uint32_t(First) / Second;
      Remainder = uint32_t(First) % Second;
    }
    writeOutput(Op.Output, (uint64_t(Remainder) << 32) | Quotient);
    return true;
  }

  if (Id == Intrinsic::MIPSRequireWord)
    return First == mips::signExtendWord(First);
  if (Id == Intrinsic::MIPSLwrUpper) {
    const auto Load = *mips::mergeLoad(Intrinsic::MIPSLwr);
    const auto Access =
        mips::mergeAccess(Load, First, archLittleEndian(Img.Arch));
    if (!availableOperandBytes(Op.Inputs[3], 0, 4))
      return false;
    if (Access.Count == 4) {
      const uint64_t Sign =
          mips::signExtendWord(readOperand(Op.Inputs[3])) >> 32;
      const uint64_t Unknown =
          unknownOperandBits(Op.Inputs[3]) & UINT64_C(0x80000000)
              ? UINT64_C(0xffffffff)
              : 0;
      writeOutput(Op.Output, Sign, Unknown);
      return true;
    }
    if (!availableOperandBytes(Op.Inputs[2], 0, 8))
      return false;
    const uint64_t Old = readOperand(Op.Inputs[2]);
    const uint64_t OldUnknown = unknownOperandBits(Op.Inputs[2]);
    const uint64_t High = Old >> 32;
    const uint64_t Sign = mips::signExtendWord(Old) >> 32;
    // r2 allows the old upper word or the old sign bit repeated. Only their
    // common known bits are concrete; no CPU policy is chosen here.
    const uint64_t Unknown =
        (OldUnknown >> 32) | (High ^ Sign) |
        ((OldUnknown & UINT64_C(0x80000000)) ? UINT64_C(0xffffffff) : 0);
    writeOutput(Op.Output, High, Unknown);
    return true;
  }
  if (const auto Store = mips::mergeStore(Id)) {
    const bool Little = archLittleEndian(Img.Arch);
    const auto Access = mips::mergeAccess(*Store, First, Little);
    if (!knownOperandBytes(Op.Inputs[2], Access.RegisterByte, Access.Count))
      return false;
    const uint64_t Value = readOperand(Op.Inputs[2]);
    std::array<uint8_t, 8> Bytes{};
    for (unsigned I = 0; I != Access.Count; ++I) {
      const unsigned Lane =
          Access.RegisterByte + (Little ? I : Access.Count - 1 - I);
      Bytes[I] = uint8_t(Value >> (Lane * 8));
    }
    // Validate the complete span before committing any byte. In particular a
    // three-byte partial store must not become a whole-word read/modify/write.
    return storeMemoryBytes(Access.First,
                            llvm::ArrayRef(Bytes).take_front(Access.Count));
  }
  const auto Load = *mips::mergeLoad(Id);
  const bool Little = archLittleEndian(Img.Arch);
  const auto Access = mips::mergeAccess(Load, First, Little);
  // A complete load does not observe the incoming destination register.
  if (Access.Count != Load.Bytes &&
      !availableOperandBytes(Op.Inputs[2], 0, Op.Inputs[2].Size))
    return false;
  uint64_t Result = readOperand(Op.Inputs[2]);
  uint64_t Unknown = unknownOperandBits(Op.Inputs[2]);
  for (unsigned I = 0; I != Access.Count; ++I) {
    const auto Byte = loadMemory(Access.First + I, 1);
    if (!Byte)
      return false;
    if (CollectLoads && int(LoadLog.size()) < limits::kMaxLoadRecords)
      LoadLog.push_back({Access.First + I, 1});
    const unsigned Lane =
        Access.RegisterByte + (Little ? I : Access.Count - 1 - I);
    const unsigned Shift = Lane * 8;
    Result = (Result & ~(UINT64_C(0xff) << Shift)) | (*Byte << Shift);
    Unknown &= ~(UINT64_C(0xff) << Shift);
  }
  writeOutput(Op.Output, Result, Unknown);
  return true;
}

} // namespace neverd
