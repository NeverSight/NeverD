//===- LowToMed.cpp - LowIR to MedIR conversion orchestration ------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// LowIR to MedIR (SSA) conversion: main entry point and nd-var-to-MedVar
/// mapping. Stack analysis, sub-register fixups, call return-value ABI
/// modeling, and the individual passes live in their own translation units.
///
//===----------------------------------------------------------------------===//

#include "neverd/ir/med/LowToMed.h"

#include "../X86/RegistrationRoots.h"

#include "neverd/Limits.h"
#include "neverd/ir/Limits.h"
#include "neverd/ir/TargetRegInfo.h"
#include "neverd/ir/intrinsics/Intrinsics.h"
#include "neverd/ir/intrinsics/RuntimeRoutineIntrinsics.h"
#include "neverd/ir/intrinsics/X64Syscall.h"
#include "neverd/ir/intrinsics/X87Environment.h"
#include "neverd/ir/low/BlockRegisterConstant.h"
#include "neverd/ir/low/CallRegisterEffects.h"
#include "neverd/ir/low/ImportCallee.h"
#include "neverd/ir/low/LowRawEnvironment.h"
#include "neverd/ir/low/RegistrationABI.h"
#include "neverd/ir/med/LowToMedError.h"
#include "neverd/ir/med/MedCallConvention.h"
#include "neverd/ir/med/MedConstantPropagation.h"
#include "neverd/ir/med/MedSyscallConvention.h"
#include "neverd/ir/med/X86RegistrationCall.h"
#include "neverd/ir/med/X86RegistrationEntry.h"
#include "neverd/ir/med/X86RegistrationSEHRegisters.h"
#include "neverd/libc/LibCNames.h"
#include "neverd/libc/LibCVariant.h"
#include "neverd/lift/X86Regs.h"
#include "neverd/loader/BinaryImage.h"
#include "neverd/loader/MachO/SourceRegisterCopy.h"
#include "neverd/loader/ObjC/ObjCClassGetterCalls.h"
#include "neverd/loader/SymbolDecoration.h"

#include "llvm/Support/Debug.h"
#include "llvm/Support/ErrorHandling.h"
#include "llvm/Support/raw_ostream.h"

#include <algorithm>
#include <cstdlib>
#include <exception>
#include <functional>
#include <limits>
#include <map>
#include <optional>
#include <set>
#include <tuple>

#define DEBUG_TYPE "neverd-low-to-med"

