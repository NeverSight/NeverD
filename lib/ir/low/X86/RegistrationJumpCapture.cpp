//===- RegistrationJumpCapture.cpp - Source-bound SEH jump capture --------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Check the physical capture call, established frame and bounded buffer.

#include "RegistrationJumpCapture.h"

#include "RegistrationABIPrivate.h"
#include "RegistrationJumpRuntime.h"
#include "RegistrationSourceCall.h"

#include "neverd/ir/low/LowIR.h"
#include "neverd/ir/low/LowNoReturn.h"
#include "neverd/lift/X86/X86Regs.h"
#include "neverd/loader/RegistrationScopeGraph.h"

namespace neverd::registration_state {

std::optional<SEHJumpCapture>
prepareSEHJumpCapture(const BinaryImage &Image, const LowFunc &Function,
                      const LowOp &Op, const FrameState &Frame, bool Installed,
                      size_t &Work) {
  if (!Installed || !Function.ExceptionMetadata ||
      !Function.ExceptionMetadata->Registration ||
      Function.ExceptionMetadata->Cxx || Frame.CallbackEntry ||
      Op.Opcode != NdOp::CALL || Op.NumInputs != 1 || Op.Seq < 0 ||
      !Op.Inputs[0].isConst() || Op.Inputs[0].Size != 4 ||
      Op.MemoryOrdering != NdMemoryOrdering::None ||
      Op.MemoryAddressSpace != NdMemoryAddressSpace::Default ||
      !registration_abi::chargeCalleeWork(Work, 16))
    return std::nullopt;
  const auto &EH = *Function.ExceptionMetadata;
  const auto &Chain = *EH.Registration;
  const bool EH4 = EH.Personality == ExceptionPersonality::ExceptHandler4;
  const int32_t Outside = registrationOutsideTryLevel(EH4);
  const unsigned ArgumentBytes = EH4 ? 20 : 16;
  if (Function.Entry != EH.CodeRange.Begin || !EH.CodeRange.contains(Op.Addr) ||
      Op.Addr > UINT32_MAX - 5 || Op.Addr + 5 > EH.CodeRange.End ||
      EH.ParseStatus != ExceptionParseStatus::Complete ||
      EH.Encoding != (EH4 ? ExceptionEncoding::X86ScopeTableEH4
                          : ExceptionEncoding::X86ScopeTableEH3) ||
      (!EH4 && EH.Personality != ExceptionPersonality::ExceptHandler3) ||
      Chain.RealignedFrame || Chain.RegistrationOffset != -16 ||
      Chain.TryLevelOffset != -4 || Chain.SeededTryLevel != Outside ||
      Frame.Registers[x86reg::RBP / x86reg::GeneralRegStride].Offset != 0)
    return std::nullopt;
  const auto Runtime = checkedJumpRuntime(Image, Op.Inputs[0].Offset, false);
  if (!Runtime || Runtime->Routine != JumpRuntimeBinding::Kind::Capture)
    return std::nullopt;
  const auto *Source = checkedRegistrationDirectCall(Image, Function, Op, Work);
  if (!Source || isUnconditionalNoReturn(*Source))
    return std::nullopt;
  const auto SP =
      Frame.Registers[x86reg::RSP / x86reg::GeneralRegStride].Offset;
  if (!SP || *SP < INT32_MIN + 4 || int64_t(*SP) + ArgumentBytes > INT32_MAX)
    return std::nullopt;
  const auto Buffer = Frame.load(*SP, 4);
  const auto Level = Frame.load(*SP + 12, 4).Constant;
  const auto Unwind = Frame.load(*SP + 8, 4).preferredAddress();
  if (!Buffer.Offset || Buffer.ReturnPC || Buffer.CallbackAddress ||
      Buffer.EntryOffset || Buffer.SEHException ||
      Frame.load(*SP + 4, 4).Constant != (EH4 ? 3u : 2u) || !Level || !Unwind ||
      Frame.load(-4, 4).Constant != Level ||
      (!Frame.load(-16, 4).PreviousChain) ||
      Frame.load(-12, 4).preferredAddress() != EH.PersonalityVA ||
      (!EH4 && Frame.load(-8, 4).preferredAddress() != Chain.ScopeTableVA))
    return std::nullopt;
  const int64_t End = int64_t(*Buffer.Offset) + SEHJumpCapture::BufferBytes;
  if (*Buffer.Offset < int64_t(*SP) + ArgumentBytes || End > -24 ||
      (*Level != uint32_t(Outside) && *Level >= Chain.Scopes.size()))
    return std::nullopt;
  const auto UnwindRuntime = checkedJumpRuntime(Image, *Unwind, false);
  if (!UnwindRuntime ||
      UnwindRuntime->Routine != (EH4 ? JumpRuntimeBinding::Kind::EH4Unwind
                                     : JumpRuntimeBinding::Kind::EH3Unwind))
    return std::nullopt;
  SEHJumpCapture Capture;
  Capture.Call = Op.Addr;
  Capture.ReturnPC = Op.Addr + 5;
  Capture.UnwindTarget = *Unwind;
  Capture.BufferOffset = *Buffer.Offset;
  Capture.StackOffset = *SP;
  Capture.Level = int32_t(*Level);
  if (EH4) {
    const auto Cookie = checkedEH4JumpCookie(Image, Chain, Frame, *SP);
    if (!Cookie)
      return std::nullopt;
    Capture.Protocol = RegistrationSEHJumpCapture::Kind::EH4;
    Capture.SecurityCookieVA = *Cookie;
  }
  constexpr uint64_t Registers[] = {x86reg::RBP, x86reg::RBX, x86reg::RDI,
                                    x86reg::RSI};
  for (unsigned I = 0; I != std::size(Registers); ++I)
    Capture.Nonvolatile[I] =
        Frame.Registers[Registers[I] / x86reg::GeneralRegStride];
  return Capture;
}

} // namespace neverd::registration_state
