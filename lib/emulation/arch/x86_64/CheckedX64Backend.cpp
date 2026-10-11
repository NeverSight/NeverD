//===- CheckedX64Backend.cpp - Checked x64 execution---------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "CheckedX64Backend.h"

#include "../../core/ExecutionDiagnostics.h"
#include "../../core/RAMTransaction.h"
#include "X64Decoder.h"
#include "X64Exception.h"
#include "X64ExceptionMonitor.h"

#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/ScopeExit.h"
#include "llvm/Support/Endian.h"
#include "llvm/Support/ErrorHandling.h"
#include "llvm/Support/Format.h"
#include "llvm/Support/MathExtras.h"
#include "llvm/Support/raw_ostream.h"

#include <algorithm>
#include <chrono>
#include <climits>
#include <cstdio>
#include <exception>
#include <utility>

namespace neverd::emulation {
using diagnostic::error;
namespace {
std::optional<bool> stackDirection(unsigned Instruction) {
  switch (Instruction) {
#define NEVERD_X64_STACK_INSTRUCTION(Name, Push)                               \
  case X86_INS_##Name:                                                         \
    return Push;
#include "X64StackInstructions.def"
#undef NEVERD_X64_STACK_INSTRUCTION
  default:
    return std::nullopt;
  }
}
enum class BitAccess { Read, Update };
std::optional<BitAccess> bitAccess(unsigned Instruction) {
  switch (Instruction) {
#define NEVERD_X64_BIT_INSTRUCTION(Name, Access)                               \
  case X86_INS_##Name:                                                         \
    return BitAccess::Access;
#include "X64BitInstructions.def"
#undef NEVERD_X64_BIT_INSTRUCTION
  default:
    return std::nullopt;
  }
}
bool admitsBitOperands(const cs_x86 &X) {
  if (X.op_count != 2)
    return false;
  const auto &Base = X.operands[0], &Index = X.operands[1];
  return (Base.type == X86_OP_REG || Base.type == X86_OP_MEM) &&
         (Base.size == x64::HalfWordBytes || Base.size == x64::DWordBytes ||
          Base.size == x64::WordBytes) &&
         (Index.type == X86_OP_IMM ||
          (Index.type == X86_OP_REG && Index.size == Base.size));
}
bool isDoubleShift(unsigned Instruction) {
  switch (Instruction) {
#define NEVERD_X64_DOUBLE_SHIFT_INSTRUCTION(Name)                              \
  case X86_INS_##Name:                                                         \
    return true;
#include "X64DoubleShiftInstructions.def"
#undef NEVERD_X64_DOUBLE_SHIFT_INSTRUCTION
  default:
    return false;
  }
}
bool admitsDoubleShiftOperands(const cs_x86 &X) {
  if (X.op_count != 3)
    return false;
  const auto &Destination = X.operands[0], &Source = X.operands[1],
             &Count = X.operands[2];
  return (Destination.type == X86_OP_REG || Destination.type == X86_OP_MEM) &&
         (Destination.size == x64::HalfWordBytes ||
          Destination.size == x64::DWordBytes ||
          Destination.size == x64::WordBytes) &&
         Source.type == X86_OP_REG && Source.size == Destination.size &&
         Count.size == x64::ByteBytes &&
         (Count.type == X86_OP_IMM ||
          (Count.type == X86_OP_REG && Count.reg == X86_REG_CL));
}
bool isScalarShift(unsigned Instruction) {
  switch (Instruction) {
#define NEVERD_X64_SCALAR_SHIFT_INSTRUCTION(Name)                              \
  case X86_INS_##Name:                                                         \
    return true;
#include "X64ScalarShiftInstructions.def"
#undef NEVERD_X64_SCALAR_SHIFT_INSTRUCTION
  default:
    return false;
  }
}
bool admitsScalarShiftOperands(const cs_x86 &X) {
  if (X.op_count != 2)
    return false;
  const auto &Destination = X.operands[0], &Count = X.operands[1];
  return (Destination.type == X86_OP_REG || Destination.type == X86_OP_MEM) &&
         (Destination.size == x64::ByteBytes ||
          Destination.size == x64::HalfWordBytes ||
          Destination.size == x64::DWordBytes ||
          Destination.size == x64::WordBytes) &&
         ((Count.type == X86_OP_IMM && Count.imm >= 0 &&
           uint64_t(Count.imm) <= UINT8_MAX) ||
          (Count.type == X86_OP_REG && Count.reg == X86_REG_CL));
}
bool isLoop(unsigned Instruction) {
  switch (Instruction) {
#define NEVERD_X64_LOOP_INSTRUCTION(Name)                                      \
  case X86_INS_##Name:                                                         \
    return true;
#include "X64LoopInstructions.def"
#undef NEVERD_X64_LOOP_INSTRUCTION
  default:
    return false;
  }
}
bool admitsLoopOperands(const cs_insn &I) {
  const auto &X = I.detail->x86;
  if (X.op_count != 1 || X.operands[0].type != X86_OP_IMM ||
      X.encoding.imm_size != x64::ByteBytes || I.size < x64::ShortBranchBytes)
    return false;
  // Capstone omits ignored REP prefixes from LOOP's normalized metadata.
  // Keep checked admission explicit, without mistaking a displacement byte
  // for a prefix or changing how the processor handles 66H, 67H and REX.
  for (uint8_t Prefix : llvm::ArrayRef(I.bytes, I.size - x64::ShortBranchBytes))
    if (Prefix == X86_PREFIX_LOCK || Prefix == X86_PREFIX_REP ||
        Prefix == X86_PREFIX_REPNE)
      return false;
  return true;
}
bool isPrefixedReturn(const cs_insn &I) {
  // The two-byte near return recommended for branch predictors: the processor
  // ignores the repeat prefix, so this is exactly the admitted C3 transfer.
  return I.id == X86_INS_RET && I.size == x64::ShortBranchBytes &&
         I.bytes[0] == X86_PREFIX_REP && I.bytes[1] == x64::NearReturnOpcode;
}
std::optional<bool> condition(unsigned Instruction, uint64_t Flags) {
  const bool Carry = Flags & x64::CarryFlag;
  const bool Parity = Flags & x64::ParityFlag;
  const bool Zero = Flags & x64::ZeroFlag;
  const bool Sign = Flags & x64::SignFlag;
  const bool Overflow = Flags & x64::OverflowFlag;
  switch (Instruction) {
#define NEVERD_X64_CONDITION(Name, Expression)                                 \
  case X86_INS_SET##Name:                                                      \
    return Expression;
#include "X64Conditions.def"
#undef NEVERD_X64_CONDITION
  default:
    return std::nullopt;
  }
}
std::optional<unsigned> updateArity(unsigned Instruction) {
  switch (Instruction) {
#define NEVERD_X64_MEMORY_UPDATE(Name, Arity, Expression)                      \
  case X86_INS_##Name:                                                         \
    return Arity;
#include "X64MemoryUpdates.def"
#undef NEVERD_X64_MEMORY_UPDATE
  default:
    return std::nullopt;
  }
}
uint64_t updateValue(unsigned Instruction, uint64_t Original, uint64_t Operand,
                     uint64_t Flags) {
  const uint64_t Carry = bool(Flags & x64::CarryFlag);
  switch (Instruction) {
#define NEVERD_X64_MEMORY_UPDATE(Name, Arity, Expression)                      \
  case X86_INS_##Name:                                                         \
    return Expression;
#include "X64MemoryUpdates.def"
#undef NEVERD_X64_MEMORY_UPDATE
  default:
    llvm_unreachable(diagnostic::Instruction);
  }
}
bool isXmm(unsigned R) { return R >= X86_REG_XMM0 && R <= X86_REG_XMM15; }
enum class VectorForm {
#define NEVERD_X64_VECTOR_FORM(Name) Name,
#include "X64VectorOperands.def"
#undef NEVERD_X64_VECTOR_FORM
};
enum class VectorOperand {
  Absent,
  Xmm,
  Memory,
  General,
  Integer,
  Integer32,
  IntegerMemory,
  Immediate
};
struct VectorOperandPattern {
  VectorForm Form;
  VectorOperand Destination, Source, Control;
};
constexpr VectorOperandPattern VectorOperands[] = {
#define NEVERD_X64_VECTOR_OPERANDS(Form, Destination, Source, Control)         \
  {VectorForm::Form, VectorOperand::Destination, VectorOperand::Source,        \
   VectorOperand::Control},
#include "X64VectorOperands.def"
#undef NEVERD_X64_VECTOR_OPERANDS
};
struct VectorOperation {
  unsigned Width, Alignment;
  bool Move;
  VectorForm Form;
  unsigned StoreLane;
};
std::optional<VectorOperation> vectorOperation(unsigned Instruction) {
  switch (Instruction) {
#define NEVERD_X64_VECTOR_INSTRUCTION(Name, Width, Alignment, Move, Form,      \
                                      Lane)                                    \
  case X86_INS_##Name:                                                         \
    return VectorOperation{Width, Alignment, Move, VectorForm::Form, Lane};
#include "X64VectorInstructions.def"
#undef NEVERD_X64_VECTOR_INSTRUCTION
  default:
    return std::nullopt;
  }
}
bool isGeneralOperand(const cs_x86_op &O) {
  if (O.type != X86_OP_REG)
    return false;
  switch (O.reg) {
#define NEVERD_X64_OPERAND_REGISTER(Name, Slot, Shift, Bits)                   \
  case X86_REG_##Name:                                                         \
    return X64Register::Slot != X64Register::PC && Shift == 0 &&               \
           Bits / CHAR_BIT == O.size;
#include "X64OperandRegisters.def"
#undef NEVERD_X64_OPERAND_REGISTER
  default:
    return false;
  }
}
bool matchesVectorOperand(VectorOperand Kind, const cs_x86_op &O,
                          unsigned Width) {
  switch (Kind) {
  case VectorOperand::Absent:
    return false;
  case VectorOperand::Xmm:
    return O.type == X86_OP_REG && isXmm(O.reg);
  case VectorOperand::Memory:
    return O.type == X86_OP_MEM && O.size == Width;
  case VectorOperand::General:
    return isGeneralOperand(O) && O.size == Width;
  case VectorOperand::Integer:
    return isGeneralOperand(O) &&
           (O.size == x64::DWordBytes || O.size == x64::WordBytes);
  case VectorOperand::Integer32:
    return isGeneralOperand(O) && O.size == x64::DWordBytes;
  case VectorOperand::IntegerMemory:
    return O.type == X86_OP_MEM &&
           (O.size == x64::DWordBytes || O.size == x64::WordBytes);
  case VectorOperand::Immediate:
    return O.type == X86_OP_IMM && O.imm >= 0 && O.imm <= UINT8_MAX;
  }
  llvm_unreachable(diagnostic::Instruction);
}
bool admitsVectorOperands(const cs_x86 &X, const VectorOperation &V) {
  // PEXTRW shares a decoder identity with its SSE4.1 r/m16 encoding. The
  // checked SSE2 contract admits only 66 0F C5; its GPR result clears all
  // bits above bit 15, including when the ignored REX.W prefix is present.
  if (V.Form == VectorForm::WordExtract && X.opcode[1] != 0xc5)
    return false;
  return llvm::any_of(VectorOperands, [&](const auto &Pattern) {
    const bool HasControl = Pattern.Control != VectorOperand::Absent;
    return Pattern.Form == V.Form && X.op_count == (HasControl ? 3 : 2) &&
           matchesVectorOperand(Pattern.Destination, X.operands[0], V.Width) &&
           matchesVectorOperand(Pattern.Source, X.operands[1], V.Width) &&
           (!HasControl ||
            matchesVectorOperand(Pattern.Control, X.operands[2], V.Width));
  });
}
std::optional<unsigned> atomicPairWidth(unsigned Instruction) {
  switch (Instruction) {
#define NEVERD_X64_ATOMIC_INSTRUCTION(Name, PairWidth)                         \
  case X86_INS_##Name:                                                         \
    return PairWidth;
#include "X64AtomicInstructions.def"
#undef NEVERD_X64_ATOMIC_INSTRUCTION
  default:
    return std::nullopt;
  }
}
} // namespace
unsigned CheckedX64Backend::implicitStackWidth(const cs_x86 &X) {
  // The decoder identity retains 66H even when a later REX.W selects 64
  // bits. PUSHF, POPF, ENTER and LEAVE use the effective operand-size prefix.
  return X.prefix[2] == X86_PREFIX_OPSIZE && !(X.rex & x64::RexW)
             ? x64::HalfWordBytes
             : x64::WordBytes;
}
bool CheckedX64Backend::canonicalRange(uint64_t A, uint64_t N) const {
  return x64::canonicalRange(A, N);
}

