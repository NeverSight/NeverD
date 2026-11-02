//===- CheckedAArch64Backend.cpp - Checked ARM64 execution ----------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "CheckedAArch64Backend.h"

#include "../../core/ExecutionDiagnostics.h"
#include "../../core/RAMTransaction.h"
#include "AArch64InstructionEffects.h"

#include "llvm/Support/Endian.h"
#include "llvm/Support/MathExtras.h"

#include <algorithm>
#include <climits>

namespace neverd::emulation {
using diagnostic::error;
bool CheckedAArch64Backend::canonicalRange(uint64_t A, uint64_t N) const {
  return aarch64::canonicalRange(A, N);
}
llvm::Expected<std::unique_ptr<ExecutionBackend>>
CheckedAArch64Backend::create(std::unique_ptr<MemoryProjection> Memory,
                              std::unique_ptr<AArch64Machine> Machine,
                              bool UserMode) {
  auto B = std::unique_ptr<CheckedAArch64Backend>(
      new CheckedAArch64Backend(UserMode));
  B->Memory = std::move(Memory);
  B->Machine = std::move(Machine);
  B->CPU.UserMode = UserMode;
  if (auto E = B->Memory->validateMappings(aarch64::canonicalRange, !UserMode))
    return E;
  if (auto E = B->initializeDecoder(CS_ARCH_AARCH64, CS_MODE_ARM))
    return E;
  return std::unique_ptr<ExecutionBackend>(std::move(B));
}
namespace {
llvm::Expected<RegisterValue> readStateRegister(const AArch64MachineState &CPU,
                                                CPURegister R) {
  if (!registerMatches(R, GuestArchitecture::AArch64))
    return error(diagnostic::Register);
  if (R >= CPURegister::AArch64V0 && R <= CPURegister::AArch64V31)
    return CPU.Vectors[unsigned(R) - unsigned(CPURegister::AArch64V0)];
  return RegisterValue{
      CPU.Registers[unsigned(R) - unsigned(CPURegister::AArch64X0)], 0};
}
} // namespace
llvm::Expected<RegisterValue>
CheckedAArch64Backend::readRegister(CPURegister R) {
  return readStateRegister(CPU, R);
}
llvm::Expected<RegisterValue>
CheckedAArch64Backend::readContextRegister(const BackendContext &C,
                                           CPURegister R) const {
  const auto *State = contextStorage(C);
  if (!State || State->Owner.expired())
    return error(diagnostic::ContextExpired);
  if (State->Owner.lock() != Identity)
    return error(diagnostic::ContextOwner);
  if (State->Space.lock() != addressSpace())
    return error(diagnostic::ContextSpace);
  return readStateRegister(static_cast<const SavedState &>(*State).CPU, R);
}
llvm::Error CheckedAArch64Backend::writeRegister(CPURegister R,
                                                 const RegisterValue &V) {
  if (auto E = mutableMemory())
    return E;
  if (!registerMatches(R, architecture()) || !registerValueFits(R, V))
    return error(diagnostic::Register);
  if (R >= CPURegister::AArch64V0 && R <= CPURegister::AArch64V31) {
    CPU.Vectors[unsigned(R) - unsigned(CPURegister::AArch64V0)] = V;
    return llvm::Error::success();
  }
  if ((R == CPURegister::AArch64NZCV && (V[0] & ~aarch64::NZCVMask)) ||
      (R == CPURegister::AArch64FPCR && (V[0] & ~aarch64::AllowedFPCR)) ||
      (R == CPURegister::AArch64FPSR && (V[0] & ~aarch64::AllowedFPSR)) ||
      (R == CPURegister::AArch64PC &&
       (!aarch64::canonical(V[0]) || V[0] % aarch64::InstructionBytes)))
    return error(diagnostic::Register);
  CPU.Registers[unsigned(R) - unsigned(CPURegister::AArch64X0)] = V[0];
  return llvm::Error::success();
}
llvm::Expected<std::unique_ptr<BackendContext>>
CheckedAArch64Backend::saveContext() {
  if (auto E = checkExecutionState())
    return E;
  auto S = std::make_unique<SavedState>();
  S->Owner = Identity;
  S->Space = addressSpace();
  S->CPU = CPU;
  S->Exclusive = Exclusive;
  return makeContext(std::move(S));
}
llvm::Error CheckedAArch64Backend::saveContext(BackendContext &C) {
  if (!contextStorage(C) || contextStorage(C)->Owner.expired())
    return error(diagnostic::ContextExpired);
  if (contextStorage(C)->Owner.lock() != Identity)
    return error(diagnostic::ContextOwner);
  if (auto E = checkExecutionState())
    return E;
  contextStorage(C)->Space = addressSpace();
  static_cast<SavedState &>(*contextStorage(C)).CPU = CPU;
  static_cast<SavedState &>(*contextStorage(C)).Exclusive = Exclusive;
  return llvm::Error::success();
}
llvm::Error CheckedAArch64Backend::restoreContext(const BackendContext &C) {
  if (!contextStorage(C) || contextStorage(C)->Owner.expired())
    return error(diagnostic::ContextExpired);
  if (contextStorage(C)->Owner.lock() != Identity)
    return error(diagnostic::ContextOwner);
  if (auto E = mutableMemory())
    return E;
  if (contextStorage(C)->Space.lock() != addressSpace())
    return error(diagnostic::ContextSpace);
  CPU = static_cast<const SavedState &>(*contextStorage(C)).CPU;
  Exclusive = static_cast<const SavedState &>(*contextStorage(C)).Exclusive;
  TimedOut = false;
  return llvm::Error::success();
}
llvm::Error
CheckedAArch64Backend::bindAddressSpace(std::shared_ptr<AddressSpace> Space) {
  if (auto E = CheckedBackend::bindAddressSpace(std::move(Space)))
    return E;
  Exclusive.reset();
  return llvm::Error::success();
}
std::optional<ServiceRequest>
CheckedAArch64Backend::decodeServiceRequest(const cs_insn &I) const {
  if (I.size != aarch64::InstructionBytes)
    return std::nullopt;
  const uint32_t Word = llvm::support::endian::read32le(I.bytes);
#define NEVERD_AARCH64_SERVICE(Kind, Name, Mask, Value, Shift, ImmediateMask)  \
  if (I.id == AARCH64_INS_##Name && (Word & Mask) == Value)                    \
    return ServiceRequest{ServiceRequestKind::Kind, I.address,                 \
                          I.address + I.size,                                  \
                          uint16_t((Word >> Shift) & ImmediateMask)};
#include "AArch64ServiceInstructions.def"
#undef NEVERD_AARCH64_SERVICE
  return std::nullopt;
}

llvm::Error CheckedAArch64Backend::execute(const cs_insn &I) {
  auto Atomic = executeAtomic(I);
  if (!Atomic)
    return Atomic.takeError();
  if (*Atomic)
    return llvm::Error::success();
  auto Effects = getAArch64InstructionEffects(I, CPU);
  if (!Effects)
    return Effects.takeError();
  const auto &Accesses = *Effects;
  // Validate the entire instruction before any native access or pair write.
  for (const auto &M : Accesses) {
    // Ordinary device loads/stores and cache maintenance need their own
    // transaction admission. Only explicit LSE atomics are projected here.
    const auto First =
        Memory->mappings().lower_bound(M.Address & ~(memory::PageSize - 1));
    for (auto P = First;
         P != Memory->mappings().end() && P->first <= M.Address + M.Size - 1;
         ++P)
      if (P->second.IO)
        return llvm::make_error<UnsupportedExecutionError>();
    if (M.CacheMaintenance) {
      // Only readable ordinary RAM is admitted. Translation/fault behavior
      // for other cache targets differs between implementations, so reject
      // those targets as outside this contract rather than inventing a load
      // or a universal architectural fault. Mapped aliases use their own
      // rights.
      auto Allowed = Memory->addressSpace()->canAccess(
          M.Address, M.Size, executionPermissions(Read));
      if (!Allowed)
        return Allowed.takeError();
      if (!*Allowed)
        return llvm::make_error<UnsupportedExecutionError>();
      continue;
    }
    if (M.Permission == Read && Hooks.Read)
      Hooks.Read(M.Address, M.Size);
    if (M.Permission == Write && Hooks.Write) {
      const unsigned LowBytes = std::min<unsigned>(M.Size, aarch64::WordBytes);
      Hooks.Write(M.Address, LowBytes,
                  M.Value[0] &
                      llvm::maskTrailingOnes<uint64_t>(LowBytes * CHAR_BIT));
      if (!StopRequested && !FirstFault && M.Size > aarch64::WordBytes)
        Hooks.Write(M.Address + aarch64::WordBytes, M.Size - aarch64::WordBytes,
                    M.Value[1]);
    }
    if (StopRequested || FirstFault)
      return llvm::Error::success();
    if (auto E = access(M.Address, M.Size, M.Permission, true, true))
      return E;
    if (StopRequested)
      return llvm::Error::success();
  }
  if (auto E = buildAArch64PageTables(*this->Memory, UserMode))
    return E;
  std::vector<RAMWriteRange> Writes;
  std::vector<RAMWriteRange> Inputs{{I.address, I.size}};
  for (const auto &M : Accesses)
    if (M.Permission == Write)
      Writes.push_back({M.Address, M.Size});
  for (const auto &M : Accesses)
    Inputs.push_back({M.Address, M.Size});
  auto Transaction = RAMTransaction::create(
      *this->Memory, Writes, execution_limits::InstructionRAMWriteBytes,
      executionPermissions(Write));
  if (!Transaction)
    return Transaction.takeError();
  auto Next = CPU;
  if (auto E = (*Transaction)
                   ->execute(
                       [&] {
                         return Machine->step(Next, {Deadline, &StopRequested});
                       },
                       Inputs))
    return E;
  if (auto E = (*Transaction)->stage())
    return E;
  if (StopRequested || FirstFault)
    return llvm::Error::success();
  if (auto E = (*Transaction)->commit())
    return E;
  CPU = Next;
  return llvm::Error::success();
}
} // namespace neverd::emulation
