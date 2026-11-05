//===- X64PagedPortIO.cpp - Ordered port device transactions --------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Own TSS permission checks and element commit across the port/RAM boundary.
//===----------------------------------------------------------------------===//

#include "neverd/analysis/arch/x86_64/X64PagedPortIO.h"

#include "X64PortIOEncoding.h"

#include "llvm/Support/Errc.h"

#include <cstdint>
#include <optional>
#include <utility>

namespace neverd::analysis {

llvm::Expected<X64PagedPortIOResult> executeX64PagedPortIO(
    X64ContinuationStateV6 &State, translate::X86PagedMemory &Memory,
    llvm::ArrayRef<uint8_t> Bytes, X64PagedAlignmentPriority AlignmentPriority,
    const X64PagedPortIOBinding &Binding) {
  using namespace translate;
  const auto Form = detail::x64PortIOEncoding(Bytes);
  if (!Form)
    return llvm::createStringError(llvm::errc::invalid_argument,
                                   "invalid paged port I/O instruction");
  if (auto Error = validateX86PortIOBinding(Binding))
    return std::move(Error);
  const auto &F = *Form;
  auto &Core = State.Core.Core.Core.Core;
  const uint8_t CPL = State.Core.CS.Selector & 3;
  const uint16_t Port =
      F.ImmediatePort ? uint16_t(*F.ImmediatePort) : uint16_t(Core.GPR[2]);
  X64PagedPortIOResult Result;
  if (CPL > ((Core.RFlags >> 12) & 3)) {
    if ((State.Unusable & 128) || (State.TR.Attributes & 0x9f) != 0x8b)
      return llvm::createStringError(
          llvm::errc::invalid_argument,
          "port permissions require a usable 64-bit TSS cache");
    auto Checked = checkX86PortBitmap(Memory, State.TR.Base, State.TR.Limit,
                                      Port, F.Bytes, false);
    if (!Checked)
      return Checked.takeError();
    Result.Access = std::move(*Checked);
    if (Result.Access.Status != X86PageWalkStatus::Completed)
      return Result;
  }
  const auto AddressValue = [&](uint64_t V) {
    return F.AddressBytes == 4 ? uint64_t(uint32_t(V)) : V;
  };
  const uint64_t Count = F.Repeat ? AddressValue(Core.GPR[1]) : 1;
  if (!Count) {
    Core.GPR[1] = 0;
    Core.PC += Bytes.size();
    Result.Completed = true;
    return Result;
  }
  const uint32_t Mask = F.Bytes == 4 ? UINT32_MAX : (1u << (8 * F.Bytes)) - 1;
  std::optional<X86PortMemoryOperand> Operand;
  if (F.String) {
    uint64_t Linear = AddressValue(Core.GPR[F.Input ? 7 : 6]);
    if (!F.Input)
      Linear += F.Segment == 1 ? Core.FSBase : F.Segment == 2 ? Core.GSBase : 0;
    // Long-mode SS overrides do not give these implicit operands stack faults.
    Operand = X86PortMemoryOperand{
        {Linear, F.Input ? MemoryAccessKind::Write : MemoryAccessKind::Read,
         CPL}};
    auto Check =
        checkX86LinearAccess(Memory.controls(), Operand->Access, F.Bytes);
    if (!Check)
      return Check.takeError();
    Operand->Fault = *Check;
    const bool Unaligned = CPL == 3 && (Core.RFlags & 0x40000) &&
                           (Memory.controls().CR0 & 0x40000) &&
                           (Linear & (F.Bytes - 1));
    if (!Operand->Fault && Unaligned) {
      if (AlignmentPriority == X64PagedAlignmentPriority::BeforePaging)
        Operand->Fault = X86MemoryException{17, 0, {}};
      else
        Operand->AlignmentFaultAfterPaging = true;
    }
  }
  auto Transfer = transferX86PortIO(
      Memory, Binding, {Port, uint8_t(F.Bytes), F.Input, uint32_t(Core.GPR[0])},
      Operand);
  if (!Transfer)
    return Transfer.takeError();
  Transfer->Access.SelfModifications.insert(
      Transfer->Access.SelfModifications.begin(),
      Result.Access.SelfModifications.begin(),
      Result.Access.SelfModifications.end());
  Result = std::move(*Transfer);
  if (!Result.Completed)
    return Result;
  if (F.Input && !F.String)
    Core.GPR[0] = F.Bytes == 4
                      ? Result.InputValue
                      : (Core.GPR[0] & ~uint64_t(Mask)) | Result.InputValue;
  if (F.String) {
    auto &Index = Core.GPR[F.Input ? 7 : 6];
    Index = AddressValue(Index + ((Core.RFlags & 0x400) ? -uint64_t(F.Bytes)
                                                        : uint64_t(F.Bytes)));
  }
  if (F.Repeat) {
    Core.GPR[1] = Count - 1;
    Result.RepeatContinue = Count != 1;
  }
  if (!Result.RepeatContinue)
    Core.PC += Bytes.size();
  Result.Completed = !Result.RepeatContinue;
  return Result;
}

} // namespace neverd::analysis