namespace neverd {

//===----------------------------------------------------------------------===//
// Runtime routines one intrinsic models
//===----------------------------------------------------------------------===//

std::map<va_t, Intrinsic> runtimeRoutineCallsOf(const BinaryImage &Img,
                                                Arch TargetArch) {
  std::map<va_t, Intrinsic> Calls;
  for (const auto &[Addr, Name] : Img.getImportAddressNames())
    if (const std::optional<Intrinsic> Id =
            runtimeRoutineIntrinsic(TargetArch, Name))
      Calls.emplace(Addr, *Id);
  // A static image defines the routine itself, at any address: an object's
  // first function starts at 0.
  for (const Symbol &Sym : Img.Symbols)
    if (Sym.IsFunc && !Sym.IsSection)
      if (const std::optional<Intrinsic> Id =
              runtimeRoutineIntrinsic(TargetArch, Sym.Name))
        Calls.emplace(Sym.Addr, *Id);
  return Calls;
}

void LowToMedConverter::liftRuntimeRoutineCalls(MedFunc &Func) {
  if (!Image)
    return;
  if (!SharedRuntimeRoutineCalls && !RuntimeRoutineCalls)
    RuntimeRoutineCalls = runtimeRoutineCallsOf(*Image, TargetArch);
  const std::map<va_t, Intrinsic> &Calls = SharedRuntimeRoutineCalls
                                               ? *SharedRuntimeRoutineCalls
                                               : *RuntimeRoutineCalls;
  if (Calls.empty())
    return;
  for (MedBlock &Block : Func.Blocks)
    for (MedOp &Op : Block.Ops) {
      if (Op.Opcode != NdOp::CALL || Op.NumInputs < 1 ||
          !Op.Inputs[0].isConst() || Op.Output.Kind != MedVar::Reg)
        continue;
      const auto It = Calls.find(Op.Inputs[0].ConstVal);
      if (It == Calls.end())
        continue;
      MedOp Read;
      Read.Opcode = NdOp::INTRINSIC;
      Read.Addr = Op.Addr;
      Read.OriginSeq = Op.OriginSeq;
      Read.Output = Op.Output;
      Read.addInput(MedVar::makeConst(static_cast<uint64_t>(It->second), 2));
      Op = std::move(Read);
    }
}

//===----------------------------------------------------------------------===//
// Stack-probe (chkstk) call neutralization
//===----------------------------------------------------------------------===//

void LowToMedConverter::neutralizeStackProbeCalls(MedFunc &Func) {
  if (!StackProbeSlots || StackProbeSlots->empty())
    return;

  for (auto &Blk : Func.Blocks) {
    for (size_t CI = 0; CI < Blk.Ops.size(); ++CI) {
      MedOp &Op = Blk.Ops[CI];
      if ((Op.Opcode != NdOp::CALL && Op.Opcode != NdOp::INDIR_CALL) ||
          Op.NumInputs < 1 || Op.Output.Size == 0)
        continue;

      // Within-block backward def of a register/temp var, searching only the
      // ops BEFORE position \p Before.  Pre-SSA a register Id is shared by
      // every write (e.g. `adrp x16` then `ldr x16,[x16,#off]` both define
      // x16), so the search must start above each use site rather than always
      // above the call — otherwise resolving the load address's base would
      // re-find the load itself.  Returns {def, its index}.  The probe's
      // GOT-load target chain lives entirely in the prologue (entry) block, so
      // the within-block scope is sufficient and safe.
      auto findDefBefore =
          [&](const MedVar &V,
              size_t Before) -> std::pair<const MedOp *, size_t> {
        if (!V.isConst())
          for (size_t J = Before; J-- > 0;) {
            const MedOp &O = Blk.Ops[J];
            if (O.Output.Size > 0 && O.Output.Kind == V.Kind &&
                O.Output.Id == V.Id)
              return {&O, J};
          }
        return {nullptr, 0};
      };

      // Resolve a var to a constant address, threading copies/width casts and a
      // folded `GOT_base + slot_offset` add (mirrors the emitter's constAddr in
      // isStackProbeCall, but with the position-aware within-block def finder).
      std::function<bool(const MedVar &, uint64_t &, int, size_t)> constAddr =
          [&](const MedVar &V, uint64_t &Out, int Depth,
              size_t Before) -> bool {
        if (Depth >
            ::neverd::limits::ir::
                kLowToMedConverterNeutralizeStackProbeCallsConstAddrDepthLimit)
          return false;
        if (V.isConst()) {
          Out = V.ConstVal;
          return true;
        }
        auto [D, DIdx] = findDefBefore(V, Before);
        if (!D || D->NumInputs < 1)
          return false;
        switch (D->Opcode) {
        case NdOp::COPY:
        case NdOp::INT_ZEXT:
        case NdOp::INT_SEXT:
          return constAddr(D->Inputs[0], Out, Depth + 1, DIdx);
        case NdOp::SUBBYTES:
          return D->NumInputs >= 2 && D->Inputs[1].isConst() &&
                 D->Inputs[1].ConstVal == 0 &&
                 constAddr(D->Inputs[0], Out, Depth + 1, DIdx);
        case NdOp::INT_ADD: {
          uint64_t A = 0, B = 0;
          if (D->NumInputs >= 2 &&
              constAddr(D->Inputs[0], A, Depth + 1, DIdx) &&
              constAddr(D->Inputs[1], B, Depth + 1, DIdx)) {
            Out = A + B;
            return true;
          }
          return false;
        }
        default:
          return false;
        }
      };

      // The call target is `LOAD <slot>` (possibly threaded through copies); a
      // direct CALL to the routine's own address has no GOT indirection.
      uint64_t SlotAddr = 0;
      bool HaveSlot = false;
      auto [D, DIdx] = findDefBefore(Op.Inputs[0], CI);
      for (
          int Guard = 0;
          D &&
          Guard <=
              ::neverd::limits::ir::
                  kLowToMedLowToMedConverterNeutralizeStackProbeCallsGuardLimit;
          ++Guard) {
        if (D->Opcode == NdOp::COPY && D->NumInputs >= 1) {
          auto Next = findDefBefore(D->Inputs[0], DIdx);
          D = Next.first;
          DIdx = Next.second;
          continue;
        }
        if (D->Opcode == NdOp::LOAD && D->NumInputs >= 1 &&
            D->MemoryAddressSpace == NdMemoryAddressSpace::Default)
          HaveSlot = constAddr(D->Inputs[0], SlotAddr, 0, DIdx);
        break;
      }
      if (!HaveSlot && Op.Inputs[0].isConst()) {
        SlotAddr = Op.Inputs[0].ConstVal;
        HaveSlot = true;
      }
      if (!HaveSlot || !StackProbeSlots->count(static_cast<va_t>(SlotAddr)))
        continue;

      // Neutralize: the probe preserves argument registers, so its modeled x0
      // definition is spurious.  Clearing the output stops buildSsa's liveness
      // from killing the live-in argument register; the emitter still elides
      // the call by its target (isStackProbeCall keys off the call target
      // operand).
      Op.Output = MedVar{};
      Op.Output.Id = -1;
      Op.Output.Size = 0;
      Op.PreservesCallerSaved = true;
    }
  }
}

namespace {
/// A function whose first instruction is also a loop header enters a block
/// that has predecessors.  SSA can place no PHI for the machine-entry edge,
/// so the loop-carried value would be lost (`for (p = arg; ...; p = p->next)`
/// kept rereading `arg`).  Give the function an empty entry block that falls
/// into the header; every other block moves up one index, so the header
/// still directly follows the entry.
void splitEntryLoopHeader(MedFunc &Func) {
  if (Func.Blocks.empty())
    return;
  bool EntryHasPred = false;
  for (const MedBlock &Block : Func.Blocks) {
    for (int Succ : Block.Succs)
      EntryHasPred |= Succ == 0;
    for (const ExceptionalEdge &Edge : Block.ExceptionalSuccs)
      EntryHasPred |= Edge.BlockId == 0;
  }
  if (!EntryHasPred)
    return;
  auto Shift = [](int &Id) {
    if (Id >= 0)
      ++Id;
  };
  for (MedBlock &Block : Func.Blocks) {
    Shift(Block.Id);
    for (int &Succ : Block.Succs)
      Shift(Succ);
    for (int &Pred : Block.Preds)
      Shift(Pred);
    for (ExceptionalEdge &Edge : Block.ExceptionalSuccs)
      Shift(Edge.BlockId);
    for (ExceptionalEdge &Edge : Block.ExceptionalPreds)
      Shift(Edge.BlockId);
  }
  Func.Blocks.front().Preds.push_back(0);
  MedBlock Entry;
  Entry.Id = 0;
  // The entry holds no instruction; an address would alias the header's
  // label and block lookups.
  Entry.StartAddr = InvalidVA;
  Entry.EndAddr = InvalidVA;
  Entry.Succs.push_back(1);
  Func.Blocks.insert(Func.Blocks.begin(), std::move(Entry));
  // A context begins at function entry, not on every visit to the machine
  // entry address. Move only its owned initializer; original instruction
  // effects stay in the loop header and run on every backedge.
  auto &Header = Func.Blocks[1].Ops;
  while (!Header.empty() && Header.front().Opcode == NdOp::INTRINSIC &&
         Header.front().NumInputs && Header.front().Inputs[0].isConst() &&
         Header.front().Inputs[0].ConstVal ==
             unsigned(Intrinsic::X87ContextBegin)) {
    Func.Blocks.front().Ops.push_back(std::move(Header.front()));
    // This is synthetic initialization, not an occurrence of the machine
    // loop header. Address-based HighIR loop recovery must not pull it back
    // into the repeated instruction region.
    Func.Blocks.front().Ops.back().Addr = InvalidVA;
    Header.erase(Header.begin());
  }
}

/// The constant the low byte of \p Register holds where the call at
/// \p CallIndex of \p Ops reads it: the last write of that byte before the
/// call, through extensions of the register itself (`xor eax, eax` writes
/// EAX and zero-extends it into RAX).  Nullopt where another kind of write,
/// an earlier call or the block's start comes first.
std::optional<uint8_t> lowByteConstantAtCall(llvm::ArrayRef<LowOp> Ops,
                                             size_t CallIndex,
                                             uint64_t Register) {
  for (size_t I = std::min(CallIndex, Ops.size()); I-- > 0;) {
    const LowOp &Op = Ops[I];
    if (Op.Opcode == NdOp::CALL || Op.Opcode == NdOp::INDIR_CALL)
      return std::nullopt;
    const NdVar &Out = Op.Output;
    // A write that leaves the byte alone, such as one to AH.
    if (!Out.isReg() || Out.Size == 0 || Out.Offset > Register ||
        Out.Offset + Out.Size <= Register)
      continue;
    if (Out.Offset != Register)
      return std::nullopt;
    if (Op.Opcode == NdOp::COPY && Op.NumInputs >= 1 && Op.Inputs[0].isConst())
      return static_cast<uint8_t>(Op.Inputs[0].Offset & 0xFF);
    if ((Op.Opcode == NdOp::INT_ZEXT || Op.Opcode == NdOp::INT_SEXT ||
         Op.Opcode == NdOp::COPY) &&
        Op.NumInputs >= 1 && Op.Inputs[0].isReg() &&
        Op.Inputs[0].Offset == Register)
      continue;
    return std::nullopt;
  }
  return std::nullopt;
}

} // namespace

bool LowToMedConverter::bindFormattedCall(MedBlock &MB, MedOp &MOp,
                                          const LowOp &LOp) {
  if (!FormattedCalls ||
      (LOp.Opcode != NdOp::CALL && LOp.Opcode != NdOp::INDIR_CALL) ||
      MOp.NumInputs < 1)
    return false;
  const auto Call = FormattedCalls->find(LOp.Addr);
  if (Call == FormattedCalls->end() ||
      Call->second.Arguments.size() >
          static_cast<size_t>(limits::kMaxBoundSourceCallArgs))
    return false;
  // Every argument the format names, in the source's order.  A floating one
  // is the low lane of its whole vector register, whose value every write
  // reaches.
  const uint16_t VectorBytes = getTargetRegInfo(TargetArch).FPABIRegWidth;
  MOp.NumInputs = 1;
  for (const FormattedCallArgument &Argument : Call->second.Arguments) {
    if (Argument.Pointer)
      MOp.ExactPointerInputs |= uint64_t(1) << (MOp.NumInputs - 1);
    if (!Argument.Floating) {
      MOp.addInput(
          ndVarToMedVar(NdVar::reg(Argument.Register, Argument.Bytes)));
      continue;
    }
    MedOp Lane;
    Lane.Opcode = NdOp::SUBBYTES;
    Lane.Addr = LOp.Addr;
    Lane.Output.Kind = MedVar::Temp;
    Lane.Output.Id = allocVarId();
    Lane.Output.Size = Argument.Bytes;
    Lane.Output.TheArch = TargetArch;
    Lane.addInput(ndVarToMedVar(NdVar::reg(Argument.Register, VectorBytes)));
    Lane.addInput(MedVar::makeConst(0, 4, ConstantAddressProvenance::Scalar));
    MOp.ExactFloatInputs |= uint64_t(1) << (MOp.NumInputs - 1);
    MOp.addInput(Lane.Output);
    MB.Ops.push_back(std::move(Lane));
  }
  MOp.ExactArguments = true;
  return true;
}

unsigned LowToMedConverter::variadicImportFixedCount(va_t Key) const {
  if (!Image || !Key)
    return 0;
  const std::string Import = importCalleeName(*Image, Key);
  if (Import.empty())
    return 0;
  // A Mach-O bind names the symbol; a PE import entry is the C name.
  const std::string Name =
      cNameOfStatedName(Import, Image->Format, Image->Arch).str();
  if (libc::libcPrototype(Name, Image->abiFormat()) ||
      !libc::isKnownFunction(Name))
    return 0;
  return libc::varArgFixedCount(Name, libc::libcVariantOf(*Image));
}

void LowToMedConverter::applyCallRegisterEffect(MedOp &MOp, const LowOp &LOp) {
  if ((LOp.Opcode != NdOp::CALL && LOp.Opcode != NdOp::INDIR_CALL) ||
      LOp.NumInputs == 0)
    return;
  // The callee's address, or the import slot a register the call goes
  // through was loaded from (RegisterCallSlot).  A call through a
  // function-pointer variable reads the arguments of the routine the
  // variable starts at (CallArgumentsFrom); nothing else follows from it.
  // A constant target is the callee even at address 0, where a relocatable
  // object's first section, and so its first function, starts.
  const bool ConstantTarget = LOp.Inputs[0].isConst();
  const va_t Key = ConstantTarget ? LOp.Inputs[0].Offset : RegisterCallSlot;
  const bool Keyed = ConstantTarget || RegisterCallSlot;
  const va_t ArgumentsKey = CallArgumentsFrom ? CallArgumentsFrom : Key;
  if (!CallArgumentsFrom && !Keyed)
    return;
  // The summaries below are read under these two keys alone.
  if (CallSummaryKeysRead) {
    CallSummaryKeysRead->insert(ArgumentsKey);
    if (Keyed)
      CallSummaryKeysRead->insert(Key);
  }
  // Publish the register arguments the callee reads as uses, so SSA sees a
  // pass-through argument and the call site knows its arity.  The calling
  // convention says whether and how (MedCallConvention.h); Mach-O
  // source-call binding owns the single-input calls of the others.  An
  // indirect call through a constant slot reaches the import the loader
  // binds there, which only its summary describes.
  const CallArgumentConvention *Convention =
      callArgumentConvention(TargetArch, TargetFormat);
  if (LOp.Opcode == NdOp::INDIR_CALL &&
      (!Convention || !Convention->ImportArgumentsFromPrototype))
    return;
  // A call through a dispatcher's own slot stays the indirect call it is;
  // the dispatcher contract below describes a call to its entry.
  if (LOp.Opcode == NdOp::INDIR_CALL && CallDispatchThunks &&
      CallDispatchThunks->count(Key))
    return;
  const TargetRegInfo &TRI = getTargetRegInfo(TargetArch);
  // A convention without register summaries still knows how many stack
  // arguments a prototyped import reads (CallEntryStackArgs).  Where only
  // internal calls take register arguments (i386), one a typed prototype
  // counts slot by slot reads exactly those slots and no register: a
  // fastcall or thiscall prototype gives no count.
  if (Convention && Convention->StackArgumentSummary &&
      !Convention->RegisterArgumentsFromCalleeSummary && CallEntryStackArgs)
    if (auto S = CallEntryStackArgs->find(ArgumentsKey);
        S != CallEntryStackArgs->end()) {
      MOp.CalleeStackArgs = static_cast<int8_t>(std::min(
          S->second, static_cast<int>(std::numeric_limits<int8_t>::max())));
      if (Convention->RegparmOnlyForInternalCalls && CallEntryStackSlots &&
          CallEntryStackSlots->count(ArgumentsKey))
        MOp.CalleeRegisterArgs = 0;
    }
  const llvm::ArrayRef<uint64_t> ArgRegs = TRI.integerParamRegs(TargetFormat);
  const auto FPParamRegs = TRI.floatingParamRegs(TargetFormat);
  const int8_t Slots =
      static_cast<int8_t>(std::min<size_t>(ArgRegs.size(), kTrackedArgSlots));
  // Where the setup of each call gives it its arguments (Windows on ARM64),
  // an import its prototype counts (CallEntryArgumentCounts) takes that many
  // integer arguments, the registers' first: its calls pass those and no
  // other register.  The setup supplies the values.  Its stack arguments
  // are counted as positions, the registers' included, or 0 for none.
  if (Convention && Convention->ArgumentsFromCallSetup &&
      Convention->ImportArgumentsFromPrototype &&
      !Convention->RegparmOnlyForInternalCalls && CallEntryArgumentCounts)
    if (auto C = CallEntryArgumentCounts->find(ArgumentsKey);
        C != CallEntryArgumentCounts->end()) {
      MOp.CalleeRegisterArgs =
          static_cast<int8_t>(std::min<int>(C->second, Slots));
      MOp.CalleeStackArgs =
          C->second > static_cast<int>(ArgRegs.size())
              ? static_cast<int8_t>(std::min(
                    C->second,
                    static_cast<int>(std::numeric_limits<int8_t>::max())))
              : 0;
    }
  // An indirect-call dispatcher calls the function in its target register.
  // Its arguments are the registers this function set before the call, the
  // rule IDA uses; a register only passed through from this function's
  // entry is not taken as an argument.
  if (Convention && Convention->DispatcherTargetRegister &&
      CallDispatchThunks && MOp.NumInputs == 1 && LOp.Inputs[0].isConst() &&
      CallDispatchThunks->count(Key)) {
    int8_t Count = 0;
    for (int8_t I = 0; I < Slots; ++I)
      if ((DispatchCallDefinedArgs >> I) & 1)
        Count = I + 1;
    MOp.Opcode = NdOp::INDIR_CALL;
    MOp.Inputs[0] = ndVarToMedVar(
        NdVar::reg(*Convention->DispatcherTargetRegister, TRI.PointerSize));
    for (int8_t I = 0; I < Count; ++I)
      MOp.addInput(ndVarToMedVar(NdVar::reg(ArgRegs[I], TRI.PointerSize)));
    MOp.CalleeRegisterArgs = Count;
  } else if (Convention && Convention->RegisterArgumentsFromCalleeSummary &&
             CallEntryReadGPRs && MOp.NumInputs == 1)
    if (auto R = CallEntryReadGPRs->find(ArgumentsKey);
        R != CallEntryReadGPRs->end() &&
        !(Convention->SummaryListsNoParameters &&
          Convention->SummaryListsNoParameters(R->second))) {
      // A positional convention passes a floating argument in its slot's
      // vector register: the whole register, whose value every write
      // reaches, and the bytes the callee reads of it.
      const bool VectorSlots = Convention->VectorArgumentsFromCalleeSummary &&
                               Convention->PositionalArgumentSlots;
      auto SlotVectorWidth = [&](int8_t I) -> uint8_t {
        return VectorSlots && static_cast<size_t>(I) < FPParamRegs.size() &&
                       static_cast<unsigned>(I) < kX64VectorArgumentFamilies &&
                       !R->second[ArgRegs[I] / 8]
                   ? R->second[kX64VectorFamilyBase + I]
                   : 0;
      };
      int8_t Count = 0;
      for (int8_t I = 0; I < Slots; ++I)
        if (R->second[ArgRegs[I] / 8] || SlotVectorWidth(I))
          Count = I + 1;
      // Pass exactly the bytes the callee reads (DL for a KIRQL), so the
      // bytes it ignores do not become an unknown incoming value.  An unread
      // slot below the last read one is still an argument position; the
      // callee cannot observe it, so it carries zero rather than whatever
      // the caller left in the register.
      for (int8_t I = 0; I < Count; ++I) {
        if (const uint8_t Vector = SlotVectorWidth(I)) {
          MOp.addInput(ndVarToMedVar(NdVar::reg(FPParamRegs[I], 16)));
          MOp.CalleeVectorSlots |= static_cast<uint8_t>(1u << I);
          MOp.CalleeVectorArgWidths |= static_cast<uint32_t>(Vector <= 4   ? 1
                                                             : Vector <= 8 ? 2
                                                                           : 4)
                                       << (4 * I);
          continue;
        }
        const uint8_t Width = R->second[ArgRegs[I] / 8];
        if (Width == 0) {
          MOp.addInput(MedVar::makeConst(0, TRI.PointerSize));
          continue;
        }
        const uint16_t Size = Width <= 1   ? 1
                              : Width <= 2 ? 2
                              : Width <= 4 ? 4
                                           : 8;
        MOp.addInput(ndVarToMedVar(NdVar::reg(ArgRegs[I], Size)));
      }
      // A variadic callee also reads the variadic arguments the caller
      // passes: the argument registers set on every path to the call.  An
      // unread fixed slot below them carries zero as above.
      std::optional<int> VariadicFrom;
      if (CallVariadicFrom && Convention->VariadicFromSummary)
        if (auto V = CallVariadicFrom->find(ArgumentsKey);
            V != CallVariadicFrom->end())
          VariadicFrom = V->second;
      // So does a variadic C library import, after the fixed parameters its
      // summary reads, where they follow them in the argument registers.
      if (!VariadicFrom && Convention->VariadicImportArgumentsInRegisters)
        if (const unsigned Fixed = variadicImportFixedCount(ArgumentsKey))
          VariadicFrom = static_cast<int>(Fixed);
      if (VariadicFrom) {
        int8_t Passed = Count;
        for (int8_t I = static_cast<int8_t>(*VariadicFrom); I < Slots; ++I)
          if ((DispatchCallDefinedArgs >> I) & 1)
            Passed = I + 1;
        for (int8_t I = Count; I < Passed; ++I)
          MOp.addInput(I < *VariadicFrom ? MedVar::makeConst(0, TRI.PointerSize)
                                         : ndVarToMedVar(NdVar::reg(
                                               ArgRegs[I], TRI.PointerSize)));
        Count = Passed;
      }
      MOp.CalleeRegisterArgs = Count;
      // The floating arguments follow, each in the next vector register the
      // callee reads: the whole register, whose value every write reaches,
      // and the bytes the callee reads of it.
      if (Convention->VectorArgumentsFromCalleeSummary && !VectorSlots) {
        int8_t Vectors = 0;
        uint32_t Widths = 0;
        for (size_t K = 0;
             K < FPParamRegs.size() && K < kX64VectorArgumentFamilies; ++K)
          if (const uint8_t Width = R->second[kX64VectorFamilyBase + K]) {
            const uint32_t Units = Width <= 4 ? 1 : Width <= 8 ? 2 : 4;
            MOp.addInput(ndVarToMedVar(NdVar::reg(FPParamRegs[K], 16)));
            Widths |= Units << (4 * Vectors);
            ++Vectors;
          }
        MOp.CalleeVectorArgs = Vectors;
        MOp.CalleeVectorArgWidths = Widths;
      }
      if (CallEntryStackArgs && Convention->StackArgumentSummary)
        if (auto S = CallEntryStackArgs->find(ArgumentsKey);
            S != CallEntryStackArgs->end())
          MOp.CalleeStackArgs = static_cast<int8_t>(std::min(
              S->second, static_cast<int>(std::numeric_limits<int8_t>::max())));
    }
  if (!CallMayWriteGPRs || !Keyed)
    return;
  auto It = CallMayWriteGPRs->find(Key);
  if (It == CallMayWriteGPRs->end())
    return;
  MOp.CallPreservedGPRs = ~It->second;
  // A callee that writes neither the integer nor the floating-point return
  // register returns nothing: the register still holds the caller's value.
  if (MOp.Output.Kind == MedVar::Reg && !(It->second & kFPReturnWriteBit))
    if (auto Family = gprFamilyOf(TargetArch, MOp.Output.RegOff);
        Family && (MOp.CallPreservedGPRs >> *Family) & 1) {
      MOp.Output = MedVar{};
      MOp.Output.Id = -1;
      MOp.Output.Size = 0;
    }
}

MedFunc LowToMedConverter::convert(const LowFunc &Low, Arch TheArch,
                                   BinaryFormat Fmt) {
  TargetArch = TheArch;
  TargetFormat = Fmt;
  for (const LowBlock &Block : Low.Blocks)
    for (const LowOp &Op : Block.Ops) {
      if (isLowRawEnvironmentRequirement(Op))
        throw LowToMedConversionError(
            "raw environment requirement has no ordinary source contract");
      if (!isKnownMemoryAddressSpace(Op.MemoryAddressSpace))
        llvm::report_fatal_error(
            "LowIR contains an unknown memory address space");
      if (Op.MemoryAddressSpace != NdMemoryAddressSpace::Default &&
          !getTargetRegInfo(TheArch).HasSegmentAddressSpaces)
        llvm::report_fatal_error(
            "FS/GS memory address spaces require an x86 target");
      if (Op.MemoryAddressSpace != NdMemoryAddressSpace::Default &&
          !opcodeSupportsMemoryAddressSpace(Op.Opcode))
        llvm::report_fatal_error(
            "memory address space is attached to a non-memory operation");
      if (Op.Opcode == NdOp::INTRINSIC) {
        const bool HasExplicitAddressSpace =
            Op.MemoryAddressSpace != NdMemoryAddressSpace::Default;
        if (Op.NumInputs == 0 || !Op.Inputs[0].isConst())
          llvm::report_fatal_error("intrinsic has no constant intrinsic ID");
        const auto Id = static_cast<Intrinsic>(Op.Inputs[0].Offset);
        if (isX87EnvironmentIntrinsic(Id) &&
            !x87EnvironmentShapeIsValid(
                Id, Op.NumInputs, Op.Output.Size,
                Op.NumInputs > 1 ? Op.Inputs[1].Size : 0,
                Op.NumInputs > 2 ? Op.Inputs[2].Size : 0,
                Op.NumInputs > 2 && Op.Inputs[2].isConst(),
                Op.NumInputs > 2 ? Op.Inputs[2].Offset : 0))
          llvm::report_fatal_error(
              "x87 environment intrinsic has an invalid image extent");
        if (isApxAtomicIntrinsic(Id)) {
          if (!intrinsicApxAtomicShapeIsValid(Id,
                                              apxAtomicLowShape(Op, TheArch)))
            llvm::report_fatal_error(
                "APX atomic intrinsic has an invalid operand/output contract");
          continue;
        }
        if (Id == Intrinsic::X86Invalidate) {
          if (!intrinsicX86InvalidateShapeIsValid(
                  Id, x86InvalidateLowShape(Op, TheArch)))
            llvm::report_fatal_error(
                "x86 invalidation intrinsic has an invalid operand/output "
                "contract");
          continue;
        }
        if (Id == Intrinsic::X86MsrAccess) {
          if (!intrinsicX86MsrAccessShapeIsValid(
                  Id, x86MsrAccessLowShape(Op, TheArch)))
            llvm::report_fatal_error(
                "x86 MSR access intrinsic has an invalid operand/output "
                "contract");
          continue;
        }
        if (Id == Intrinsic::X86RequireDivPrecondition) {
          if (!intrinsicX86DivPreconditionShapeIsValid(
                  Id, x86DivPreconditionLowShape(Op, TheArch)))
            llvm::report_fatal_error(
                "x86 divide precondition has an invalid operand/output "
                "contract");
          continue;
        }
        if (isPdepPextIntrinsic(Id)) {
          if (!intrinsicPdepPextShapeIsValid(Id, pdepPextLowShape(Op)))
            llvm::report_fatal_error(
                "PDEP/PEXT intrinsic has an invalid operand/output contract");
          continue;
        }
        if (Id == Intrinsic::X86FPClass) {
          if (HasExplicitAddressSpace)
            llvm::report_fatal_error(
                "intrinsic does not support a memory address space");
          if (Op.NumInputs != 5 || !Op.Inputs[1].isConst() ||
              !Op.Inputs[4].isConst() ||
              (Op.Inputs[1].Offset & ~UINT64_C(0x03)) != 0 ||
              !intrinsicX86FPClassShapeIsValid(
                  Op.NumInputs, Op.Output.Size,
                  static_cast<uint8_t>(Op.Inputs[1].Offset), Op.Inputs[1].Size,
                  Op.Inputs[2].Size, Op.Inputs[3].Size, Op.Inputs[4].Size))
            llvm::report_fatal_error(
                "x86 FPClass intrinsic has an invalid operand/output shape");
          continue;
        }
        if (isX86VP4DPIntrinsic(Id)) {
          if (!intrinsicX86VP4DPShapeIsValid(
                  Id, Op.NumInputs, Op.Output.Size,
                  Op.NumInputs > 1 ? Op.Inputs[1].Size : 0,
                  Op.NumInputs > 2 ? Op.Inputs[2].Size : 0,
                  Op.NumInputs > 3 ? Op.Inputs[3].Size : 0,
                  Op.NumInputs > 4 ? Op.Inputs[4].Size : 0,
                  Op.NumInputs > 5 ? Op.Inputs[5].Size : 0) ||
              !Op.Inputs[3].isConst() ||
              (!Op.Inputs[4].isReg() && !Op.Inputs[4].isConst()) ||
              !Op.Inputs[5].isConst() || Op.Inputs[3].Offset > 28 ||
              (Op.Inputs[3].Offset & 3) != 0 || Op.Inputs[5].Offset > 1)
            llvm::report_fatal_error(
                "x86 VP4DP intrinsic has an invalid operand/output shape");
          continue;
        }
        const bool IsDefaultString =
            !HasExplicitAddressSpace && isX86StringIntrinsic(Id);
        if (IsDefaultString && !intrinsicStringShapeIsValid(
                                   Id, Op.NumInputs, Op.Output.Size,
                                   Op.NumInputs > 1 ? Op.Inputs[1].Size : 0))
          llvm::report_fatal_error(
              "x86 string intrinsic has an invalid operand/output shape");
        const bool IsMemoryIntrinsic = intrinsicSupportsMemoryAddressSpace(Id);
        if (HasExplicitAddressSpace && !IsMemoryIntrinsic)
          llvm::report_fatal_error(
              "intrinsic does not support a memory address space");
        const bool IsDefaultRegisterForm =
            !HasExplicitAddressSpace &&
            intrinsicDefaultRegisterShapeIsValid(
                Id, Op.NumInputs, Op.Output.Size,
                Op.NumInputs > 1 ? Op.Inputs[1].Size : 0);
        if (IsMemoryIntrinsic && !IsDefaultString && !IsDefaultRegisterForm &&
            !intrinsicMemoryAddressSpaceShapeIsValid(
                Id, Op.NumInputs, Op.Output.Size,
                Op.NumInputs > 1 ? Op.Inputs[1].Size : 0,
                Op.NumInputs > 2 ? Op.Inputs[2].Size : 0,
                Op.NumInputs > 3 ? Op.Inputs[3].Size : 0))
          llvm::report_fatal_error(
              "memory intrinsic has an invalid operand/output shape");
      }
    }
  NextVarId = 0;
  NextTempId = 0;
  NextCallSiteId = 1;
  StackSlots.clear();
  RegVarMap.clear();
  TempVarMap.clear();

  analyzeStack(Low);

  // ARM ELF literal relocations proved at their ADD outputs
  // (LowToMedARM.cpp).
  using OccurrenceKey = std::pair<va_t, int>;
  const RelativeLiteralOutputs RelativeLiterals =
      armRelativeLiteralOutputs(Low, TheArch, Fmt);

  MedFunc Func;
  Func.Entry = Low.Entry;
  Func.Name = Low.Name;
  Func.JumpTables = Low.JumpTables;
  Func.ModuleAnalysisRoots = Low.ModuleAnalysisRoots;
  Func.CxxContinuationEntries = Low.CxxContinuationEntries;
  Func.UnsafeIndirectBranchAddresses = Low.UnsafeIndirectBranchAddresses;
  Func.ExceptionMetadata = Low.ExceptionMetadata;
  Func.RegistrationStates = Low.RegistrationStates;
  Func.CalleePopBytes = Low.CalleePopBytes;
  bool HasReturn = false, AllX87Returns = true;
  for (const LowBlock &Block : Low.Blocks)
    for (const LowOp &Op : Block.Ops)
      if (Op.Opcode == NdOp::RETURN) {
        HasReturn = true;
        AllX87Returns &=
            Op.NumInputs == 1 && Op.Inputs[0].isReg() &&
            Op.Inputs[0].Size == 10 &&
            getTargetRegInfo(TheArch).isX87StackReg(Op.Inputs[0].Offset);
      }
  Func.ExplicitX87ReturnValue = HasReturn && AllX87Returns;
  if (KnownRegistrationFrameFacts &&
      KnownRegistrationFrameFacts->Entry == Low.Entry) {
    Func.RegistrationCallerCleanupABIComplete =
        KnownRegistrationFrameFacts->CallerCleanupABIComplete;
    Func.RegistrationCxxEntryPopBytes =
        KnownRegistrationFrameFacts->CxxEntryPopBytes;
  } else {
    if (Low.RegistrationStates && Image)
      Func.RegistrationCallerCleanupABIComplete =
          hasCallerCleanupRegistrationABI(Low, *Image);
    if (Low.ExceptionMetadata && Low.ExceptionMetadata->Cxx && Image)
      Func.RegistrationCxxEntryPopBytes =
          getCheckedX86RegistrationCxxParentABI(Low, *Image);
  }
  if (SourceCallHintsEnabled && Image && Fmt == BinaryFormat::MachO &&
      TheArch == Arch::AArch64 && Image->Arch == TheArch) {
    Func.RegisterCopyProjections = sourceRegisterCopies(*Image, Low);
    Func.ClassGetterCallFacts = sourceClassGetterCalls(*Image, Low);
  }

  // Argument registers written on every path from entry, per block, for
  // dispatcher and variadic calls of conventions that pass them
  // (MedCallConvention.h).  A call clobbers them.
  std::vector<uint8_t> DispatchDefinedIn;
  const CallArgumentConvention *Convention =
      callArgumentConvention(TheArch, Fmt);
  const TargetRegInfo &ConventionRegs = getTargetRegInfo(TheArch);
  const llvm::ArrayRef<uint64_t> ArgRegs = ConventionRegs.integerParamRegs(Fmt);
  const size_t ArgSlots = std::min<size_t>(ArgRegs.size(), kTrackedArgSlots);
  const uint8_t AllArgs = static_cast<uint8_t>((1u << ArgSlots) - 1);
  const bool TrackDispatchArgs =
      Convention && ((Convention->DispatcherTargetRegister &&
                      CallDispatchThunks && !CallDispatchThunks->empty()) ||
                     (Convention->VariadicFromSummary && CallVariadicFrom &&
                      !CallVariadicFrom->empty()) ||
                     (Convention->VariadicImportArgumentsInRegisters && Image));
  auto ArgBit = [&](const NdVar &V) -> uint8_t {
    if (!V.isReg())
      return 0;
    for (size_t I = 0; I < ArgSlots; ++I)
      if (V.Offset >= ArgRegs[I] &&
          V.Offset < ArgRegs[I] + ConventionRegs.FullRegWidth)
        return uint8_t(1u << I);
    return 0;
  };
  auto StepDefined = [&](uint8_t Defined, const LowOp &Op) {
    if (Op.Opcode == NdOp::CALL || Op.Opcode == NdOp::INDIR_CALL)
      return uint8_t(0);
    return uint8_t(Defined | ArgBit(Op.Output));
  };
  if (TrackDispatchArgs) {
    std::map<int, size_t> IndexOf;
    for (size_t B = 0; B < Low.Blocks.size(); ++B)
      IndexOf[Low.Blocks[B].Id] = B;
    DispatchDefinedIn.assign(Low.Blocks.size(), AllArgs);
    if (!DispatchDefinedIn.empty())
      DispatchDefinedIn[0] = 0;
    for (bool Changed = true; Changed;) {
      Changed = false;
      for (size_t B = 0; B < Low.Blocks.size(); ++B) {
        uint8_t In = B == 0 ? 0 : AllArgs;
        bool AnyPred = false;
        for (int P : Low.Blocks[B].Preds) {
          auto It = IndexOf.find(P);
          if (It == IndexOf.end())
            continue;
          uint8_t Out = DispatchDefinedIn[It->second];
          for (const LowOp &Op : Low.Blocks[It->second].Ops)
            Out = StepDefined(Out, Op);
          In &= Out;
          AnyPred = true;
        }
        if (B != 0 && !AnyPred)
          In = 0;
        if (In != DispatchDefinedIn[B]) {
          DispatchDefinedIn[B] = In;
          Changed = true;
        }
      }
    }
  }
  size_t LowBlockIndex = 0;

  for (const auto &LB : Low.Blocks) {
    uint8_t DispatchDefined =
        TrackDispatchArgs ? DispatchDefinedIn[LowBlockIndex] : 0;
    ++LowBlockIndex;
    MedBlock MB;
    MB.Id = LB.Id;
    MB.StartAddr = LB.StartAddr;
    MB.EndAddr = LB.EndAddr;
    MB.Succs = LB.Succs;
    MB.Preds = LB.Preds;
    MB.ExceptionalSuccs = LB.ExceptionalSuccs;
    MB.ExceptionalPreds = LB.ExceptionalPreds;

    size_t BoundaryIndex = 0;
    for (size_t LowOpIndex = 0; LowOpIndex < LB.Ops.size(); ++LowOpIndex) {
      // A call through a slot that always holds one of the image's own
      // functions calls that function (BinaryImage::ConstantCodeSlots).
      std::optional<LowOp> DirectCall;
      if (const LowOp &Call = LB.Ops[LowOpIndex];
          Image && Call.Opcode == NdOp::INDIR_CALL && Call.NumInputs >= 1 &&
          Call.Inputs[0].isConst())
        if (auto It = Image->ConstantCodeSlots.find(Call.Inputs[0].Offset);
            It != Image->ConstantCodeSlots.end()) {
          DirectCall = Call;
          DirectCall->Opcode = NdOp::CALL;
          DirectCall->Inputs[0] = NdVar::cst(It->second, Call.Inputs[0].Size);
        }
      const LowOp &LOp = DirectCall ? *DirectCall : LB.Ops[LowOpIndex];
      while (BoundaryIndex < LB.InstructionBoundaries.size()) {
        const LowInstructionBoundary &Boundary =
            LB.InstructionBoundaries[BoundaryIndex];
        if (Boundary.FirstOp > LowOpIndex ||
            LowOpIndex - Boundary.FirstOp < Boundary.OpCount)
          break;
        ++BoundaryIndex;
      }
      // A stack probe touches the frame's pages and changes nothing the
      // program observes (StackProbeRoutines.inc): it lowers to nothing.
      const bool TailCall =
          BoundaryIndex < LB.InstructionBoundaries.size() &&
          LB.InstructionBoundaries[BoundaryIndex].FirstOp <= LowOpIndex &&
          LB.InstructionBoundaries[BoundaryIndex].Control ==
              LowInstructionControl::TailCall;
      if (Image && LOp.Opcode == NdOp::CALL && LOp.NumInputs > 0 &&
          LOp.Inputs[0].isConst() && !TailCall &&
          libc::stackProbeEffect(*Image, LOp.Inputs[0].Offset) ==
              libc::StackProbeEffect::Probe)
        continue;

      MedOp MOp;
      if (const auto Site = sourceCallOccurrenceKey(LOp); Site) {
        const auto Found = Func.RegisterCopyProjections.find(*Site);
        if (Found != Func.RegisterCopyProjections.end()) {
          // All normalized sources refer to leaf entry, including when a
          // sequence temporarily uses another destination as scratch.
          auto SnapshotValue = [&](const SourceRegisterValue &Source) {
            if (const auto *Address =
                    std::get_if<SourceConstantStringAddress>(&Source))
              return MedVar::makeConst(Address->Address, 8,
                                       ConstantAddressProvenance::DataAddress,
                                       Address->Address);
            MedOp Read;
            Read.Opcode = NdOp::COPY;
            Read.Addr = LOp.Addr;
            Read.Output.Kind = MedVar::Temp;
            Read.Output.Id = allocVarId();
            Read.Output.Size = 8;
            Read.Output.TheArch = TheArch;
            Read.addInput(ndVarToMedVar(
                NdVar::reg(std::get<SourceEntryRegister>(Source).Offset, 8)));
            const auto Value = Read.Output;
            MB.Ops.push_back(std::move(Read));
            return Value;
          };
          std::vector<std::pair<uint64_t, MedVar>> Snapshots;
          for (const auto &[Destination, Source] : Found->second.Registers)
            Snapshots.emplace_back(Destination, SnapshotValue(Source));
          if (const auto &Store = Found->second.StackStore) {
            const auto Value = SnapshotValue(Store->Value);
            MedOp Write;
            Write.Opcode = NdOp::STORE;
            Write.Addr = LOp.Addr;
            Write.addInput(ndVarToMedVar(
                NdVar::reg(getTargetRegInfo(TheArch).StackPointer, 8)));
            Write.addInput(Value);
            MB.Ops.push_back(std::move(Write));
          }
          for (const auto &[Destination, Value] : Snapshots) {
            MedOp Write;
            Write.Opcode = NdOp::COPY;
            Write.Addr = LOp.Addr;
            Write.Output = ndVarToMedVar(NdVar::reg(Destination, 8));
            Write.addInput(Value);
            MB.Ops.push_back(std::move(Write));
          }
          continue;
        }
      }
      MOp.Opcode = LOp.Opcode;
      MOp.MemoryOrdering = LOp.MemoryOrdering;
      MOp.MemoryAddressSpace = LOp.MemoryAddressSpace;
      MOp.Addr = LOp.Addr;
      MOp.OriginSeq = LOp.Seq;
      if (MOp.Opcode == NdOp::CALL || MOp.Opcode == NdOp::INDIR_CALL) {
        MOp.CallSiteId = NextCallSiteId++;
        if (BoundaryIndex < LB.InstructionBoundaries.size()) {
          const LowInstructionBoundary &Boundary =
              LB.InstructionBoundaries[BoundaryIndex];
          if (Boundary.FirstOp <= LowOpIndex &&
              LowOpIndex - Boundary.FirstOp < Boundary.OpCount)
            MOp.DoesNotReturn = hasLowInstructionControlFlag(
                Boundary.ControlFlags, LowInstructionControlFlag::NoReturn);
          if (TheArch == Arch::X86 && Fmt == BinaryFormat::COFF)
            MOp.DoesNotReturn |= x86_registration::hasNoReturnCallEffect(
                Low, LB, LowOpIndex, Boundary);
        }
      }

      if (LOp.Output.Size > 0)
        MOp.Output = ndVarToMedVar(LOp.Output);

      // A register/temp XOR with itself is a machine zero idiom, including
      // 128/256/512-bit vector containers. Eliminate the read before liveness
      // and SSA: folding only in HighIR leaves a false incoming FP parameter.
      const bool SelfXor = isLowSelfXor(LOp);
      // `or r, -1` and `and r, 0` do not depend on the register's old value.
      // Folding them here keeps that value from becoming a read before SSA:
      // otherwise a path where it is undefined merges `0 /* unknown */` into
      // it.
      const std::optional<uint64_t> AbsorbingConstant =
          lowAbsorbingConstant(LOp);
      if (SelfXor) {
        MOp.Opcode = NdOp::COPY;
        MOp.addInput(MedVar::makeConst(0, LOp.Output.Size,
                                       ConstantAddressProvenance::Scalar));
      } else if (AbsorbingConstant) {
        MOp.Opcode = NdOp::COPY;
        MOp.addInput(MedVar::makeConst(*AbsorbingConstant, LOp.Output.Size,
                                       ConstantAddressProvenance::Scalar));
      } else {
        for (uint8_t I = 0; I < LOp.NumInputs; ++I)
          MOp.addInput(ndVarToMedVar(LOp.Inputs[I]));
      }
      // A supervisor call under a known operating system's ABI reads its
      // number and argument registers and writes its result register.
      if (Image && LOp.Opcode == NdOp::INTRINSIC && LOp.NumInputs == 2 &&
          LOp.Inputs[0].isConst() && LOp.Inputs[1].isConst())
        if (const SyscallConvention *Syscall = syscallConvention(
                *Image, TheArch, static_cast<Intrinsic>(LOp.Inputs[0].Offset),
                LOp.Inputs[1].Offset)) {
          MedVar Id = MOp.Inputs[0];
          Id.ConstVal = static_cast<uint64_t>(Syscall->Modeled);
          const uint16_t Size = Syscall->RegisterSize;
          // A syscall the block numbers just before the call, and whose
          // arity is verified, reads only its own argument registers.
          std::optional<unsigned> Read;
          if (const auto Number = blockRegisterConstant(
                  LB.Ops, LowOpIndex, Syscall->NumberRegister, Size))
            Read = linuxSyscallArgumentCount(TheArch, *Number);
          MOp.NumInputs = 0;
          MOp.addInput(Id);
          MOp.addInput(
              ndVarToMedVar(NdVar::reg(Syscall->NumberRegister, Size)));
          for (uint8_t I = 0; I < Syscall->ArgumentCount; ++I)
            MOp.addInput(Read && I >= *Read
                             ? MedVar::makeConst(
                                   0, Size, ConstantAddressProvenance::Scalar)
                             : ndVarToMedVar(NdVar::reg(
                                   Syscall->ArgumentRegisters[I], Size)));
          MOp.Output = ndVarToMedVar(NdVar::reg(Syscall->ResultRegister, Size));
        }
      // An SMC or HVC #0 reads the registers of the SMC Calling Convention;
      // the lift binds its results.
      if (LOp.Opcode == NdOp::INTRINSIC && LOp.NumInputs == 2 &&
          LOp.Inputs[0].isConst() && LOp.Inputs[1].isConst())
        for (uint64_t Register : smcccArgumentRegisters(
                 TheArch, static_cast<Intrinsic>(LOp.Inputs[0].Offset),
                 LOp.Inputs[1].Offset))
          MOp.addInput(ndVarToMedVar(
              NdVar::reg(Register, TheArch == Arch::AArch64 ? 8 : 4)));
      DispatchCallDefinedArgs = DispatchDefined;
      // A register an indirect call goes through, loaded from an import's
      // slot, names that import as the slot would (`mov rax, [rip+slot];
      // test rax, rax; je; jmp rax` in register_tm_clones).
      RegisterCallSlot = 0;
      if (LOp.Opcode == NdOp::INDIR_CALL && LOp.NumInputs >= 1 &&
          LOp.Inputs[0].isReg() && Image)
        if (const std::optional<va_t> Slot =
                loadedCallSlot(Low, LB, LowOpIndex, LOp.Inputs[0]);
            Slot && !importCalleeName(*Image, *Slot).empty() &&
            !importOwnRoutine(*Image, CallArgumentSources, *Slot))
          RegisterCallSlot = *Slot;
      // An import's slot names the import; any other function-pointer
      // variable gives the call only the arguments of what it starts at, as
      // the slot of an import the loader starts at this image's own routine
      // does, and the stub that jumps through it.
      CallArgumentsFrom = 0;
      if (LOp.Opcode == NdOp::INDIR_CALL && LOp.NumInputs >= 1 &&
          !RegisterCallSlot && Image && CallArgumentSources &&
          !CallArgumentSources->empty() &&
          !(LOp.Inputs[0].isConst() &&
            !importCalleeName(*Image, LOp.Inputs[0].Offset).empty() &&
            !importOwnRoutine(*Image, CallArgumentSources,
                              LOp.Inputs[0].Offset)))
        if (const std::optional<va_t> Source = pointerVariableArgumentsFrom(
                CallArgumentSources, Low.Blocks, LB, LowOpIndex, LOp))
          CallArgumentsFrom = *Source;
      if (LOp.Opcode == NdOp::CALL && LOp.NumInputs >= 1 &&
          LOp.Inputs[0].isConst() && Image)
        if (const std::optional<va_t> Own = importOwnRoutine(
                *Image, CallArgumentSources, LOp.Inputs[0].Offset))
          CallArgumentsFrom = *Own;
      // How many vector registers the call tells a variadic callee it
      // passes, at most (System V AL).
      if (Convention && Convention->VariadicVectorCountRegister &&
          (LOp.Opcode == NdOp::CALL || LOp.Opcode == NdOp::INDIR_CALL))
        if (const std::optional<uint8_t> Count = lowByteConstantAtCall(
                LB.Ops, LowOpIndex, *Convention->VariadicVectorCountRegister);
            Count && *Count <= ConventionRegs.floatingParamRegs(Fmt).size())
          MOp.VariadicVectorCount = static_cast<int8_t>(*Count);
      if (!bindFormattedCall(MB, MOp, LOp))
        applyCallRegisterEffect(MOp, LOp);
      if (TheArch == Arch::X86 && Low.RegistrationStates &&
          Low.RegistrationStates->SecurityCookiesComplete &&
          Low.RegistrationStates->cookieCheck(LOp.Addr, LOp.Seq)) {
        MOp.Output = MedVar{};
        MOp.Output.Id = -1;
        MOp.CallPreservedGPRs = 0xff;
      }
      if (TrackDispatchArgs)
        DispatchDefined = StepDefined(DispatchDefined, LOp);

      if (LOp.Opcode == NdOp::INTRINSIC && LOp.NumInputs > 0 &&
          LOp.Inputs[0].isConst()) {
        const auto Id = static_cast<Intrinsic>(LOp.Inputs[0].Offset);
        const uint8_t Count = intrinsicOutputCount(Id);
        for (size_t Next = LowOpIndex + 1;
             Next < LB.Ops.size() && MOp.IntrinsicOutputs.size() < Count;
             ++Next) {
          const LowOp &Write = LB.Ops[Next];
          if (Write.Addr != LOp.Addr)
            break;
          const bool IsTransport =
              Write.Opcode == NdOp::COPY || Write.Opcode == NdOp::INT_ZEXT ||
              Write.Opcode == NdOp::INT_SEXT || Write.Opcode == NdOp::SUBBYTES;
          if (!IsTransport)
            break;
          if (Write.NumInputs == 0 || !Write.Inputs[0].isTemp())
            continue;
          MedVar Source = ndVarToMedVar(Write.Inputs[0]);
          if (std::none_of(
                  MOp.IntrinsicOutputs.begin(), MOp.IntrinsicOutputs.end(),
                  [&](const MedVar &Existing) { return Existing == Source; }))
            MOp.IntrinsicOutputs.push_back(Source);
        }
      }

      const OccurrenceKey MaterializationKey{LOp.Addr, LOp.Seq};
      auto Materialization = RelativeLiterals.Exact.find(MaterializationKey);
      if (Materialization != RelativeLiterals.Exact.end() &&
          !RelativeLiterals.Ambiguous.count(MaterializationKey) &&
          LOp.Opcode == NdOp::INT_ADD && LOp.NumInputs == 2 &&
          LOp.Output == Materialization->second->OutputWitness &&
          MOp.Opcode == NdOp::INT_ADD && MOp.Output.Size == 4 &&
          MOp.IntrinsicOutputs.empty() &&
          MOp.MemoryOrdering == NdMemoryOrdering::None &&
          MOp.MemoryAddressSpace == NdMemoryAddressSpace::Default) {
        const auto &Proof = *Materialization->second;
        MOp.Opcode = NdOp::COPY;
        MOp.NumInputs = 0;
        MOp.addInput(MedVar::makeConst(Proof.TargetVA, 4, Proof.Provenance,
                                       Proof.TargetOwnerVA));
      }
      MB.Ops.push_back(MOp);

      // i386 callee-cleanup: a checked runtime import or direct callee pops on
      // return (x86 `ret imm`, the SysV hidden struct-return (sret) pointer
      // pop) leaves the caller's stack pointer that many bytes higher than a
      // balanced call. The lifter modeled the CALL as SP-neutral, so add the
      // pop here -- before SSA, so the chain is well-formed -- and later stack
      // accesses (the cdecl `add esp, k` cleanup, the result-buffer reload) use
      // the corrected SP.
      if ((MOp.Opcode == NdOp::CALL || MOp.Opcode == NdOp::INDIR_CALL) &&
          MOp.NumInputs >= 1 && MOp.Inputs[0].isConst()) {
        auto Pop = TheArch == Arch::X86 ? registrationCallStackPop(Low, LB, LOp)
                                        : std::nullopt;
        // A call through an import's slot (`call [__imp__Sleep@4]`) names
        // the import as its stub does.
        if (!Pop && CalleePopMap) {
          const auto It = CalleePopMap->find(MOp.Inputs[0].ConstVal);
          if (It != CalleePopMap->end())
            Pop = It->second;
        }
        if (Pop && *Pop) {
          const auto &TRI = getTargetRegInfo(TheArch);
          MedVar Sp;
          Sp.Kind = MedVar::Reg;
          Sp.RegOff = TRI.StackPointer;
          Sp.Size = static_cast<uint16_t>(TRI.PointerSize);
          Sp.TheArch = TheArch;
          // Thread the SAME SSA variable the lifter uses for the stack pointer
          // (ndVarToMedVar keys registers by (RegOff,Size) in RegVarMap).  A
          // hand-built MedVar would otherwise keep the default id 0 — a foreign
          // variable — so buildSsa would split SP into two SSA names and a
          // loop- carried SP would take this call's intermediate +imm value
          // instead of the later balanced (post-`sub esp`) value, drifting each
          // iteration.
          auto SpKey = std::make_pair(static_cast<uint64_t>(TRI.StackPointer),
                                      static_cast<uint16_t>(TRI.PointerSize));
          auto SpIt = RegVarMap.find(SpKey);
          Sp.Id = (SpIt != RegVarMap.end()) ? SpIt->second
                                            : (RegVarMap[SpKey] = allocVarId());
          MedOp Adj;
          Adj.Opcode = NdOp::INT_ADD;
          Adj.Addr = LOp.Addr;
          Adj.Output = Sp;
          Adj.addInput(Sp);
          Adj.addInput(
              MedVar::makeConst(static_cast<uint64_t>(*Pop),
                                static_cast<uint16_t>(TRI.PointerSize)));
          MB.Ops.push_back(Adj);
        }
      }
    }

    Func.Blocks.push_back(std::move(MB));
  }

  // Before sub-register fixup: model calls to known i64-returning callees
  // (32-bit register-pair return EDX:EAX / R1:R0) as defining the pair. Running
  // ahead of fixupSubRegisters lets its implicit zero-extension widen BOTH the
  // low (EAX) and the synthesized high (EDX) halves into their 64-bit
  // containers symmetrically — otherwise only the low half (whose wide alias
  // survives from the call's original return-register output) reaches the
  // container and a post-call read of the high half resolves to the stale
  // pre-call value. buildSsa then creates the loop-carried high-half PHI for a
  // threaded i64 accumulator.  No-op unless the pipeline set the i64-callee set
  // (only known after whole-program return-type inference).
  size_t CopiedOps = 0;
  for (const auto &Block : Func.Blocks)
    CopiedOps += Block.Ops.size();
  if (Low.DecodedInstructionCount >
          static_cast<uint64_t>(limits::kMaxSSAFunctionOps) ||
      CopiedOps > limits::kMaxSSAFunctionOps) {
    LLVM_DEBUG(llvm::dbgs() << "LowIR -> MedIR: skipping SSA for " << Func.Name
                            << " insns=" << Low.DecodedInstructionCount
                            << " ops=" << CopiedOps << "\n");
    if (TheArch == Arch::X64 && Low.ExceptionMetadata &&
        Low.ExceptionMetadata->SEH)
      throw LowToMedConversionError(
          "Windows SEH establisher frame: SSA size limit prevents proof");
    // The unoptimized ops still carry their LowIR occurrences, so switch
    // selectors bind exactly as they would after the full pipeline.
    resolveSwitchSelectorPlans(Func);
    Func.SkippedSSA = true;
    return Func;
  }

  try {
    x86_registration::RegistrationRoots(Low, TheArch, Fmt)
        .disconnectNoReturnFallthroughs(Func);
    bindSourceCalls(Func, Low, Fmt);
    modelKnownWideCallReturns(Func);
    debugVerifyMedFunc(Func, "modelKnownWideCallReturns");

    modelSEHFinallyRegisters(Func);
    debugVerifyMedFunc(Func, "modelSEHFinallyRegisters");

    fixupSubRegisters(Func);
    debugVerifyMedFunc(Func, "fixupSubRegisters");

    simplifyCfg(Func);
    debugVerifyMedFunc(Func, "simplifyCfg");

    // ARM predication is flattened in LowIR as an instruction-local guard plus
    // its same-address effects.  Materialize that micro-CFG before SSA so the
    // skip path keeps the incoming architectural registers while the effect
    // path receives the new definitions.  Doing this in the LLVM emitter would
    // be too late: SSA would already have treated every effect as
    // unconditional.
    materializePredicatedEffects(Func);
    debugVerifyMedFunc(Func, "materializePredicatedEffects");

    // Apple clang's prologue stack-probe (____chkstk_darwin) is modeled as an
    // ordinary call returning in x0; clear that spurious output before SSA so
    // its liveness does not kill the live-in argument registers (the probe
    // preserves every register except x16/x17).  No-op unless the pipeline
    // provided the chkstk slot set (Mach-O only).
    neutralizeStackProbeCalls(Func);
    debugVerifyMedFunc(Func, "neutralizeStackProbeCalls");
    liftRuntimeRoutineCalls(Func);
    debugVerifyMedFunc(Func, "liftRuntimeRoutineCalls");

    splitEntryLoopHeader(Func);
    debugVerifyMedFunc(Func, "splitEntryLoopHeader");
    buildSsa(Func, Low);
    debugVerifyMedFunc(Func, "buildSsa");

    // The x86 lifter's PUSHF and the ARM lifter's MRS of APSR.
    foldMachineFlagsIntoStatusImages(Func);
    debugVerifyMedFunc(Func, "foldMachineFlagsIntoStatusImages");

    // Model a call's floating-point/vector return (x86-64 returns it in XMM0, a
    // caller-saved vector register the lifter did not model the call as
    // defining). Done before copy propagation so a post-call read of the result
    // register is not folded back to the pre-call argument value. Model a
    // direct call's small struct-by-value return across multiple registers
    // (x86-64 eightbytes / AArch64 HFA) before modelCallFPReturn so it claims
    // the FP return register of a struct-returning call as one of the aggregate
    // fields rather than the lone scalar FP result.
    modelCallStructReturn(Func);
    debugVerifyMedFunc(Func, "modelCallStructReturn");

    modelCallFPReturn(Func);
    debugVerifyMedFunc(Func, "modelCallFPReturn");

    // Model a call's x87 floating-point return on i386 (the cdecl convention
    // leaves it on the x87 top-of-stack, st0): reconnect the post-call `fstp`
    // read of st0 to the call's result, which the lifter did not model.
    modelCallX87Return(Func);
    debugVerifyMedFunc(Func, "modelCallX87Return");

    // Model a call's 64-bit integer return on 32-bit targets (i386 EDX:EAX,
    // ARM32 R1:R0): the lifter did not model the call as defining the high-half
    // register, so reconnect post-call reads of it to the call's high result.
    modelCallWideIntReturn(Func, TargetArch);
    debugVerifyMedFunc(Func, "modelCallWideIntReturn");

    // Post-SSA pass: fix sub-register reads that should reference a loop PHI.
    // When a sub-register (e.g. SIL) has SSAVer=0 (entry block definition)
    // but the current block has a PHI for a wider register (RSI), insert a
    // SUBBYTES and update the read to use the extracted value.
    {
      int MaxSSAVer = 0;
      for (auto &MB : Func.Blocks)
        for (auto &Op : MB.Ops)
          if (Op.Output.SSAVer > MaxSSAVer)
            MaxSSAVer = Op.Output.SSAVer;
      int NextVer = MaxSSAVer + 100;

      for (auto &MB : Func.Blocks) {
        if (MB.Phis.empty())
          continue;
        // Only apply to loop headers (blocks that have themselves as a
        // predecessor).
        bool IsLoopHeader = false;
        for (int P : MB.Preds)
          if (P == MB.Id)
            IsLoopHeader = true;
        if (!IsLoopHeader)
          continue;
        std::map<uint64_t, const PhiNode *> PhiByRegOff;
        for (const auto &Phi : MB.Phis) {
          if (Phi.Output.Kind == MedVar::Reg && Phi.Output.Size > 0) {
            PhiByRegOff[Phi.Output.RegOff] = &Phi;
          }
        }
        if (PhiByRegOff.empty())
          continue;

        struct PostSSASub {
          size_t InsertBefore;
          MedOp Op;
          size_t OpIdx;
          uint8_t InpIdx;
          int NewVer;
        };
        std::vector<PostSSASub> Fixes;
        std::map<std::pair<int, uint64_t>, int> AlreadyFixed;

        for (size_t OI = 0; OI < MB.Ops.size(); ++OI) {
          auto &MOp = MB.Ops[OI];
          for (uint8_t I = 0; I < MOp.NumInputs; ++I) {
            auto &Inp = MOp.Inputs[I];
            if (Inp.Kind != MedVar::Reg || Inp.Size == 0 || Inp.SSAVer != 0)
              continue;
            // Direct RegOff match: if a PHI at the same RegOff has a wider
            // size, the current register is a sub-register of the PHI.
            {
              auto PhiIt = PhiByRegOff.find(Inp.RegOff);
              if (PhiIt == PhiByRegOff.end() ||
                  PhiIt->second->Output.Size <= Inp.Size)
                continue;
              auto FixKey = std::make_pair(Inp.Id, Inp.RegOff);
              auto FIt = AlreadyFixed.find(FixKey);
              int NewVarVer;
              if (FIt != AlreadyFixed.end()) {
                NewVarVer = FIt->second;
              } else {
                NewVarVer = NextVer++;
                AlreadyFixed[FixKey] = NewVarVer;
                MedOp Sub;
                Sub.Opcode = NdOp::SUBBYTES;
                Sub.Addr = MOp.Addr;
                Sub.Output = Inp;
                Sub.Output.SSAVer = NewVarVer;
                const auto &PhiOut = PhiIt->second->Output;
                MedVar Wide;
                Wide.Kind = MedVar::Reg;
                Wide.Id = PhiOut.Id;
                Wide.Size = PhiOut.Size;
                Wide.RegOff = PhiOut.RegOff;
                Wide.SSAVer = PhiOut.SSAVer;
                Wide.TheArch = TargetArch;
                Sub.addInput(Wide);
                Sub.addInput(MedVar::makeConst(0, 4));
                Fixes.push_back({OI, std::move(Sub), OI, I, NewVarVer});
              }
              Inp.SSAVer = NewVarVer;
            }
          }
        }
        for (auto It = Fixes.rbegin(); It != Fixes.rend(); ++It)
          MB.Ops.insert(MB.Ops.begin() + static_cast<long>(It->InsertBefore),
                        std::move(It->Op));
      }
    }

    // Complementary direction: the block above fixes a narrow read that should
    // come from a wider loop PHI; this fixes a WIDE read that must merge a
    // narrower loop-carried sub-register PHI (e.g. byte-popcount `movl %edi`
    // over `shrb %dil`).  Runs post-SSA so the narrow phi is visible.
    mergeLoopCarriedPartialReads(Func);
    debugVerifyMedFunc(Func, "mergeLoopCarriedPartialReads");

    // ARM/AArch64 analogue: a wide vector (Q) read at a loop header that
    // resolves to the loop-invariant preamble value because only its 64-bit
    // halves (D sub-registers) are loop-carried via phis.  Reconstruct from the
    // half phis.
    mergeLoopCarriedVectorReads(Func);
    debugVerifyMedFunc(Func, "mergeLoopCarriedVectorReads");

    x86_registration::discardUnusedSEHCallInputs(Func);
    detectCc(Func, TheArch, Fmt);
    completeX86RegistrationEntryParameters(Func);
    debugVerifyMedFunc(Func, "detectCc");

    propagate(Func);
    debugVerifyMedFunc(Func, "propagate");

    eliminateFlags(Func);
    debugVerifyMedFunc(Func, "eliminateFlags");

    // Where ordinary arithmetic writes the flags, flag lowering leaves PF/AF/OF
    // writes that no remaining COND_BR reads.  Without DCE those become LLVMC
    // `__builtin_popcount` / flag SSA noise on `test`/`cmp` that only consume
    // ZF.
    if (getTargetRegInfo(TheArch).ArithmeticWritesFlags) {
      runDce(Func);
      debugVerifyMedFunc(Func, "runDce");
    }

    // Bind public LowIR selector occurrences only after every MedIR rewrite and
    // SSA/propagation pass has finished.  A source op that disappeared, was
    // duplicated, or no longer has the certified operand role deliberately
    // yields no plan; backends must fail closed rather than fall back to a
    // physical register-number scan.
    if (Image && foldImmutableTableScans(Func, *Image)) {
      propagateInvariantConstants(Func);
      runDce(Func);
      debugVerifyMedFunc(Func, "foldImmutableTableScans");
    }
    resolveSwitchSelectorPlans(Func);
    resolveScalarAddressModels(Func,
                               Low.RelocatedInstructionScalarModelOccurrences);
    resolveI386GetPcModels(Func, Low.I386GetPcOccurrences);
    resolveCxxContinuationExits(
        Func, Low,
        static_cast<uint16_t>(getTargetRegInfo(TheArch).PointerSize));

    LLVM_DEBUG(llvm::dbgs() << "LowIR -> MedIR: " << Func.Blocks.size()
                            << " blocks, " << Func.Params.size() << " params, "
                            << Func.Locals.size() << " locals\n");
  } catch (const LowToMedConversionError &) {
    throw;
  } catch (const std::exception &) {
    LLVM_DEBUG(llvm::dbgs() << "LowIR -> MedIR: SSA/rewrite threw; keeping "
                               "copied blocks for "
                            << Func.Name << "\n");
  } catch (...) {
    LLVM_DEBUG(llvm::dbgs() << "LowIR -> MedIR: SSA/rewrite threw; keeping "
                               "copied blocks for "
                            << Func.Name << "\n");
  }
  return Func;
}

void LowToMedConverter::resolveCxxContinuationExits(MedFunc &Func,
                                                    const LowFunc &Low,
                                                    uint16_t PointerSize) {
  Func.CxxContinuationExits.clear();
  Func.CxxContinuationExitAnalysisComplete = false;
  if (!Low.CxxContinuationExitAnalysisComplete || PointerSize == 0)
    return;

  using OccurrenceKey = std::pair<va_t, int>;
  std::set<OccurrenceKey> SeenOccurrences;
  std::vector<MedCxxContinuationExitEvidence> BoundExits;
  BoundExits.reserve(Low.CxxContinuationExits.size());

  for (const LowCxxContinuationExitEvidence &Evidence :
       Low.CxxContinuationExits) {
    const OccurrenceKey Key{Evidence.ReturnAddr, Evidence.ReturnSeq};
    if (Evidence.ReturnAddr == InvalidVA || Evidence.ReturnSeq < 0 ||
        !SeenOccurrences.insert(Key).second ||
        !std::is_sorted(Evidence.Targets.begin(), Evidence.Targets.end()) ||
        std::adjacent_find(Evidence.Targets.begin(), Evidence.Targets.end()) !=
            Evidence.Targets.end() ||
        std::find(Evidence.Targets.begin(), Evidence.Targets.end(),
                  InvalidVA) != Evidence.Targets.end() ||
        (!Evidence.Complete && !Evidence.Targets.empty()))
      return;

    const MedBlock *BoundBlock = nullptr;
    const MedOp *BoundReturn = nullptr;
    for (const MedBlock &Block : Func.Blocks) {
      for (const MedOp &Op : Block.Ops) {
        if (Op.Opcode != NdOp::RETURN || Op.Addr != Evidence.ReturnAddr ||
            Op.OriginSeq != Evidence.ReturnSeq)
          continue;
        if (BoundReturn)
          return;
        BoundBlock = &Block;
        BoundReturn = &Op;
      }
    }
    if (!BoundBlock || !BoundReturn || BoundReturn->NumInputs != 1 ||
        BoundReturn->Inputs[0].Size != PointerSize)
      return;

    MedCxxContinuationExitEvidence Bound;
    Bound.ReturnAddr = Evidence.ReturnAddr;
    Bound.ReturnSeq = Evidence.ReturnSeq;
    Bound.BlockId = BoundBlock->Id;
    Bound.ReturnValue = BoundReturn->Inputs[0];
    Bound.Targets = Evidence.Targets;
    Bound.Complete = Evidence.Complete;
    BoundExits.push_back(std::move(Bound));
  }

  Func.CxxContinuationExits = std::move(BoundExits);
  Func.CxxContinuationExitAnalysisComplete = true;
}

void LowToMedConverter::resolveScalarAddressModels(
    MedFunc &Func,
    const std::vector<RelocatedInstructionScalarModelOccurrence> &Models) {
  Func.ScalarAddressModels.clear();
  for (const RelocatedInstructionScalarModelOccurrence &Model : Models) {
    if (Model.InstructionAddr == InvalidVA || Model.OpSeq < 0 ||
        Model.Width == 0 || Model.OutputWitness.Size != Model.Width ||
        (!Model.OutputWitness.isReg() && !Model.OutputWitness.isTemp()))
      continue;

    const MedVar Expected = ndVarToMedVar(Model.OutputWitness);
    std::optional<MedVar> Bound;
    bool Ambiguous = false;
    for (const MedBlock &Block : Func.Blocks) {
      for (const MedOp &Op : Block.Ops) {
        if (Op.Addr != Model.InstructionAddr || Op.OriginSeq != Model.OpSeq ||
            Op.Opcode != Model.OutputOpcode || Op.Output.Size != Model.Width)
          continue;
        const MedVar &Candidate = Op.Output;
        const bool SameLane =
            !Candidate.isConst() && Candidate.Kind == Expected.Kind &&
            Candidate.Id == Expected.Id && Candidate.Size == Expected.Size &&
            (Candidate.Kind != MedVar::Reg ||
             Candidate.RegOff == Expected.RegOff);
        if (!SameLane)
          continue;
        if (Bound) {
          Ambiguous = true;
          break;
        }
        Bound = Candidate;
      }
      if (Ambiguous)
        break;
    }
    if (Ambiguous || !Bound)
      continue;
    Func.ScalarAddressModels.push_back({Model.Model, *Bound});
  }
}

void LowToMedConverter::resolveI386GetPcModels(
    MedFunc &Func, const std::vector<I386GetPcOccurrence> &Occurrences) {
  Func.I386GetPcModels.clear();
  using Key = std::tuple<int, int, int, uint16_t, int>;
  auto keyFor = [](const MedVar &Value) {
    return Key{static_cast<int>(Value.Kind), Value.Id, Value.SSAVer, Value.Size,
               Value.RegOff};
  };
  std::map<Key, MedI386GetPcModel> BoundModels;
  std::set<Key> Ambiguous;

  for (const I386GetPcOccurrence &Occurrence : Occurrences) {
    if (!Occurrence.RawPCAuthenticated ||
        Occurrence.InstructionAddr == InvalidVA || Occurrence.OpSeq < 0 ||
        Occurrence.OutputOpcode != NdOp::COPY ||
        Occurrence.OutputWitness.Size != 4 ||
        (!Occurrence.OutputWitness.isReg() &&
         !Occurrence.OutputWitness.isTemp()) ||
        !Occurrence.InputWitness.isTemp() || Occurrence.InputWitness.Size != 4)
      continue;

    const MedVar Expected = ndVarToMedVar(Occurrence.OutputWitness);
    const MedVar ExpectedInput = ndVarToMedVar(Occurrence.InputWitness);
    std::optional<MedI386GetPcModel> Bound;
    bool Multiple = false;
    bool SawSurvivingCopy = false;
    for (const MedBlock &Block : Func.Blocks) {
      for (const MedOp &Op : Block.Ops) {
        if (Op.Addr != Occurrence.InstructionAddr ||
            Op.OriginSeq != Occurrence.OpSeq ||
            Op.Opcode != Occurrence.OutputOpcode || Op.Output.Size != 4)
          continue;
        const MedVar &Candidate = Op.Output;
        const bool SameLane =
            !Candidate.isConst() && Candidate.Kind == Expected.Kind &&
            Candidate.Id == Expected.Id && Candidate.Size == Expected.Size &&
            (Candidate.Kind != MedVar::Reg ||
             Candidate.RegOff == Expected.RegOff);
        if (!SameLane)
          continue;
        SawSurvivingCopy = true;
        if (Op.NumInputs != 1)
          continue;
        const MedVar &CandidateInput = Op.Inputs[0];
        const bool SameInputLane =
            !CandidateInput.isConst() &&
            CandidateInput.Kind == ExpectedInput.Kind &&
            CandidateInput.Id == ExpectedInput.Id &&
            CandidateInput.Size == ExpectedInput.Size &&
            (CandidateInput.Kind != MedVar::Reg ||
             CandidateInput.RegOff == ExpectedInput.RegOff);
        if (!SameInputLane)
          continue;
        if (Bound) {
          Multiple = true;
          break;
        }
        Bound =
            MedI386GetPcModel{Candidate, CandidateInput, Occurrence.PCValue};
      }
      if (Multiple)
        break;
    }
    // Propagation can redirect every user of the POP's architectural COPY to
    // its input and DCE can then remove that COPY.  The CFG proof still names
    // the exact POP LOAD temporary.  Bind that surviving producer only when
    // there is no conflicting rewritten COPY, and require one matching LOAD at
    // the same instruction before the original COPY sequence.  Its raw stack
    // load remains in MedIR; only an address expression derived from this
    // authenticated SSA value may fold to the call-next PC.
    if (!Multiple && !Bound && !SawSurvivingCopy) {
      const MedVar *PopLoad = nullptr;
      for (const MedBlock &Block : Func.Blocks) {
        for (const MedOp &Op : Block.Ops) {
          if (Op.Addr != Occurrence.InstructionAddr ||
              Op.OriginSeq >= Occurrence.OpSeq || Op.Opcode != NdOp::LOAD ||
              Op.NumInputs != 1 ||
              Op.MemoryAddressSpace != NdMemoryAddressSpace::Default ||
              Op.Output.Kind != ExpectedInput.Kind ||
              Op.Output.Id != ExpectedInput.Id ||
              Op.Output.Size != ExpectedInput.Size)
            continue;
          if (PopLoad) {
            Multiple = true;
            break;
          }
          PopLoad = &Op.Output;
        }
        if (Multiple)
          break;
      }
      if (PopLoad)
        Bound = MedI386GetPcModel{MedVar{}, *PopLoad, Occurrence.PCValue};
    }
    if (Multiple || !Bound)
      continue;

    const Key BoundKey =
        keyFor(Bound->Output.Size != 0 ? Bound->Output : Bound->Value);
    if (Ambiguous.count(BoundKey))
      continue;
    auto [It, Inserted] = BoundModels.emplace(BoundKey, *Bound);
    if (!Inserted && It->second.PCValue != Occurrence.PCValue) {
      BoundModels.erase(It);
      Ambiguous.insert(BoundKey);
    }
  }

  for (const auto &[BoundKey, Model] : BoundModels) {
    (void)BoundKey;
    Func.I386GetPcModels.push_back(Model);
  }
}

void LowToMedConverter::resolveSwitchSelectorPlans(MedFunc &Func) {
  Func.SwitchSelectorPlans.clear();
  struct BoundSelector {
    int BlockId = -1;
    MedVar Value = {};
  };
  std::map<int, const MedBlock *> Blocks;
  for (const MedBlock &Block : Func.Blocks)
    if (!Blocks.emplace(Block.Id, &Block).second)
      return;
  auto bindUseRef = [&](const JumpTableSelectorUseRef &Ref, int DispatchBlock,
                        const std::set<int> *AllowedBlocks =
                            nullptr) -> std::optional<BoundSelector> {
    std::map<int, std::vector<MedVar>> Bindings;
    size_t Count = 0;
    for (const MedBlock &Block : Func.Blocks) {
      if (AllowedBlocks && !AllowedBlocks->count(Block.Id))
        continue;
      for (const MedOp &Op : Block.Ops) {
        if (Op.Addr != Ref.Addr || Op.OriginSeq != Ref.Seq ||
            Op.Opcode != Ref.ExpectedOpcode)
          continue;
        MedVar Candidate;
        if (Ref.Role == JumpTableSelectorUseRef::ValueRole::Input) {
          if (Ref.InputNo >= Op.NumInputs ||
              Op.Inputs[Ref.InputNo].Size != Ref.ExpectedSize)
            continue;
          Candidate = Op.Inputs[Ref.InputNo];
        } else {
          if (Op.Output.Size != Ref.ExpectedSize)
            continue;
          Candidate = Op.Output;
        }
        ++Count;
        Bindings[Block.Id].push_back(Candidate);
      }
    }
    // A CFG product (for example x87 TOP states) copies the instruction but
    // gives each dispatcher its own SSA lifetime. Every incoming path must
    // encounter the same occurrence before an independent entry. A surviving
    // use in a sibling clone cannot repair this clone's missing/changed use.
    std::optional<BoundSelector> Bound;
    if (AllowedBlocks) {
      // An edge-merged plan binds each explicitly admitted predecessor once;
      // the caller checks complete, one-to-one predecessor coverage below.
      if (Count == 1)
        Bound = BoundSelector{Bindings.begin()->first,
                              Bindings.begin()->second.front()};
    } else {
      std::set<int> Seen;
      std::vector<int> Work{DispatchBlock};
      size_t Remaining = limits::kMaxSSAFunctionOps;
      while (!Work.empty()) {
        if (Remaining-- == 0)
          return std::nullopt;
        const int Id = Work.back();
        Work.pop_back();
        if (!Seen.insert(Id).second)
          continue;
        const auto BlockIt = Blocks.find(Id);
        if (BlockIt == Blocks.end())
          return std::nullopt;
        if (const auto Found = Bindings.find(Id); Found != Bindings.end()) {
          if (Found->second.size() != 1 || (Bound && Bound->BlockId != Id))
            return std::nullopt;
          Bound = BoundSelector{Id, Found->second.front()};
          continue;
        }
        const MedBlock &Block = *BlockIt->second;
        if (Block.Preds.empty() || Block.Id == Func.Blocks.front().Id ||
            Func.ModuleAnalysisRoots.count(Block.StartAddr))
          return std::nullopt;
        if (Block.Preds.size() > Remaining)
          return std::nullopt;
        Remaining -= Block.Preds.size();
        Work.insert(Work.end(), Block.Preds.begin(), Block.Preds.end());
      }
    }
    if (!Bound || Bound->Value.Size == 0 || Bound->Value.isConst())
      return std::nullopt;
    return Bound;
  };

  for (const JumpTable &JT : Func.JumpTables) {
    for (const MedBlock &Dispatch : Func.Blocks) {
      const size_t Branches = std::count_if(
          Dispatch.Ops.begin(), Dispatch.Ops.end(), [&](const MedOp &Op) {
            return Op.Addr == JT.InsnAddr && Op.Opcode == NdOp::INDIR_BR;
          });
      if (Branches != 1)
        continue;
      const auto Key = std::make_pair(JT.InsnAddr, Dispatch.Id);
      if (JT.CompositeSelectorUseRef) {
        const JumpTableCompositeSelectorUseRef &Composite =
            *JT.CompositeSelectorUseRef;
        if (!JT.TwoTableSelect || !JT.SelectorUseRefs.empty() ||
            Composite.RecipeKind !=
                JumpTableCompositeSelectorUseRef::Kind::SelectOffset)
          continue;
        auto ByteIndex = bindUseRef(Composite.ByteIndex, Dispatch.Id);
        auto Condition = bindUseRef(Composite.Condition, Dispatch.Id);
        if (!ByteIndex || !Condition ||
            ByteIndex->Value.Size != Composite.ResultSize)
          continue;

        MedSwitchSelectorPlan Plan;
        Plan.PlanKind = MedSwitchSelectorPlan::Kind::SelectOffset;
        Plan.Selector = ByteIndex->Value;
        Plan.Condition = Condition->Value;
        Plan.TrueOffset = Composite.TrueOffset;
        Plan.FalseOffset = Composite.FalseOffset;
        Plan.ResultSize = Composite.ResultSize;
        Func.SwitchSelectorPlans.emplace(Key, std::move(Plan));
        continue;
      }
      if (JT.SelectorUseRefs.empty() || JT.TwoTableSelect)
        continue;
      if (JT.SelectorUseRefs.size() > 1) {
        if (Dispatch.Preds.size() < 2 ||
            Dispatch.Preds.size() != JT.SelectorUseRefs.size())
          continue;
        const std::set<int> Preds(Dispatch.Preds.begin(), Dispatch.Preds.end());
        MedSwitchSelectorPlan Plan;
        Plan.PlanKind = MedSwitchSelectorPlan::Kind::EdgeMerged;
        std::set<int> SeenPreds;
        bool Valid = true;
        for (const JumpTableSelectorUseRef &Ref : JT.SelectorUseRefs) {
          auto Selector = bindUseRef(Ref, Dispatch.Id, &Preds);
          if (!Selector || Selector->Value.Size == 0 ||
              (Plan.ResultSize != 0 &&
               Selector->Value.Size != Plan.ResultSize) ||
              !SeenPreds.insert(Selector->BlockId).second) {
            Valid = false;
            break;
          }
          Plan.ResultSize = Selector->Value.Size;
          Plan.EdgeSelectors.emplace_back(Selector->BlockId, Selector->Value);
        }
        if (!Valid || SeenPreds != Preds)
          continue;
        std::sort(
            Plan.EdgeSelectors.begin(), Plan.EdgeSelectors.end(),
            [](const auto &A, const auto &B) { return A.first < B.first; });
        Func.SwitchSelectorPlans.emplace(Key, std::move(Plan));
        continue;
      }
      auto Selector = bindUseRef(JT.SelectorUseRefs.front(), Dispatch.Id);
      if (!Selector)
        continue;

      MedSwitchSelectorPlan Plan;
      Plan.PlanKind = MedSwitchSelectorPlan::Kind::Direct;
      Plan.Selector = Selector->Value;
      Plan.ResultSize = Selector->Value.Size;
      Func.SwitchSelectorPlans.emplace(Key, std::move(Plan));
    }
  }
}

//===----------------------------------------------------------------------===//
// NdVar -> MedVar conversion
//===----------------------------------------------------------------------===//

MedVar LowToMedConverter::ndVarToMedVar(const NdVar &VN) {
  MedVar MV;
  MV.Size = VN.Size;

  switch (VN.Space) {
  case VnodeSpace::REG: {
    auto Key = std::make_pair(VN.Offset, VN.Size);
    auto It = RegVarMap.find(Key);
    if (It != RegVarMap.end()) {
      MV.Kind = MedVar::Reg;
      MV.Id = It->second;
    } else {
      MV.Kind = MedVar::Reg;
      MV.Id = allocVarId();
      RegVarMap[Key] = MV.Id;
    }
    MV.RegOff = VN.Offset;
    MV.TheArch = TargetArch;

    {
      const auto &TRI = getTargetRegInfo(TargetArch);
      if (TRI.isFlag(VN.Offset, VN.Size))
        MV.Kind = MedVar::Flag;
    }
    break;
  }
  case VnodeSpace::TEMP: {
    auto Key = std::make_pair(VN.Offset, VN.Size);
    auto It = TempVarMap.find(Key);
    if (It != TempVarMap.end()) {
      MV.Kind = MedVar::Temp;
      MV.Id = It->second;
    } else {
      MV.Kind = MedVar::Temp;
      MV.Id = allocVarId();
      TempVarMap[Key] = MV.Id;
    }
    break;
  }
  case VnodeSpace::CONST: {
    MV =
        MedVar::makeConst(VN.Offset, VN.Size, VN.Provenance, VN.AddressOwnerVA);
    break;
  }
  case VnodeSpace::STACK: {
    MV.Kind = MedVar::Stack;
    for (const auto &Slot : StackSlots) {
      if (Slot.Offset == static_cast<int64_t>(VN.Offset) &&
          Slot.Size == VN.Size) {
        MV.Id = Slot.VarId;
        break;
      }
    }
    MV.StackOff = static_cast<int64_t>(VN.Offset);
    break;
  }
  default:
    MV.Kind = MedVar::Temp;
    MV.Id = allocVarId();
    break;
  }

  return MV;
}

} // namespace neverd
