//===- CheckedX64Backend.cpp - Checked x64 execution---------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "CheckedX64Backend.h"

#include "../../core/ExecutionDiagnostics.h"

#include "llvm/ADT/ScopeExit.h"

#include <chrono>
#include <exception>
#include <utility>

namespace neverd::emulation {
using diagnostic::error;
namespace {
bool canonicalRange(uint64_t A, uint64_t N) {
  return N && N - 1 <= UINT64_MAX - A && x64::canonical(A) &&
         x64::canonical(A + N - 1) &&
         ((A <= x64::UserMax) == (A + N - 1 <= x64::UserMax));
}
} // namespace

llvm::Expected<std::unique_ptr<ExecutionBackend>>
CheckedX64Backend::create(ExecutionBackendKind Kind, uint64_t Limit) {
  auto B = std::unique_ptr<CheckedX64Backend>(new CheckedX64Backend());
  auto Memory = PhysicalMemory::create(Limit);
  if (!Memory)
    return Memory.takeError();
  B->Memory = std::move(*Memory);
  auto Machine = Kind == ExecutionBackendKind::KVM
                     ? createKvmMachine(B->Memory->data(), B->Memory->size())
                     : createWhpMachine(B->Memory->data(), B->Memory->size());
  if (!Machine)
    return Machine.takeError();
  B->Machine = std::move(*Machine);
  if (cs_open(CS_ARCH_X86, CS_MODE_64, &B->Decoder) != CS_ERR_OK ||
      cs_option(B->Decoder, CS_OPT_DETAIL, CS_OPT_ON) != CS_ERR_OK)
    return error(diagnostic::Decode);
  B->CPU.reg(X64Register::FLAGS) = x64::InitialFlags;
  B->CPU.reg(X64Register::CS) = x64::CodeSelector;
  B->CPU.reg(X64Register::SS) = x64::DataSelector;
  return std::unique_ptr<ExecutionBackend>(std::move(B));
}

CheckedX64Backend::~CheckedX64Backend() {
  if (Decoder)
    cs_close(&Decoder);
}

llvm::Error CheckedX64Backend::mutableMemory() const {
  if (FirstFault || RecoverableFault)
    return error(diagnostic::Faulted);
  if (Running)
    return error(diagnostic::Running);
  return llvm::Error::success();
}

llvm::Error CheckedX64Backend::map(uint64_t A, uint64_t N, unsigned P) {
  if (auto E = mutableMemory())
    return E;
  if (!canonicalRange(A, N))
    return error(diagnostic::InvalidMapping);
  return Memory->map(A, N, P);
}

llvm::Error CheckedX64Backend::mapAlias(uint64_t A, uint64_t S, uint64_t N,
                                        unsigned P) {
  return replaceAliases({}, {GuestAliasMapping{A, S, N, P}});
}

llvm::Error CheckedX64Backend::unmapAlias(uint64_t A, uint64_t N) {
  return replaceAliases({GuestAliasRange{A, N}}, {});
}

llvm::Error
CheckedX64Backend::replaceAliases(llvm::ArrayRef<GuestAliasRange> Remove,
                                  llvm::ArrayRef<GuestAliasMapping> Add) {
  if (auto E = mutableMemory())
    return E;
  for (const auto &R : Add)
    if (!canonicalRange(R.Address, R.Size) || !canonicalRange(R.Source, R.Size))
      return error(diagnostic::InvalidMapping);
  return Memory->aliases(Remove, Add);
}

llvm::Error CheckedX64Backend::protect(uint64_t A, uint64_t N, unsigned P) {
  if (auto E = mutableMemory())
    return E;
  return Memory->protect(A, N, P);
}

llvm::Error CheckedX64Backend::access(uint64_t A, uint64_t N, unsigned P,
                                      bool Recoverable) {
  if (FirstFault)
    return error(diagnostic::Faulted);
  if (auto Kind = Memory->check(A, N, P)) {
    auto Access = P == Execute ? BackendAccessKind::Execute
                  : P == Write ? BackendAccessKind::Write
                               : BackendAccessKind::Read;
    BackendFault F{*Kind, CPU.reg(X64Register::PC), A, N, Access, std::nullopt};
    if (Recoverable && P != Execute && Hooks.RecoverableFault &&
        Hooks.RecoverableFault(F)) {
      RecoverableFault = F;
      StopRequested = true;
      return llvm::Error::success();
    }
    FirstFault = F;
    if (Hooks.Fault)
      Hooks.Fault(A, N, backendAccessKindName(Access));
    return error(diagnostic::MemoryAccess);
  }
  return llvm::Error::success();
}

llvm::Error CheckedX64Backend::read(uint64_t A,
                                    llvm::MutableArrayRef<uint8_t> B) {
  if (auto E = access(A, B.size(), Read))
    return E;
  return Memory->read(A, B);
}

llvm::Error CheckedX64Backend::write(uint64_t A, llvm::ArrayRef<uint8_t> B) {
  if (auto E = mutableMemory())
    return E;
  if (auto E = access(A, B.size(), Write))
    return E;
  return Memory->write(A, B);
}

llvm::Error CheckedX64Backend::fetch(uint64_t A,
                                     llvm::MutableArrayRef<uint8_t> B) {
  if (auto E = access(A, B.size(), Execute))
    return E;
  return Memory->read(A, B, Execute);
}

llvm::Expected<bool> CheckedX64Backend::canAccess(uint64_t A, uint64_t N,
                                                  unsigned P) const {
  if (FirstFault || (P & ~(Read | Write | Execute)))
    return error(diagnostic::Faulted);
  return !Memory->check(A, N, P);
}

llvm::Error CheckedX64Backend::validateBacking(uint64_t A, uint64_t N) const {
  if (auto E = mutableMemory())
    return E;
  if (Memory->check(A, N, 0))
    return error(diagnostic::MemoryAccess);
  return llvm::Error::success();
}

llvm::Error CheckedX64Backend::readBacking(uint64_t A,
                                           llvm::MutableArrayRef<uint8_t> B) {
  if (auto E = validateBacking(A, B.size()))
    return E;
  return Memory->read(A, B, 0);
}

llvm::Error CheckedX64Backend::writeBacking(uint64_t A,
                                            llvm::ArrayRef<uint8_t> B) {
  if (auto E = validateBacking(A, B.size()))
    return E;
  return Memory->write(A, B, 0);
}

llvm::Error
CheckedX64Backend::snapshotBacking(uint64_t A,
                                   llvm::MutableArrayRef<uint8_t> B) {
  if (Running)
    return error(diagnostic::Running);
  return Memory->read(A, B, 0);
}

llvm::Expected<uint64_t> CheckedX64Backend::reg(X64Register R) {
  if (unsigned(R) >= CPU.Registers.size())
    return error(diagnostic::Register);
  return CPU.reg(R);
}

llvm::Error CheckedX64Backend::setReg(X64Register R, uint64_t V) {
  if (auto E = mutableMemory())
    return E;
  if (unsigned(R) >= CPU.Registers.size() ||
      (R == X64Register::CR8 && V > x64::MaxCR8) ||
      (R == X64Register::CS && V != x64::CodeSelector) ||
      (R == X64Register::SS && V != x64::DataSelector) ||
      (R == X64Register::FLAGS &&
       ((V & ~x64::AllowedFlags) || !(V & x64::ReservedFlag))))
    return error(diagnostic::Register);
  CPU.reg(R) = V;
  return llvm::Error::success();
}

llvm::Error CheckedX64Backend::setGSBase(uint64_t A) {
  if (auto E = mutableMemory())
    return E;
  if (!x64::canonical(A))
    return error(diagnostic::Register);
  CPU.GSBase = A;
  return llvm::Error::success();
}

llvm::Expected<ExecutionBackend::XmmValue> CheckedX64Backend::xmm(unsigned R) {
  if (R >= CPU.Xmm.size())
    return error(diagnostic::Register);
  return CPU.Xmm[R];
}

llvm::Error CheckedX64Backend::setXmm(unsigned R, const XmmValue &V) {
  if (auto E = mutableMemory())
    return E;
  if (R >= CPU.Xmm.size())
    return error(diagnostic::Register);
  CPU.Xmm[R] = V;
  return llvm::Error::success();
}

llvm::Expected<std::unique_ptr<BackendContext>>
CheckedX64Backend::saveContext() {
  if (FirstFault || RecoverableFault)
    return error(diagnostic::Faulted);
  auto S = std::make_unique<SavedState>();
  S->Owner = Identity;
  S->CPU = CPU;
  return std::unique_ptr<BackendContext>(new BackendContext(std::move(S)));
}

llvm::Error CheckedX64Backend::saveContext(BackendContext &C) {
  if (!C.State || C.State->Owner.expired())
    return error(diagnostic::ContextExpired);
  if (C.State->Owner.lock() != Identity)
    return error(diagnostic::ContextOwner);
  if (FirstFault || RecoverableFault)
    return error(diagnostic::Faulted);
  static_cast<SavedState &>(*C.State).CPU = CPU;
  return llvm::Error::success();
}

llvm::Error CheckedX64Backend::restoreContext(const BackendContext &C) {
  if (!C.State || C.State->Owner.expired())
    return error(diagnostic::ContextExpired);
  if (C.State->Owner.lock() != Identity)
    return error(diagnostic::ContextOwner);
  if (auto E = mutableMemory())
    return E;
  CPU = static_cast<const SavedState &>(*C.State).CPU;
  TimedOut = false;
  return llvm::Error::success();
}

llvm::Error CheckedX64Backend::installHooks(BackendHooks H) {
  if (auto E = mutableMemory())
    return E;
  Hooks = std::move(H);
  return llvm::Error::success();
}

std::optional<BackendFault> CheckedX64Backend::takeRecoverableFault() {
  return std::exchange(RecoverableFault, std::nullopt);
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

llvm::Error CheckedX64Backend::execute(const cs_insn &I) {
  switch (I.id) {
#define NEVERD_CHECKED_X64_INSTRUCTION(Name)                                   \
  case X86_INS_##Name:                                                         \
    break;
#include "CheckedX64Instructions.def"
#undef NEVERD_CHECKED_X64_INSTRUCTION
  default:
    return llvm::make_error<UnsupportedExecutionError>();
  }
  const auto &X = I.detail->x86;
  if (X.prefix[0] ||
      (X.addr_size != x64::DWordBytes && X.addr_size != x64::WordBytes) ||
      ((I.id == X86_INS_RET || I.id == X86_INS_CALL) &&
       X.prefix[2] == X86_PREFIX_OPSIZE))
    return llvm::make_error<UnsupportedExecutionError>();
  // The 16-bit BSWAP encoding has undefined architectural results.
  if (I.id == X86_INS_BSWAP &&
      (X.op_count != 1 || (X.operands[0].size != x64::DWordBytes &&
                           X.operands[0].size != x64::WordBytes)))
    return llvm::make_error<UnsupportedExecutionError>();
  struct Access {
    uint64_t Address;
    unsigned Size, Permission;
    uint64_t Value;
  };
  std::vector<Access> Accesses;
  auto Value = [&](const cs_x86_op &O) -> llvm::Expected<uint64_t> {
    if (O.type == X86_OP_IMM)
      return uint64_t(O.imm);
    if (O.type == X86_OP_REG)
      return operandRegister(O.reg);
    return llvm::make_error<UnsupportedExecutionError>();
  };
  for (unsigned N = 0; N < X.op_count; ++N) {
    const auto &O = X.operands[N];
    if (O.type == X86_OP_REG) {
      auto V = operandRegister(O.reg);
      if (!V)
        return V.takeError();
    }
    if (O.type != X86_OP_MEM || I.id == X86_INS_LEA || I.id == X86_INS_NOP)
      continue;
    if (O.size > x64::WordBytes || !O.size ||
        (O.mem.segment != X86_REG_INVALID && O.mem.segment != X86_REG_DS &&
         O.mem.segment != X86_REG_SS && O.mem.segment != X86_REG_ES &&
         O.mem.segment != X86_REG_GS))
      return llvm::make_error<UnsupportedExecutionError>();
    auto B = operandRegister(O.mem.base), Index = operandRegister(O.mem.index);
    if (!B) {
      if (!Index)
        llvm::consumeError(Index.takeError());
      return B.takeError();
    }
    if (!Index)
      return Index.takeError();
    uint64_t A = *B + *Index * O.mem.scale + O.mem.disp;
    if (O.mem.base == X86_REG_RIP || O.mem.base == X86_REG_EIP)
      A += I.size;
    if (X.addr_size == x64::DWordBytes)
      A = uint32_t(A);
    if (O.mem.segment == X86_REG_GS)
      A += CPU.GSBase;
    if (O.access == CS_AC_READ)
      Accesses.push_back({A, O.size, Read, 0});
    else if (O.access == CS_AC_WRITE &&
             (I.id == X86_INS_MOV || I.id == X86_INS_MOVABS) && N == 0 &&
             X.op_count == 2) {
      auto V = Value(X.operands[1]);
      if (!V)
        return V.takeError();
      Accesses.push_back({A, O.size, Write, *V});
    } else
      return llvm::make_error<UnsupportedExecutionError>();
  }
  uint64_t SP = CPU.reg(X64Register::SP);
  if (I.id == X86_INS_PUSH || I.id == X86_INS_CALL) {
    uint64_t V = I.address + I.size;
    if (I.id == X86_INS_PUSH) {
      if (X.op_count != 1 || X.operands[0].size != x64::WordBytes)
        return llvm::make_error<UnsupportedExecutionError>();
      auto Source = Value(X.operands[0]);
      if (!Source)
        return Source.takeError();
      V = *Source;
    }
    Accesses.push_back({SP - x64::WordBytes, x64::WordBytes, Write, V});
  }
  if (I.id == X86_INS_POP || I.id == X86_INS_RET) {
    if (I.id == X86_INS_POP &&
        (X.op_count != 1 || X.operands[0].type != X86_OP_REG ||
         X.operands[0].size != x64::WordBytes))
      return llvm::make_error<UnsupportedExecutionError>();
    Accesses.push_back({SP, x64::WordBytes, Read, 0});
  }
  // Single-page transactions are the initial contract. Cross-page partial
  // effects and MMIO are not approximated by a sequence of host callbacks.
  for (const auto &A : Accesses)
    if (A.Size - 1 > UINT64_MAX - A.Address ||
        A.Address / x64::PageSize != (A.Address + A.Size - 1) / x64::PageSize)
      return llvm::make_error<UnsupportedExecutionError>();
  for (const auto &A : Accesses) {
    if (A.Permission == Read && Hooks.Read)
      Hooks.Read(A.Address, A.Size);
    if (A.Permission == Write && Hooks.Write)
      Hooks.Write(A.Address, A.Size, A.Value);
    if (StopRequested || FirstFault)
      return llvm::Error::success();
    if (auto E = access(A.Address, A.Size, A.Permission, true))
      return E;
    if (StopRequested)
      return llvm::Error::success();
  }
  auto Root = buildX64PageTables(*Memory, PageTableRoot);
  if (!Root)
    return Root.takeError();
  PageTableRoot = *Root;
  return Machine->step(CPU, PageTableRoot);
}

llvm::Error CheckedX64Backend::run(uint64_t PC, uint64_t Timeout) {
  if (auto E = mutableMemory())
    return E;
  CPU.reg(X64Register::PC) = PC;
  Running = true;
  TimedOut = false;
  StopRequested = false;
  auto Reset = llvm::scope_exit([&] { Running = false; });
  auto Deadline =
      std::chrono::steady_clock::now() + std::chrono::microseconds(Timeout);
  try {
    while (!StopRequested) {
      if (std::chrono::steady_clock::now() >= Deadline) {
        TimedOut = true;
        break;
      }
      PC = CPU.reg(X64Register::PC);
      std::array<uint8_t, x64::MaxInstructionBytes> Bytes{};
      size_t Count = 0;
      for (; Count < Bytes.size() && Count <= UINT64_MAX - PC; ++Count) {
        if (Memory->check(PC + Count, 1, Execute))
          break;
        if (auto E = Memory->read(
                PC + Count, llvm::MutableArrayRef<uint8_t>(&Bytes[Count], 1),
                Execute))
          return E;
      }
      cs_insn *Decoded = nullptr;
      if (!Count)
        return access(PC, 1, Execute);
      if (!cs_disasm(Decoder, Bytes.data(), Count, PC, 1, &Decoded)) {
        FirstFault = BackendFault{BackendFaultKind::InvalidInstruction, PC};
        if (Hooks.InvalidInstruction)
          Hooks.InvalidInstruction();
        return llvm::make_error<UnsupportedExecutionError>();
      }
      auto Free = llvm::scope_exit([&] { cs_free(Decoded, 1); });
      if (Hooks.Instruction)
        Hooks.Instruction(PC, Decoded->size);
      if (FirstFault)
        return error(diagnostic::Faulted);
      if (StopRequested)
        break;
      if (auto E = execute(*Decoded)) {
        if (!FirstFault) {
          const bool Unsupported = E.isA<UnsupportedExecutionError>();
          FirstFault =
              BackendFault{Unsupported ? BackendFaultKind::InvalidInstruction
                                       : BackendFaultKind::UnhandledException,
                           PC};
          if (Unsupported && Hooks.InvalidInstruction)
            Hooks.InvalidInstruction();
        }
        return E;
      }
      if (FirstFault)
        return error(diagnostic::Faulted);
    }
  } catch (...) {
    FirstFault = BackendFault{BackendFaultKind::UnhandledException,
                              CPU.reg(X64Register::PC)};
    return error(diagnostic::Callback);
  }
  return llvm::Error::success();
}
} // namespace neverd::emulation