llvm::Expected<std::unique_ptr<ExecutionBackend>>
CheckedX64Backend::create(std::unique_ptr<MemoryProjection> Memory,
                          std::unique_ptr<X64Machine> Machine, bool UserMode,
                          bool SIMDExceptions, bool Direct) {
  auto B = std::unique_ptr<CheckedX64Backend>(
      new CheckedX64Backend(UserMode, SIMDExceptions, Direct));
  B->Memory = std::move(Memory);
  B->Machine = std::move(Machine);
  B->CPU.UserMode = UserMode;
  if (auto E = B->Memory->validateMappings(x64::canonicalRange, !UserMode))
    return E;
  if (auto E = B->initializeDecoder(
          CS_ARCH_X86, x64::decoderMode(B->Machine->branchModel())))
    return E;
  B->CPU.reg(X64Register::FLAGS) = x64::InitialFlags;
  B->CPU.reg(X64Register::CS) =
      UserMode ? x64::UserCodeSelector : x64::CodeSelector;
  B->CPU.reg(X64Register::SS) =
      UserMode ? x64::UserDataSelector : x64::DataSelector;
  return std::unique_ptr<ExecutionBackend>(std::move(B));
}

llvm::Expected<RegisterValue> CheckedX64Backend::readRegister(CPURegister R) {
  if (!registerMatches(R, architecture()))
    return error(diagnostic::Register);
  if (R == CPURegister::X64GSBase)
    return RegisterValue{CPU.GSBase, 0};
  if (R == CPURegister::X64FSBase)
    return RegisterValue{CPU.FSBase, 0};
  if (R == CPURegister::X64MXCSR)
    return RegisterValue{CPU.MXCSR, 0};
  if (R >= CPURegister::X64V0 && R <= CPURegister::X64V15)
    return CPU.Xmm[unsigned(R) - unsigned(CPURegister::X64V0)];
  if (isX64FPRegister(R))
    return readX64FPRegister(CPU.FP, R);
  return RegisterValue{CPU.Registers[unsigned(R)], 0};
}
llvm::Expected<RegisterValue>
CheckedX64Backend::supportedControlBits(CPURegister R) const {
  if (R == CPURegister::X64MXCSR)
    return RegisterValue{Machine->mxcsrMask(), 0};
  return ExecutionBackend::supportedControlBits(R);
}
llvm::Error CheckedX64Backend::writeRegister(CPURegister R,
                                             const RegisterValue &V) {
  if (auto E = mutableMemory())
    return E;
  if (!registerMatches(R, architecture()) || !registerValueFits(R, V))
    return error(diagnostic::Register);
  if (isX64FPRegister(R))
    return writeX64FPRegister(CPU.FP, R, V);
  if (R == CPURegister::X64MXCSR) {
    // DAZ is admitted only by the machine's immutable capability contract.
    // Unmasked execution requires the resolved semantic exception capability.
    if (V[1] || (V[0] & ~uint64_t(Machine->mxcsrMask())) || !permitsMXCSR(V[0]))
      return error(diagnostic::Register);
    CPU.MXCSR = V[0];
    return llvm::Error::success();
  }
  if (R >= CPURegister::X64V0 && R <= CPURegister::X64V15) {
    CPU.Xmm[unsigned(R) - unsigned(CPURegister::X64V0)] = V;
    return llvm::Error::success();
  }
  if (V[1] || (R == CPURegister::X64CR8 && V[0] > x64::MaxCR8) ||
      (R == CPURegister::X64CS &&
       V[0] != (UserMode ? x64::UserCodeSelector : x64::CodeSelector)) ||
      (R == CPURegister::X64SS &&
       V[0] != (UserMode ? x64::UserDataSelector : x64::DataSelector)) ||
      (R == CPURegister::X64FLAGS &&
       ((V[0] & ~x64::AllowedFlags) || !(V[0] & x64::ReservedFlag))) ||
      ((R == CPURegister::X64GSBase || R == CPURegister::X64FSBase) &&
       !x64::canonical(V[0])))
    return error(diagnostic::Register);
  if (R == CPURegister::X64GSBase)
    CPU.GSBase = V[0];
  else if (R == CPURegister::X64FSBase)
    CPU.FSBase = V[0];
  else
    CPU.Registers[unsigned(R)] = V[0];
  return llvm::Error::success();
}

