//===- X64MachineProbe.cpp - Native startup validation ------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "X64MachineProbe.h"

#include "../../core/ExecutionDiagnostics.h"
#include "../../core/MemoryProjection.h"

#include "llvm/ADT/ScopeExit.h"
#include "llvm/Support/Endian.h"
#include "llvm/Support/FormatVariadic.h"

#include <cstring>

namespace neverd::emulation {
namespace {
using namespace x64;
static_assert(probe::CodeOffset >= gateway::VectorCount * gateway::CodeStride);
static_assert(probe::CodeOffset + probe::programBytes() <= PageSize);
static_assert(probe::TLSOffset >= gateway::TSSOffset + gateway::TSSBytes);
static_assert(probe::TLSOffset >=
              gateway::GDTOffset + gateway::GDTEntries * WordBytes);
static_assert(probe::TLSOffset + 2 * WordBytes <= PageSize);
static_assert(probe::FloatingDestination < XmmCount &&
              probe::FloatingSource < XmmCount &&
              probe::VectorDestination < XmmCount &&
              probe::VectorSource < XmmCount);

llvm::Error annotateInterruption(llvm::Error Error, llvm::StringRef Step) {
  return llvm::handleErrors(
      std::move(Error), [&](const MachineInterruptedError &Interrupted) {
        std::string Detail;
        llvm::raw_string_ostream OS(Detail);
        Interrupted.log(OS);
        return llvm::make_error<MachineInterruptedError>(
            llvm::formatv(probe::Interrupted, Step, Interrupted.stopRequested(),
                          Interrupted.deadlineReached(), OS.str())
                .str(),
            Interrupted.stopRequested(), Interrupted.deadlineReached());
      });
}

llvm::Error stateMismatch(llvm::StringRef Instruction,
                          const X64MachineState &Expected,
                          const X64MachineState &Observed) {
  std::string Details;
  auto Field = [&](llvm::StringRef Name, uint64_t Want, uint64_t Got) {
    if (Want == Got)
      return;
    if (!Details.empty())
      Details += probe::FieldSeparator;
    Details += llvm::formatv(probe::FieldMismatch, Name, Want, Got).str();
  };
#define NEVERD_X64_REGISTER(Name, Decoder, Backend)                            \
  Field(#Name, Expected.reg(X64Register::Name),                                \
        Observed.reg(X64Register::Name));
#include "neverd/emulation/arch/x86_64/X64Registers.def"
#undef NEVERD_X64_REGISTER
#define NEVERD_X64_PROBE_FIELD(Member)                                         \
  Field(#Member, Expected.Member, Observed.Member);
#include "X64MachineProbe.def"
#undef NEVERD_X64_PROBE_FIELD
#define NEVERD_X64_FP_CONTROL(Name, Member, Type, UC, Offset)                  \
  Field(#Name, Expected.FP.Member, Observed.FP.Member);
#include "X64FPState.def"
#undef NEVERD_X64_FP_CONTROL
  auto Lanes = [&](llvm::StringRef Name, const auto &Want, const auto &Got) {
    for (unsigned Index = 0; Index < Want.size(); ++Index)
      for (unsigned Word = 0; Word < Want[Index].size(); ++Word)
        Field(llvm::formatv(probe::IndexedWord, Name, Index, Word).str(),
              Want[Index][Word], Got[Index][Word]);
  };
  Lanes(probe::XmmName, Expected.Xmm, Observed.Xmm);
  Lanes(probe::FPName, Expected.FP.Registers, Observed.FP.Registers);
  return llvm::createStringError(
      llvm::inconvertibleErrorCode(),
      llvm::formatv(probe::StateMismatch, probe::State, Instruction, Details)
          .str());
}
} // namespace

llvm::Error verifyX64Machine(X64Machine &Machine, MemoryProjection &Memory,
                             uint32_t *MXCSRMask, bool SIMDExceptions,
                             X64BranchModel *BranchModel) {
  if (auto E = Memory.beginRun())
    return E;
  auto Release = llvm::scope_exit([&] { Memory.endRun(); });
  // WHP does not need the exception gateway during ordinary execution, but
  // both native probes use the same private, supervisor-only storage.
  auto Root = buildX64PageTables(Memory, false, true);
  if (!Root)
    return Root.takeError();
  const auto Base = x64ExceptionMonitorBase(Memory);
  auto *Code = Memory.data() + x64::gateway::CodeGPA + x64::probe::CodeOffset;
  for (const auto &Instruction : x64::probe::Program) {
    std::memcpy(Code, Instruction.Code.data(), Instruction.Code.size());
    Code += Instruction.Code.size();
  }
  auto *TLS = Memory.data() + x64::gateway::DataGPA + x64::probe::TLSOffset;
  llvm::support::endian::write64le(TLS, x64::probe::FSValue);
  llvm::support::endian::write64le(TLS + x64::WordBytes, x64::probe::GSValue);
  X64MachineState State;
  for (unsigned Index = 0; Index < State.Registers.size(); ++Index)
    State.Registers[Index] =
        x64::probe::ScalarSeed + Index * x64::probe::ScalarStride;
  for (unsigned Index = 0; Index < State.Xmm.size(); ++Index)
    State.Xmm[Index] = {
        x64::probe::VectorLowSeed + Index * x64::probe::VectorStride,
        x64::probe::VectorHighSeed - Index * x64::probe::VectorStride};
  State.reg(X64Register::PC) = Base + x64::PageSize + x64::probe::CodeOffset;
  State.reg(X64Register::SP) =
      Base + x64::gateway::Bytes - x64::probe::StackAlignment;
  State.reg(X64Register::FLAGS) = x64::InitialFlags;
  State.reg(X64Register::CR8) = x64::probe::CR8;
  State.reg(X64Register::CS) = x64::CodeSelector;
  State.reg(X64Register::SS) = x64::DataSelector;
  State.FSBase = Base + x64::probe::TLSOffset;
  State.GSBase = State.FSBase + x64::WordBytes;
  State.MXCSR = x64::probe::MXCSR;
  State.FP.Control = x64::probe::FPControl;
  State.FP.Status = x64::probe::FPStatus;
  State.FP.Tag = x64::probe::FPTag;
  State.FP.Opcode = x64::probe::FPOpcode;
  State.FP.Instruction = State.reg(X64Register::PC);
  State.FP.Data = State.FSBase;
  for (unsigned Index = 0; Index < State.FP.Registers.size(); ++Index)
    State.FP.Registers[Index] = {x64::probe::FP80Base + Index,
                                 x64::probe::FP80High};
  State.Xmm[x64::probe::FloatingDestination][0] =
      (State.Xmm[x64::probe::FloatingDestination][0] & ~x64::probe::FloatMask) |
      x64::probe::FloatOne;
  State.Xmm[x64::probe::FloatingSource][0] =
      (State.Xmm[x64::probe::FloatingSource][0] & ~x64::probe::FloatMask) |
      x64::probe::FloatHalfULP;
  const MachineRunControl Control{
      std::chrono::steady_clock::now() +
      std::chrono::microseconds(x64::probe::TimeoutMicroseconds)};
  auto CheckDeadline = [&](llvm::StringRef Step) -> llvm::Error {
    if (!Control.interrupted())
      return llvm::Error::success();
    return annotateInterruption(diagnostic::interrupted(probe::State, Control),
                                Step);
  };
  auto Step = [&](llvm::StringRef Name) {
    return annotateInterruption(Machine.step(State, *Root, Control), Name);
  };
  for (const auto &Instruction : x64::probe::Program) {
    if (auto E = CheckDeadline(Instruction.Name))
      return E;
    auto Expected = State;
    Expected.reg(X64Register::PC) += Instruction.Code.size();
    switch (Instruction.Kind) {
    case x64::probe::Step::Nop:
      break;
    case x64::probe::Step::FloatingAdd:
      Expected.Xmm[x64::probe::FloatingDestination][0] =
          (Expected.Xmm[x64::probe::FloatingDestination][0] &
           ~x64::probe::FloatMask) |
          x64::probe::FloatRoundedUp;
      Expected.MXCSR = x64::probe::InexactMXCSR;
      break;
    case x64::probe::Step::VectorAdd:
      for (unsigned Word = 0; Word < RegisterValue{}.size(); ++Word)
        Expected.Xmm[x64::probe::VectorDestination][Word] +=
            Expected.Xmm[x64::probe::VectorSource][Word];
      break;
    case x64::probe::Step::ReadFS:
      Expected.reg(X64Register::R14) = x64::probe::FSValue;
      break;
    case x64::probe::Step::ReadGS:
      Expected.reg(X64Register::R15) = x64::probe::GSValue;
      break;
    case x64::probe::Step::ReadCS:
      Expected.reg(X64Register::R12) =
          (Expected.reg(X64Register::R12) & ~x64::probe::SelectorMask) |
          x64::CodeSelector;
      break;
    case x64::probe::Step::ReadSS:
      Expected.reg(X64Register::R13) =
          (Expected.reg(X64Register::R13) & ~x64::probe::SelectorMask) |
          x64::DataSelector;
      break;
    case x64::probe::Step::ReadCR8:
      Expected.reg(X64Register::R11) = x64::probe::CR8;
      break;
    }
    if (auto E = Step(Instruction.Name))
      return E;
    if (State != Expected)
      return stateMismatch(Instruction.Name, Expected, State);
    if (auto E = CheckDeadline(Instruction.Name))
      return E;
  }
  std::optional<X64BranchModel> ObservedBranchModel;
  if (BranchModel) {
    auto Probe = [&](llvm::StringRef Name, uint64_t Flags, bool CanNarrow,
                     llvm::ArrayRef<uint8_t> Bytes) -> llvm::Error {
      if (auto E = CheckDeadline(Name))
        return E;
      auto *Code = Memory.data() + gateway::CodeGPA + probe::CodeOffset;
      std::memcpy(Code, Bytes.data(), Bytes.size());
      State.reg(X64Register::PC) = Base + PageSize + probe::CodeOffset;
      State.reg(X64Register::FLAGS) = InitialFlags | Flags;
      auto Expected = State;
      Expected.reg(X64Register::PC) += Bytes.size();
      if (auto E = Step(Name))
        return E;
      if (!ObservedBranchModel) {
        if (State.reg(X64Register::PC) == Expected.reg(X64Register::PC))
          ObservedBranchModel = X64BranchModel::Intel;
        else if (State.reg(X64Register::PC) ==
                 Expected.reg(X64Register::PC) - probe::BranchWidthDifference)
          ObservedBranchModel = X64BranchModel::AMD;
        else
          return diagnostic::error(probe::BranchModelFailure);
      }
      if (CanNarrow && *ObservedBranchModel == X64BranchModel::AMD)
        Expected.reg(X64Register::PC) -= probe::BranchWidthDifference;
      if (State != Expected)
        return stateMismatch(Name, Expected, State);
      return CheckDeadline(Name);
    };
#define NEVERD_X64_PROBE_BRANCH(Name, Flags, CanNarrow, ...)                   \
  {                                                                            \
    constexpr uint8_t Bytes[] = {__VA_ARGS__};                                 \
    if (auto E = Probe(#Name, Flags, CanNarrow, Bytes))                        \
      return E;                                                                \
  }
#include "X64MachineProbe.def"
#undef NEVERD_X64_PROBE_BRANCH
  }
  if (SIMDExceptions) {
    auto *Code = Memory.data() + gateway::CodeGPA + probe::CodeOffset;
    std::memcpy(Code, probe::FloatingAdd, sizeof(probe::FloatingAdd));
    State.reg(X64Register::PC) = Base + PageSize + probe::CodeOffset;
    State.MXCSR = probe::UnmaskedMXCSR;
    State.Xmm[probe::FloatingDestination][0] =
        (State.Xmm[probe::FloatingDestination][0] & ~probe::FloatMask) |
        probe::FloatOne;
    State.Xmm[probe::FloatingSource][0] =
        (State.Xmm[probe::FloatingSource][0] & ~probe::FloatMask) |
        probe::FloatHalfULP;
    auto Expected = State;
    bool Trapped = false;
    if (auto E = CheckDeadline(probe::SIMDName))
      return E;
    auto E = Step(probe::SIMDName);
    if (auto Remaining = llvm::handleErrors(
            std::move(E), [&](const X64ExceptionError &Fault) {
              Trapped =
                  Fault.exception().Vector == unsigned(ExceptionVector::SIMD) &&
                  !Fault.exception().ErrorCode &&
                  !Fault.exception().FaultAddress;
            }))
      return Remaining;
    if (!Trapped)
      return diagnostic::error(probe::SIMDFault);
    if (State != Expected)
      return stateMismatch(probe::SIMDName, Expected, State);
    // The masked tie rounds to the unchanged 1.0 destination. The old sticky
    // status must also survive an exact retry with precision still unmasked.
    for (bool Repair : {false, true}) {
      State = Expected;
      if (Repair)
        State.Xmm[probe::FloatingSource][0] &= ~probe::FloatMask;
      else
        State.MXCSR |= InitialMXCSR;
      auto Retried = State;
      Retried.reg(X64Register::PC) += sizeof(probe::FloatingAdd);
      if (auto E = CheckDeadline(probe::SIMDName))
        return E;
      if (auto E = Step(probe::SIMDName))
        return E;
      if (State != Retried)
        return stateMismatch(probe::SIMDName, Retried, State);
    }
    if (auto E = CheckDeadline(probe::SIMDName))
      return E;
  }
  if (!MXCSRMask) {
    if (BranchModel)
      *BranchModel = *ObservedBranchModel;
    return llvm::Error::success();
  }
  static_assert(probe::FXOffset % probe::StackAlignment == 0);
  static_assert(probe::FXOffset + fp::LegacyBytes <= PageSize);
  auto *Saved = Memory.data() + gateway::DataGPA + probe::FXOffset;
  std::fill_n(Saved, fp::LegacyBytes, 0);
  State.reg(X64Register::R11) = Base + probe::FXOffset;
  auto Run = [&](llvm::StringRef Name,
                 llvm::ArrayRef<uint8_t> Bytes) -> llvm::Error {
    if (auto E = CheckDeadline(Name))
      return E;
    auto *Code = Memory.data() + gateway::CodeGPA + probe::CodeOffset;
    std::memcpy(Code, Bytes.data(), Bytes.size());
    State.reg(X64Register::PC) = Base + PageSize + probe::CodeOffset;
    auto Expected = State;
    Expected.reg(X64Register::PC) += Bytes.size();
    if (auto E = Step(Name))
      return E;
    if (State != Expected)
      return stateMismatch(Name, Expected, State);
    if (auto E = CheckDeadline(Name))
      return E;
    return llvm::Error::success();
  };
  constexpr uint8_t Save[] = {
#define NEVERD_X64_PROBE_FXSAVE(...) __VA_ARGS__
#include "X64MachineProbe.def"
#undef NEVERD_X64_PROBE_FXSAVE
  };
  if (auto E = Run(probe::SaveName, Save))
    return E;
  const uint32_t Raw =
      llvm::support::endian::read32le(Saved + fp::MXCSRMaskOffset);
  const uint32_t Mask =
      Raw ? Raw & fp::ArchitecturalMXCSRMask : fp::BaselineMXCSRMask;
  if ((Mask & fp::BaselineMXCSRMask) != fp::BaselineMXCSRMask)
    return llvm::createStringError(
        llvm::inconvertibleErrorCode(),
        llvm::formatv(probe::MXCSRMaskFailure, Raw).str());
  if (Mask & fp::DenormalsAreZero) {
    State.MXCSR = probe::DAZMXCSR;
    State.Xmm[probe::FloatingDestination][0] =
        (State.Xmm[probe::FloatingDestination][0] & ~probe::FloatMask) |
        probe::NegativeZero;
    State.Xmm[probe::FloatingSource][0] =
        (State.Xmm[probe::FloatingSource][0] & ~probe::FloatMask) |
        probe::NegativeSubnormal;
    // -0 + DAZ(-subnormal) = -0, with no new denormal/precision status.
    // The source, all other lanes and every control field must survive.
    if (auto E = Run(probe::DAZName, probe::FloatingAdd))
      return E;
  }
  *MXCSRMask = Mask;
  if (BranchModel)
    *BranchModel = *ObservedBranchModel;
  return llvm::Error::success();
}
} // namespace neverd::emulation
