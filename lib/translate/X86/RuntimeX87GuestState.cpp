//===- RuntimeX87GuestState.cpp - Explicit translated x87 state -----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "neverd/translate/X86/RuntimeX87GuestState.h"

#include "llvm/ADT/APInt.h"
#include "llvm/Support/Endian.h"
#include "llvm/Support/Errc.h"

#include <algorithm>
#include <array>
#include <iterator>
#include <utility>

namespace neverd::translate {
namespace {

llvm::Error invalid(llvm::StringRef Message) {
  return llvm::createStringError(llvm::errc::invalid_argument, "%s",
                                 Message.str().c_str());
}

const GuestRegisterValue *findX87(const GuestState &State) {
  for (const GuestRegisterValue &Register : State.Registers)
    if (Register.ID >= kFirstExtensionRegisterID &&
        Register.ExtensionName == kPortableX87GuestExtensionV1)
      return &Register;
  return nullptr;
}

llvm::APInt encodeX87(const PortableX87StateV1 &State) {
  std::array<uint8_t, kPortableX87StateSizeV1> Bytes{};
  llvm::support::endian::write32le(Bytes.data(), State.Magic);
  llvm::support::endian::write16le(Bytes.data() + 4, State.Version);
  llvm::support::endian::write16le(Bytes.data() + 6, State.Size);
  Bytes[8] = State.EnvironmentKnown;
  Bytes[9] = State.KnownPayload;
  std::copy(std::begin(State.Reserved), std::end(State.Reserved),
            Bytes.begin() + 10);
  std::copy(State.Image.begin(), State.Image.end(),
            Bytes.begin() + offsetof(PortableX87StateV1, Image));
  llvm::APInt Value(kPortableX87GuestExtensionBitsV1, 0);
  for (unsigned Index = 0; Index != Bytes.size(); ++Index)
    Value.insertBits(uint64_t{Bytes[Index]}, Index * 8, 8);
  return Value;
}

llvm::Expected<PortableX87StateV1>
decodeX87(const GuestRegisterValue &Register) {
  if (Register.Value.getBitWidth() != kPortableX87GuestExtensionBitsV1)
    return invalid("portable x87 guest extension has the wrong width");
  std::array<uint8_t, kPortableX87StateSizeV1> Bytes{};
  for (unsigned Index = 0; Index != Bytes.size(); ++Index)
    Bytes[Index] = Register.Value.extractBitsAsZExtValue(8, Index * 8);
  PortableX87StateV1 State;
  State.Magic = llvm::support::endian::read32le(Bytes.data());
  State.Version = llvm::support::endian::read16le(Bytes.data() + 4);
  State.Size = llvm::support::endian::read16le(Bytes.data() + 6);
  State.EnvironmentKnown = Bytes[8];
  State.KnownPayload = Bytes[9];
  std::copy_n(Bytes.begin() + 10, std::size(State.Reserved),
              std::begin(State.Reserved));
  std::copy_n(Bytes.begin() + offsetof(PortableX87StateV1, Image),
              State.Image.size(), State.Image.begin());
  if (llvm::Error Error = validatePortableX87StateV1(State))
    return std::move(Error);
  return State;
}

bool knownStatus(PortableX87StepStatusV1 Status) {
  switch (Status) {
#define NEVERD_X87_PORTABLE_STATUS(CxxName, CName, Value, Diagnostic)          \
  case PortableX87StepStatusV1::CxxName:                                       \
    return true;
#include "neverd/ir/X86/X87PortableStatus.def"
#undef NEVERD_X87_PORTABLE_STATUS
  }
  return false;
}

} // namespace

llvm::Error validateRuntimeX87LastStepV1(const RuntimeX87LastStepV1 &Step) {
  if (Step.Attempted > 1 || Step.InstructionCompleted > 1 ||
      Step.HasScalarResult > 1 ||
      std::any_of(std::begin(Step.Reserved), std::end(Step.Reserved),
                  [](uint8_t Byte) { return Byte != 0; }) ||
      !knownStatus(Step.Status))
    return invalid("invalid translated x87 step diagnostic");
  if (!Step.Attempted) {
    if (Step.Selector != 0 ||
        Step.Status != PortableX87StepStatusV1::InvalidState || Step.PC != 0 ||
        Step.NextPC != 0 || Step.DataAddress != 0 || Step.ScalarResult != 0 ||
        Step.InstructionCompleted != 0 || Step.HasScalarResult != 0)
      return invalid("unused translated x87 diagnostic is not empty");
    return llvm::Error::success();
  }
  const bool Completed =
      Step.Status == PortableX87StepStatusV1::Completed ||
      Step.Status == PortableX87StepStatusV1::SelfModification;
  if (Step.InstructionCompleted != Completed || Step.NextPC <= Step.PC)
    return invalid("translated x87 completion or continuation is incoherent");
  if (!Step.HasScalarResult && Step.ScalarResult != 0)
    return invalid("translated x87 diagnostic carries an invalid result");
  if (!Completed && Step.HasScalarResult)
    return invalid("refused translated x87 instruction carries a result");
  if (Completed) {
    const auto Spec = x87ContextInstruction(Step.Selector);
    if (!Spec || Step.HasScalarResult != (Spec->WritesAX || Spec->WritesFlags))
      return invalid("translated x87 result disagrees with its instruction");
    if ((Spec->MemoryBytes == 0 && Step.DataAddress != 0) ||
        (Spec->WritesAX && Step.ScalarResult > UINT16_MAX) ||
        (Step.Status == PortableX87StepStatusV1::SelfModification &&
         !Spec->WritesMemory))
      return invalid("translated x87 operand or completion kind is incoherent");
  }
  return llvm::Error::success();
}

llvm::Error validateRuntimeGuestStateX86_64X87V1(
    const RuntimeGuestStateX86_64X87V1 &State) {
  if (llvm::Error Error = validateRuntimeGuestStateX86_64V1(State.Integer))
    return Error;
  if (llvm::Error Error = validatePortableX87StateV1(State.X87))
    return Error;
  return validateRuntimeX87LastStepV1(State.LastStep);
}

llvm::Error setPortableX87GuestStateV1(GuestState &State, RegisterID ID,
                                       const PortableX87StateV1 &X87) {
  if (llvm::Error Error = validateGuestState(State))
    return Error;
  if (llvm::Error Error = validatePortableX87StateV1(X87))
    return Error;
  if (State.Architecture != GuestArchitecture::X86_64 ||
      ID < kFirstExtensionRegisterID)
    return invalid("portable x87 state requires an x86-64 extension register");
  if (const auto *Existing = findRegisterValue(State, ID);
      Existing && Existing->ExtensionName != kPortableX87GuestExtensionV1)
    return invalid("portable x87 extension would replace unrelated state");
  return setRegisterValue(State, ID, encodeX87(X87),
                          kPortableX87GuestExtensionV1);
}

llvm::Expected<RuntimeGuestStateX86_64X87V1>
createRuntimeGuestStateX86_64X87V1(const GuestState &State) {
  auto Integer = createRuntimeGuestStateX86_64V1(State);
  if (!Integer)
    return Integer.takeError();
  const auto *Extension = findX87(State);
  if (!Extension)
    return invalid("portable x87 guest state is not explicitly provisioned");
  auto X87 = decodeX87(*Extension);
  if (!X87)
    return X87.takeError();
  return RuntimeGuestStateX86_64X87V1{*Integer, *X87, {}};
}

llvm::Error applyRuntimeGuestStateX86_64X87V1(
    const RuntimeGuestStateX86_64X87V1 &RuntimeState, GuestState &State) {
  if (llvm::Error Error = validateRuntimeGuestStateX86_64X87V1(RuntimeState))
    return Error;
  auto Existing = createRuntimeGuestStateX86_64X87V1(State);
  if (!Existing)
    return Existing.takeError();
  const RegisterID ID = findX87(State)->ID;
  GuestState Candidate = State;
  if (llvm::Error Error =
          applyRuntimeGuestStateX86_64V1(RuntimeState.Integer, Candidate))
    return Error;
  if (llvm::Error Error =
          setPortableX87GuestStateV1(Candidate, ID, RuntimeState.X87))
    return Error;
  State = std::move(Candidate);
  return llvm::Error::success();
}

} // namespace neverd::translate