llvm::Expected<std::unique_ptr<BackendContext>>
CheckedX64Backend::saveContext() {
  if (auto E = checkExecutionState())
    return E;
  auto S = std::make_unique<SavedState>();
  S->Owner = Identity;
  S->Space = addressSpace();
  S->CPU = CPU;
  return makeContext(std::move(S));
}

llvm::Error CheckedX64Backend::saveContext(BackendContext &C) {
  if (!contextStorage(C) || contextStorage(C)->Owner.expired())
    return error(diagnostic::ContextExpired);
  if (contextStorage(C)->Owner.lock() != Identity)
    return error(diagnostic::ContextOwner);
  if (auto E = checkExecutionState())
    return E;
  contextStorage(C)->Space = addressSpace();
  static_cast<SavedState &>(*contextStorage(C)).CPU = CPU;
  return llvm::Error::success();
}

llvm::Error CheckedX64Backend::restoreContext(const BackendContext &C) {
  if (!contextStorage(C) || contextStorage(C)->Owner.expired())
    return error(diagnostic::ContextExpired);
  if (contextStorage(C)->Owner.lock() != Identity)
    return error(diagnostic::ContextOwner);
  if (auto E = mutableMemory())
    return E;
  if (contextStorage(C)->Space.lock() != addressSpace())
    return error(diagnostic::ContextSpace);
  CPU = static_cast<const SavedState &>(*contextStorage(C)).CPU;
  TimedOut = false;
  return llvm::Error::success();
}

llvm::Expected<uint64_t> CheckedX64Backend::operandRegister(unsigned R) const {
  if (R == X86_REG_INVALID)
    return uint64_t(0);
  switch (R) {
#define NEVERD_X64_OPERAND_REGISTER(ID, Name, Shift, Bits)                     \
  case X86_REG_##ID:                                                           \
    return (CPU.reg(X64Register::Name) >> Shift) &                             \
           (UINT64_MAX >> (x64::WordBits - Bits));
#include "X64OperandRegisters.def"
#undef NEVERD_X64_OPERAND_REGISTER
  default:
    return llvm::make_error<UnsupportedExecutionError>();
  }
}

std::optional<ServiceRequest>
CheckedX64Backend::decodeServiceRequest(const cs_insn &I) const {
#define NEVERD_X64_SERVICE(Kind, Name, ...)                                    \
  if (I.id == X86_INS_##Name) {                                                \
    constexpr uint8_t Bytes[] = {__VA_ARGS__};                                 \
    if (llvm::ArrayRef(I.bytes, I.size) == llvm::ArrayRef(Bytes))              \
      return ServiceRequest{ServiceRequestKind::Kind, I.address,               \
                            I.address + I.size};                               \
  }
#include "X64ServiceInstructions.def"
#undef NEVERD_X64_SERVICE
  return std::nullopt;
}

namespace {
std::vector<ExecutionWatch>
watchesExceptPage(llvm::ArrayRef<ExecutionWatch> Watches, uint64_t Page) {
  std::vector<ExecutionWatch> Open;
  const uint64_t PageLast = Page | (x64::PageSize - 1);
  for (const ExecutionWatch &W : Watches) {
    const uint64_t Last = W.Address + (W.Size - 1);
    if (Last < Page || W.Address > PageLast) {
      Open.push_back(W);
      continue;
    }
    if (W.Address < Page)
      Open.push_back({W.Address, Page - W.Address});
    if (Last > PageLast)
      Open.push_back({PageLast + 1, Last - PageLast});
  }
  return Open;
}
} // namespace

llvm::Error CheckedX64Backend::stepWatchedInstruction(bool WatchWrites) {
  const uint64_t PC = CPU.reg(X64Register::PC);
  const uint64_t Page = PC & ~(x64::PageSize - 1);
  auto Open = watchesExceptPage(FetchWatchPages, Page);
  // A resumed instruction can itself cross into a second watched page. Open
  // both fetch pages for this one processor step, retaining base permissions.
  if (PC <= UINT64_MAX - (x64::MaxInstructionBytes - 1)) {
    const uint64_t Last =
        (PC + x64::MaxInstructionBytes - 1) & ~(x64::PageSize - 1);
    if (Last != Page)
      Open = watchesExceptPage(Open, Last);
  }
  // The punched overlay must not reuse the full watch projection, and the
  // following free run must not reuse the punched one.
  ++WatchEpoch;
  auto Root = buildX64PageTables(
      *Memory, UserMode, Machine->requiresExceptionMonitor(), Open, WatchEpoch,
      WatchWrites, WatchWrites ? RegionPhysical : std::nullopt, &WatchTables);
  ++WatchEpoch;
  if (!Root)
    return Root.takeError();
  return stepDirectInstruction(*Root);
}

llvm::Error CheckedX64Backend::publishDirectException(
    const X64Exception &Raised, bool AllowSplit, bool &Resume,
    bool WritesProtected, std::optional<uint64_t> OpenPage) {
  Resume = false;
  const uint64_t PC = CPU.reg(X64Register::PC);
  // The system call extension is disabled, so each service instruction
  // is undefined exactly where the guest requests the service.
  if (Raised.Vector == unsigned(x64::ExceptionVector::InvalidOpcode)) {
#define NEVERD_X64_SERVICE(Kind, Name, ...)                                    \
  {                                                                            \
    constexpr uint8_t Expected[] = {__VA_ARGS__};                              \
    uint8_t Actual[sizeof(Expected)];                                          \
    if (!Memory->check(PC, sizeof(Expected), executionPermissions(Execute))) { \
      if (auto Failed = Memory->read(PC, Actual, Execute))                     \
        return Failed;                                                         \
      if (llvm::ArrayRef(Actual) == llvm::ArrayRef(Expected)) {                \
        PendingService = ServiceRequest{ServiceRequestKind::Kind, PC,          \
                                        PC + sizeof(Expected)};                \
        return llvm::Error::success();                                         \
      }                                                                        \
    }                                                                          \
  }
#include "X64ServiceInstructions.def"
#undef NEVERD_X64_SERVICE
  }
  // A fetch into a watched page faults only because the overlay made an
  // otherwise executable page non-executable. An instruction that starts in
  // the range is the watch, reported before it runs. An instruction that
  // started outside and only fetches its tail here is not: retire that one
  // instruction, then the next one faults at the same boundary a checked
  // watch uses.
  // The execute bit distinguishes a fetch from a data fault. A transport that
  // omits it still reports the overlay: the page stays present and readable,
  // so the fault is not a write and not a not-present miss.
  const bool FetchCode =
      !Raised.ErrorCode ||
      (*Raised.ErrorCode & x64::gateway::PageFaultExecute) ||
      ((*Raised.ErrorCode & x64::gateway::PageFaultPresent) &&
       !(*Raised.ErrorCode & x64::gateway::PageFaultWrite));
  const bool WatchPage =
      Raised.FaultAddress && llvm::any_of(FetchWatchPages, [&](const auto &W) {
        const uint64_t Page = *Raised.FaultAddress & ~(x64::PageSize - 1);
        return Page >= (W.Address & ~(x64::PageSize - 1)) &&
               Page <= ((W.Address + W.Size - 1) & ~(x64::PageSize - 1));
      });
  const bool FetchWatch =
      Raised.Vector == unsigned(x64::ExceptionVector::PageFault) &&
      Raised.FaultAddress && FetchCode && WatchPage &&
      !Memory->check(*Raised.FaultAddress, 1, executionPermissions(Execute));
  const bool RegionFetch =
      OpenPage && Raised.FaultAddress && FetchCode &&
      Raised.Vector == unsigned(x64::ExceptionVector::PageFault) &&
      (*Raised.FaultAddress & ~(x64::PageSize - 1)) != *OpenPage &&
      !Memory->check(*Raised.FaultAddress, 1, executionPermissions(Execute));
  // This private region closed all other code pages, including unwatched
  // ones. Restore the ordinary projection before entering the new page.
  if (RegionFetch && *Raised.FaultAddress == PC) {
    Resume = true;
    return llvm::Error::success();
  }
  const bool SplitFetch = ((FetchWatch && AllowSplit) || RegionFetch) &&
                          *Raised.FaultAddress >= PC &&
                          *Raised.FaultAddress - PC < x64::MaxInstructionBytes;
  // Write protection belongs only to this projection. The underlying guest
  // permissions still admit the store, including stores through another VA
  // naming the watched physical page. Retire it once with the overlay open,
  // then notify the owner before any subsequent guest instruction.
  bool WriteWatch = false, PublicWriteWatch = false, CodeWrite = false;
  if (WritesProtected &&
      Raised.Vector == unsigned(x64::ExceptionVector::PageFault) &&
      Raised.FaultAddress &&
      (Raised.ErrorCode ? bool(*Raised.ErrorCode & x64::gateway::PageFaultWrite)
                        : !SplitFetch) &&
      !Memory->check(*Raised.FaultAddress, 1, executionPermissions(Write))) {
    const uint64_t Page = *Raised.FaultAddress & ~(x64::PageSize - 1);
    const auto P = Memory->mappings().find(Page);
    if (P != Memory->mappings().end() && !P->second.IO) {
      PublicWriteWatch =
          Memory->writeWatched(P->second.Physical, x64::PageSize);
      CodeWrite = RegionPhysical == P->second.Physical;
      WriteWatch = PublicWriteWatch || CodeWrite;
    }
  }
  // A private region opens the instruction's first page. Some transports
  // fetch its watched tail before reporting an execution breakpoint, so the
  // current watched start must still stop before retiring that instruction.
  if ((FetchWatch && *Raised.FaultAddress == PC) || SplitFetch) {
    if (executionWatched(PC) && Hooks.Instruction)
      Hooks.Instruction(PC, 0);
    if (StopRequested || FirstFault)
      return llvm::Error::success();
  }
  // Some transports supply the fault address but no access/error code. A
  // possible fetch gets its ordinary one-step retry first. A possible write
  // is attributed to the overlay only if the retry without write protection
  // retires successfully; a repeated real fault is published unchanged.
  if (WriteWatch || SplitFetch) {
    RegionDirty |= CodeWrite;
    std::optional<uint64_t> NextOpen;
    auto Stepped = !WriteWatch && !OpenPage && !executionWatched(PC) &&
                           Machine->supportsExecutionStops()
                       ? runWatchedRegion(NextOpen)
                       : stepWatchedInstruction(!WriteWatch);
    if (!Stepped) {
      // A processor step may retire a self-branch or one REP iteration while
      // leaving RIP unchanged. Successful retirement is the evidence.
      if (PublicWriteWatch && Hooks.MemoryWritten)
        Hooks.MemoryWritten();
      Resume = !StopRequested;
      return llvm::Error::success();
    }
    if (Stepped.isA<MachineInterruptedError>())
      return Stepped;
    std::optional<X64Exception> Again;
    auto Other = llvm::handleErrors(
        std::move(Stepped),
        [&](const X64ExceptionError &Exception) -> llvm::Error {
          Again = Exception.exception();
          return llvm::Error::success();
        });
    if (Other)
      return Other;
    if (!Again)
      return error(diagnostic::DirectExecutionUnsupported);
    return publishDirectException(*Again, false, Resume, !WriteWatch, NextOpen);
  }
  BackendFault Fault{BackendFaultKind::Interrupt, PC};
  Fault.Interrupt = Raised.Vector;
  Fault.Address = Raised.FaultAddress;
  Fault.ErrorCode = Raised.ErrorCode;
  return raiseFault(Fault, true);
}

llvm::Error CheckedX64Backend::executeDirect() {
  for (;;) {
    if (RegionPage &&
        (CPU.reg(X64Register::PC) & ~(x64::PageSize - 1)) != *RegionPage) {
      leaveWatchedRegion();
    }
    auto Root = buildX64PageTables(
        *Memory, UserMode, Machine->requiresExceptionMonitor(), FetchWatchPages,
        WatchEpoch, true, RegionPhysical, &WatchTables);
    if (!Root)
      return Root.takeError();
    auto Next = CPU;
    auto E = Machine->run(Next, *Root, {Deadline, &StopRequested});
    // Only an event ends a free run.
    if (!E)
      return error(diagnostic::DirectExecutionUnsupported);
    if (E.isA<MachineInterruptedError>()) {
      CPU = Next;
      return E;
    }
    std::optional<X64Exception> Raised;
    auto Other = llvm::handleErrors(
        std::move(E), [&](const X64ExceptionError &Exception) -> llvm::Error {
          CPU = Next;
          Raised = Exception.exception();
          return llvm::Error::success();
        });
    if (Other)
      return Other;
    if (!Raised)
      return error(diagnostic::DirectExecutionUnsupported);
    bool Resume = false;
    if (auto Failed = publishDirectException(*Raised, true, Resume))
      return Failed;
    if (!Resume)
      return llvm::Error::success();
  }
}

llvm::Error CheckedX64Backend::execute(const cs_insn &I) {
  const auto &X = I.detail->x86;
  if (StringRestart && StringRestart->PC != I.address)
    StringRestart.reset();
  if (I.id == X86_INS_ENTER)
    return executeEnter(I);
  // MOVSD and CMPSD share decoder identities with scalar SSE operations.
  // Only their string forms have two implicit memory operands.
  if ((I.id != X86_INS_MOVSD && I.id != X86_INS_CMPSD) ||
      (X.op_count == 2 && X.operands[0].type == X86_OP_MEM &&
       X.operands[1].type == X86_OP_MEM)) {
    switch (I.id) {
#define NEVERD_X64_STRING_INSTRUCTION(Name, Width, Kind)                       \
  case X86_INS_##Name:                                                         \
    return executeString(I, Width, StringOperation::Kind);
#include "X64StringInstructions.def"
#undef NEVERD_X64_STRING_INSTRUCTION
    default:
      break;
    }
  }
  switch (I.id) {
#define NEVERD_CHECKED_X64_INSTRUCTION(Name)                                   \
  case X86_INS_##Name:                                                         \
    break;
#include "CheckedX64Instructions.def"
#undef NEVERD_CHECKED_X64_INSTRUCTION
  default:
    return llvm::make_error<UnsupportedExecutionError>();
  }
  const bool LoadMXCSR = I.id == X86_INS_LDMXCSR;
  const bool StoreMXCSR = I.id == X86_INS_STMXCSR;
  if ((LoadMXCSR || StoreMXCSR) &&
      (X.op_count != 1 || X.operands[0].type != X86_OP_MEM ||
       X.operands[0].size != x64::DWordBytes || X.prefix[2]))
    return llvm::make_error<UnsupportedExecutionError>();
  const auto Vector = vectorOperation(I.id);
  if (Vector && !admitsVectorOperands(X, *Vector))
    return llvm::make_error<UnsupportedExecutionError>();
  const bool Locked = X.prefix[0] == X86_PREFIX_LOCK;
  const auto PairWidth = atomicPairWidth(I.id);
  const bool Atomic = PairWidth.has_value();
  const bool WideAtomic = PairWidth.value_or(0) != 0;
  // Address size selects RCX/ECX. The processor model owns 66H target width,
  // the decrement, unchanged flags and the signed rel8 displacement.
  if (isLoop(I.id) && !admitsLoopOperands(I))
    return llvm::make_error<UnsupportedExecutionError>();
  const bool ScalarShift = isScalarShift(I.id);
  if (ScalarShift && !admitsScalarShiftOperands(X))
    return llvm::make_error<UnsupportedExecutionError>();
  const bool DoubleShift = isDoubleShift(I.id);
  if (DoubleShift) {
    if (!admitsDoubleShiftOperands(X))
      return llvm::make_error<UnsupportedExecutionError>();
    const auto &Count = X.operands[2];
    const uint64_t RawCount = Count.type == X86_OP_IMM
                                  ? uint64_t(Count.imm)
                                  : CPU.reg(X64Register::CX);
    const uint64_t Mask = X.operands[0].size == x64::WordBytes
                              ? x64::WideShiftCountMask
                              : x64::ShiftCountMask;
    // Counts above the operand width have undefined destination and flags.
    // Reject them before memory admission or observers, on every transport.
    if ((RawCount & Mask) > X.operands[0].size * CHAR_BIT)
      return llvm::make_error<UnsupportedExecutionError>();
  }
  const auto Bit = bitAccess(I.id);
  const bool BitWrites = Bit == BitAccess::Update;
  if (Bit && !admitsBitOperands(X))
    return llvm::make_error<UnsupportedExecutionError>();
  if (Atomic && !WideAtomic &&
      (X.op_count != 2 || X.operands[1].type != X86_OP_REG ||
       (X.operands[0].type != X86_OP_REG && X.operands[0].type != X86_OP_MEM) ||
       X.operands[0].size != X.operands[1].size))
    return llvm::make_error<UnsupportedExecutionError>();
  if (WideAtomic) {
    if (X.op_count != 1 || X.operands[0].type != X86_OP_MEM ||
        X.operands[0].size != *PairWidth ||
        X.encoding.modrm_offset < x64::WideAtomicOpcodeBytes)
      return llvm::make_error<UnsupportedExecutionError>();
    // Normalized decoder metadata can hide REP/HLE prefixes. Admit only the
    // ordinary locked/unlocked operation; displacement bytes are not prefixes.
    for (uint8_t Prefix : llvm::ArrayRef(
             I.bytes, X.encoding.modrm_offset - x64::WideAtomicOpcodeBytes))
      if (Prefix == X86_PREFIX_REP || Prefix == X86_PREFIX_REPNE)
        return llvm::make_error<UnsupportedExecutionError>();
  }
  if ((X.prefix[0] && !Locked && !isPrefixedReturn(I)) ||
      (Locked && ((!updateArity(I.id) && !Atomic && !BitWrites) ||
                  X.operands[0].type != X86_OP_MEM)) ||
      (X.addr_size != x64::DWordBytes && X.addr_size != x64::WordBytes) ||
      ((I.id == X86_INS_RET || I.id == X86_INS_CALL) &&
       X.prefix[2] == X86_PREFIX_OPSIZE))
    return llvm::make_error<UnsupportedExecutionError>();
  // The 16-bit BSWAP encoding has undefined architectural results.
  if (I.id == X86_INS_BSWAP &&
      (X.op_count != 1 || (X.operands[0].size != x64::DWordBytes &&
                           X.operands[0].size != x64::WordBytes)))
    return llvm::make_error<UnsupportedExecutionError>();
  if (I.id == X86_INS_LAHF || I.id == X86_INS_SAHF) {
    if (X.op_count)
      return llvm::make_error<UnsupportedExecutionError>();
    // AH remains the implicit operand even with REX. Complete this small
    // register transfer in the ISA owner so a software transport cannot
    // reinterpret it as an explicit byte register and select SPL instead.
    auto Next = CPU;
    const uint64_t AX = CPU.reg(X64Register::AX);
    const uint64_t Flags = CPU.reg(X64Register::FLAGS);
    if (I.id == X86_INS_LAHF)
      Next.reg(X64Register::AX) =
          (AX & ~x64::AccumulatorHighByteMask) |
          (((Flags & x64::StatusByteFlags) | x64::ReservedFlag)
           << x64::AccumulatorHighByteShift);
    else
      Next.reg(X64Register::FLAGS) =
          (Flags & ~x64::StatusByteFlags) |
          ((AX >> x64::AccumulatorHighByteShift) & x64::StatusByteFlags);
    Next.reg(X64Register::PC) += I.size;
    if (!StopRequested && !FirstFault)
      CPU = Next;
    return llvm::Error::success();
  }
  std::vector<Access> Accesses;
  auto Value = [&](const cs_x86_op &O) -> llvm::Expected<uint64_t> {
    if (O.type == X86_OP_IMM)
      return uint64_t(O.imm);
    if (O.type == X86_OP_REG)
      return operandRegister(O.reg);
    return llvm::make_error<UnsupportedExecutionError>();
  };
  const auto StackPush = stackDirection(I.id);
  if (StackPush)
    if (auto E = prepareStack(I, *StackPush, Accesses))
      return E;
  for (unsigned N = 0; !StackPush && N < X.op_count; ++N) {
    const auto &O = X.operands[N];
    if (O.type == X86_OP_REG && !(Vector && isXmm(O.reg))) {
      auto V = operandRegister(O.reg);
      if (!V)
        return V.takeError();
    }
    if (O.type != X86_OP_MEM || I.id == X86_INS_LEA || I.id == X86_INS_NOP)
      continue;
    if (O.size > (Vector || WideAtomic ? x64::VectorBytes : x64::WordBytes) ||
        !O.size)
      return llvm::make_error<UnsupportedExecutionError>();
    uint64_t ExtraOffset = 0;
    if (Bit && X.operands[1].type == X86_OP_REG) {
      auto Offset = operandRegister(X.operands[1].reg);
      if (!Offset)
        return Offset.takeError();
      const int64_t Bits = O.size * CHAR_BIT;
      const int64_t Signed = llvm::SignExtend64(*Offset, Bits);
      // A signed register index selects a whole operand-sized word. Floor
      // division also handles negative indices; an immediate stays within
      // the base word. Address-size wrapping precedes the segment base.
      const int64_t Words = Signed / Bits - (Signed % Bits < 0);
      ExtraOffset = uint64_t(Words) * O.size;
    }
    auto Address = operandAddress(I, O, ExtraOffset);
    if (!Address)
      return Address.takeError();
    const uint64_t A = *Address;
    // Aligned SSE and CMPXCHG16B raise #GP(0) before any page lookup or data
    // observation, even for a failed comparison or an unlocked operation.
    // Keep this architectural outcome identical on native and software
    // transports, including an absent/protected operand or a wrapped span.
    if ((Vector && A % Vector->Alignment) ||
        (WideAtomic && *PairWidth == x64::VectorBytes && A % *PairWidth)) {
      BackendFault Fault{BackendFaultKind::Interrupt, I.address};
      Fault.Interrupt = unsigned(x64::ExceptionVector::GeneralProtection);
      Fault.ErrorCode = x64::NoSelectorErrorCode;
      Fault.Cause = BackendFaultCause::OperandAlignment;
      return raiseFault(Fault, true);
    }
    // Memory XCHG is implicitly locked, including unaligned ordinary RAM.
    // The RAM transaction retains the entry address and owns atomic publish;
    // the selected processor supplies the register result or precise fault.
    // Device atomics retain their separate natural-alignment requirement.
    if (Locked && I.id != X86_INS_XCHG && A % O.size)
      return llvm::make_error<UnsupportedExecutionError>();
    // Some SETcc and SSE destinations have advisory decoder access metadata
    // marking them as reads. The architecture owns their actual effects.
    unsigned OperandAccess = O.access;
    if (N == 0) {
      if (LoadMXCSR || StoreMXCSR)
        OperandAccess = LoadMXCSR ? CS_AC_READ : CS_AC_WRITE;
      else if (Bit)
        OperandAccess = BitWrites ? CS_AC_READ | CS_AC_WRITE : CS_AC_READ;
      else if ((Vector && Vector->Move) || condition(I.id, 0))
        OperandAccess = CS_AC_WRITE;
      else if (updateArity(I.id))
        OperandAccess = CS_AC_READ | CS_AC_WRITE;
      else if (Atomic || DoubleShift || ScalarShift)
        OperandAccess = CS_AC_READ | CS_AC_WRITE;
    }
    if (OperandAccess == CS_AC_READ)
      Accesses.push_back({A, O.size, Read, 0});
    else if (StoreMXCSR)
      Accesses.push_back({A, O.size, Write, CPU.MXCSR});
    else if (Vector && Vector->Move && OperandAccess == CS_AC_WRITE && N == 0 &&
             X.operands[1].type == X86_OP_REG && isXmm(X.operands[1].reg)) {
      const auto &V = CPU.Xmm[X.operands[1].reg - X86_REG_XMM0];
      Accesses.push_back(
          {A, O.size, Write, V[Vector->StoreLane], std::nullopt, V[1]});
    } else if (OperandAccess == CS_AC_WRITE &&
               (I.id == X86_INS_MOV || I.id == X86_INS_MOVABS) && N == 0 &&
               X.op_count == 2) {
      auto V = Value(X.operands[1]);
      if (!V)
        return V.takeError();
      Accesses.push_back({A, O.size, Write, *V});
    } else if (OperandAccess == CS_AC_WRITE && N == 0 && X.op_count == 1 &&
               O.size == x64::ByteBytes) {
      auto Condition = condition(I.id, CPU.reg(X64Register::FLAGS));
      if (!Condition)
        return llvm::make_error<UnsupportedExecutionError>();
      Accesses.push_back({A, O.size, Write, uint64_t(*Condition)});
    } else if ((Atomic || BitWrites || DoubleShift || ScalarShift) && N == 0 &&
               OperandAccess == (CS_AC_READ | CS_AC_WRITE)) {
      Accesses.push_back({A, O.size, Read, 0});
      Accesses.push_back({A, O.size, Write, 0, std::nullopt, 0, true});
    } else if (OperandAccess == (CS_AC_READ | CS_AC_WRITE) && N == 0) {
      auto Arity = updateArity(I.id);
      if (!Arity || X.op_count != *Arity)
        return llvm::make_error<UnsupportedExecutionError>();
      uint64_t Operand = 0;
      if (*Arity == 2) {
        auto V = Value(X.operands[1]);
        if (!V)
          return V.takeError();
        Operand = *V;
      }
      Accesses.push_back({A, O.size, Read, 0});
      Accesses.push_back({A, O.size, Write, Operand, I.id});
    } else
      return llvm::make_error<UnsupportedExecutionError>();
  }
  uint64_t SP = CPU.reg(X64Register::SP);
  const bool PushFlags = I.id == X86_INS_PUSHF || I.id == X86_INS_PUSHFQ;
  const bool PopFlags = I.id == X86_INS_POPF || I.id == X86_INS_POPFQ;
  if (PushFlags || PopFlags) {
    if (X.op_count)
      return llvm::make_error<UnsupportedExecutionError>();
    const unsigned Size = implicitStackWidth(X);
    // Long mode always uses RSP for the implicit stack, even with 67H or
    // a segment override. Public flags exclude VM, RF and transport TF.
    if (PushFlags)
      Accesses.push_back({SP - Size, Size, Write, CPU.reg(X64Register::FLAGS)});
    else
      Accesses.push_back({SP, Size, Read, 0});
  }
  if (I.id == X86_INS_CALL)
    Accesses.push_back(
        {SP - x64::WordBytes, x64::WordBytes, Write, I.address + I.size});
  if (I.id == X86_INS_RET)
    Accesses.push_back({SP, x64::WordBytes, Read, 0});
  if (I.id == X86_INS_LEAVE) {
    // LEAVE has no explicit operands. Inspect its bytes because the decoder
    // may omit ignored REP prefixes from normalized metadata.
    if (X.op_count ||
        llvm::any_of(llvm::ArrayRef(I.bytes, I.size), [](uint8_t B) {
          return B == X86_PREFIX_LOCK || B == X86_PREFIX_REP ||
                 B == X86_PREFIX_REPNE;
        }))
      return llvm::make_error<UnsupportedExecutionError>();
    // In long mode the implicit stack address is the original full RBP,
    // independent of 67H, segment overrides and the old RSP. Preflight the
    // complete read before the processor publishes either RSP or RBP.
    Accesses.push_back(
        {CPU.reg(X64Register::BP), implicitStackWidth(X), Read, 0});
  }
  // RAM operands may cross pages with unrelated physical owners. Validate
  // their whole extent before native execution; a synchronous fault cannot
  // publish a prefix store. Device transfers remain indivisible and cannot
  // combine RAM and device pages or two independent device transactions.
  for (const auto &A : Accesses) {
    if (A.Size - 1 > UINT64_MAX - A.Address)
      return access(A.Address, A.Size, A.Permission, true, true);
    const uint64_t Last = A.Address + A.Size - 1;
    if (A.Address / x64::PageSize != Last / x64::PageSize &&
        (deviceAt(A.Address) || deviceAt(Last)))
      return llvm::make_error<UnsupportedExecutionError>();
  }
  // A device transfer is one bounded scalar MOV, MOVZX or MOVSX. RMW, wide
  // vector and implicit-stack device accesses cannot be split into callbacks.
  const Access *DeviceAccess = nullptr;
  for (const auto &A : Accesses)
    if (deviceAt(A.Address)) {
      if ((Atomic || BitWrites || updateArity(I.id)) && Accesses.size() == 2 &&
          Accesses[0].Permission == Read && Accesses[1].Permission == Write &&
          Accesses[0].Address == Accesses[1].Address &&
          Accesses[0].Size == Accesses[1].Size)
        return deviceAtomic(I, A.Address, A.Size);
      if (Accesses.size() != 1 || Locked ||
          (I.id != X86_INS_MOV && I.id != X86_INS_MOVABS &&
           I.id != X86_INS_MOVZX && I.id != X86_INS_MOVSX) ||
          (A.Permission == Read && X.operands[0].type != X86_OP_REG))
        return llvm::make_error<UnsupportedExecutionError>();
      DeviceAccess = &A;
    }
  for (const auto &A : Accesses) {
    if (A.Permission == Read && Hooks.Read)
      Hooks.Read(A.Address, A.Size);
    if (A.Permission == Write && Hooks.Write && !A.Deferred) {
      uint64_t Value = A.Value;
      if (A.Update) {
        // The preceding read access has already checked user permissions and
        // offered a pre-effect stop. The physical execution lease prevents a
        // second CPU from changing these bytes before the actual instruction.
        auto Original = readInteger(A.Address, A.Size);
        if (!Original)
          return Original.takeError();
        Value = updateValue(*A.Update, *Original, A.Value,
                            CPU.reg(X64Register::FLAGS)) &
                (UINT64_MAX >> (x64::WordBits - A.Size * CHAR_BIT));
      }
      // The scalar observer carries at most one word. A full-width store is
      // observed as two ordered words, both before any instruction effect.
      Hooks.Write(A.Address, std::min<unsigned>(A.Size, x64::WordBytes),
                  A.Size < x64::WordBytes
                      ? Value &
                            (UINT64_MAX >> (x64::WordBits - A.Size * CHAR_BIT))
                      : Value);
      if (!StopRequested && !FirstFault && A.Size > x64::WordBytes)
        Hooks.Write(A.Address + x64::WordBytes, A.Size - x64::WordBytes,
                    A.High);
    }
    if (StopRequested || FirstFault)
      return llvm::Error::success();
    // Every exchange performs a write access, including a failed comparison.
    // Check both permissions at the read boundary so an absent or protected
    // operand reports the architectural RMW write fault before CPU entry.
    const unsigned Permissions =
        Atomic && A.Permission == Read ? Read | Write : A.Permission;
    if (auto E = access(A.Address, A.Size, Permissions, true, true))
      return E;
    if (StopRequested)
      return llvm::Error::success();
  }
  if (LoadMXCSR) {
    auto Value = readInteger(Accesses.front().Address, x64::DWordBytes);
    if (!Value)
      return Value.takeError();
    // Reserved bits fault before changing MXCSR. An otherwise architectural
    // value with unmasked exceptions requires precise machine delivery.
    if (*Value & ~uint64_t(Machine->mxcsrMask())) {
      BackendFault Fault{BackendFaultKind::Interrupt, I.address};
      Fault.Interrupt = unsigned(x64::ExceptionVector::GeneralProtection);
      Fault.ErrorCode = x64::NoSelectorErrorCode;
      return raiseFault(Fault, true);
    }
    if (!permitsMXCSR(*Value))
      return llvm::make_error<UnsupportedExecutionError>();
  }
  if (DeviceAccess)
    return deviceTransfer(I, DeviceAccess->Address, DeviceAccess->Size,
                          DeviceAccess->Permission, DeviceAccess->Value);
  if (PopFlags) {
    const unsigned Size = Accesses.back().Size;
    auto Value = readInteger(SP, Size);
    if (!Value)
      return Value.takeError();
    uint64_t Mask =
        Size == x64::HalfWordBytes ? x64::PopWordFlags : x64::PopQwordFlags;
    // Intel SDM POPF: IOPL changes only at CPL0. The checked profiles keep
    // IOPL zero, so CPL3 cannot change IF. Reserved bits, VM, VIF and VIP
    // remain unchanged; RF clears independently of the stack image.
    if (!UserMode)
      Mask |= x64::InterruptFlag | x64::IOPrivilegeFlags;
    const uint64_t Flags =
        ((CPU.reg(X64Register::FLAGS) & ~Mask) | (*Value & Mask)) &
        ~x64::ResumeFlag;
    // Validate the effective result, not ignored input bits. Guest TF and
    // other unmodeled control flags cannot silently enter the CPU contract.
    if (Flags & ~x64::AllowedFlags)
      return llvm::make_error<UnsupportedExecutionError>();
    if (StopRequested || FirstFault)
      return llvm::Error::success();
    // Native POPF could clear the transport's private TF and run beyond this
    // boundary. Complete the ISA transition under the physical read lease,
    // without patching the stack (which can be read-only or alias code).
    auto Next = CPU;
    Next.reg(X64Register::FLAGS) = Flags;
    Next.reg(X64Register::SP) += Size;
    Next.reg(X64Register::PC) += I.size;
    CPU = Next;
    return llvm::Error::success();
  }
  auto Root = buildX64PageTables(*Memory, UserMode,
                                 Machine->requiresExceptionMonitor());
  if (!Root)
    return Root.takeError();
  std::vector<RAMWriteRange> Writes;
  std::vector<RAMWriteRange> Inputs{{I.address, I.size}};
  for (const auto &A : Accesses)
    if (A.Permission == Write)
      Writes.push_back({A.Address, A.Size});
  for (const auto &A : Accesses)
    Inputs.push_back({A.Address, A.Size});
  auto Transaction = RAMTransaction::create(
      *Memory, Writes, execution_limits::InstructionRAMWriteBytes,
      executionPermissions(Write));
  if (!Transaction)
    return Transaction.takeError();
  auto Next = CPU;
  if (auto E = (*Transaction)
                   ->execute(
                       [&] {
                         return Machine->step(Next, *Root,
                                              {Deadline, &StopRequested});
                       },
                       Inputs)) {
    // Discard speculative RAM before the OS receives a processor exception.
    // Its architectural fault state (including FP status) remains
    // authoritative.
    Transaction->reset();
    return llvm::handleErrors(std::move(E), [&](const X64ExceptionError &E) {
      CPU = Next;
      BackendFault Fault{BackendFaultKind::Interrupt, I.address};
      Fault.Interrupt = E.exception().Vector;
      Fault.Address = E.exception().FaultAddress;
      Fault.ErrorCode = E.exception().ErrorCode;
      return raiseFault(Fault, true);
    });
  }
  if (PushFlags) {
    // Native single stepping may expose its private TF in the stack image.
    // Publish the admitted architectural flags under the existing RAM lease
    // and transaction; cancellation still rolls back every written byte.
    const auto &A = Accesses.back();
    for (unsigned N = 0; N < A.Size; ++N) {
      const uint64_t Address = A.Address + N;
      const auto &P = Memory->mappings().at(Address & ~(x64::PageSize - 1));
      *Memory->physicalPointer(P.Physical + Address % x64::PageSize) =
          uint8_t(A.Value >> (N * CHAR_BIT));
    }
  }
  if (auto E = (*Transaction)->stage())
    return E;
  if (StopRequested || FirstFault)
    return llvm::Error::success();
  for (const auto &A : Accesses)
    if (A.Deferred && Hooks.Write) {
      // A wide result uses two scalar observations over one transaction.
      // Neither half is published until both callbacks accept the result.
      for (unsigned Offset = 0; Offset < A.Size; Offset += x64::WordBytes) {
        const unsigned Size =
            std::min<unsigned>(A.Size - Offset, x64::WordBytes);
        std::array<uint8_t, x64::WordBytes> Bytes{};
        if (auto E = (*Transaction)
                         ->read(A.Address + Offset,
                                llvm::MutableArrayRef(Bytes.data(), Size)))
          return E;
        uint64_t Value = 0;
        for (unsigned N = 0; N < Size; ++N)
          Value |= uint64_t(Bytes[N]) << (N * CHAR_BIT);
        Hooks.Write(A.Address + Offset, Size, Value);
        if (StopRequested || FirstFault)
          return llvm::Error::success();
      }
    }
  if (auto E = (*Transaction)->commit())
    return E;
  CPU = Next;
  return llvm::Error::success();
}

} // namespace neverd::emulation
