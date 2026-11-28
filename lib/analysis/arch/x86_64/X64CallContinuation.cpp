//===- X64CallContinuation.cpp - Conditional raw call semantics ---------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file Architecture-owned conditional normal-return policy and integrity.

#include "X64CallContinuation.h"

#include "../../core/RecoveryDigest.h"
#include "NativeStackControl.h"

#include "neverd/analysis/Limits.h"
#include "neverd/ir/TargetRegInfo.h"
#include "neverd/ir/low/LowRawCall.h"
#include "neverd/lift/X86/X86Regs.h"

#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/Endian.h"

#include <algorithm>
#include <iterator>
#include <map>
#include <tuple>
#include <vector>

namespace neverd::analysis::x64 {
namespace {
constexpr uint64_t kHomeBytes = 32;
constexpr uint16_t kAddressBytes = 8;
constexpr uint64_t kArithmeticFlags[] = {x86reg::CF, x86reg::PF, x86reg::AF,
                                         x86reg::ZF, x86reg::SF, x86reg::OF};

llvm::Error invalid(const char *Message) {
  return llvm::createStringError(llvm::errc::invalid_argument, "%s", Message);
}

} // namespace

void appendReturningCallDeclaration(RecoveryDigest &H,
                                    const SpecializationX64ReturningCall &D) {
  H.number(D.Version);
  H.number(D.Address);
  H.number(static_cast<unsigned>(D.Profile));
  H.number(D.OutgoingBytes);
  // Keep valid V1 digests unchanged. Malformed V1 requests still encode their
  // extra ranges, although admission rejects them independently.
  if (D.Version != 1 || !D.PreservedFrameRanges.empty()) {
    H.number(D.PreservedFrameRanges.size());
    for (const auto &Range : D.PreservedFrameRanges) {
      H.number(static_cast<uint64_t>(Range.Begin));
      H.number(static_cast<uint64_t>(Range.End));
    }
  }
  if (D.Version >= 4 || !D.ResultObjectExclusions.empty()) {
    H.number(D.ResultObjectExclusions.size());
    for (const auto &Range : D.ResultObjectExclusions) {
      H.number(Range.Begin);
      H.number(Range.End);
    }
  }
  if (D.Version >= 5 || !D.FrameOutputs.empty()) {
    H.number(D.FrameOutputs.size());
    for (const auto &Output : D.FrameOutputs) {
      H.number(Output.PointerRegister);
      H.number(Output.Bytes);
    }
  }
}

llvm::Error validateReturningCallDeclarations(const SpecializationOptions &O) {
  if (O.X64ReturningCalls.empty())
    return llvm::Error::success();
  if (O.X64MachineExits.empty() || O.RequireRestoredFrameAtReturn ||
      O.ExternalStoresPreserveEntryReturnSlot ||
      !O.X64TerminalImports.empty() || !O.EntryFrameBounds ||
      O.EntryFrameBounds->Begin >= O.EntryFrameBounds->End)
    return invalid(
        "returning calls require an explicit machine region and "
        "nonempty entry-frame bounds without ordinary return contracts");
  for (size_t I = 0; I < O.X64ReturningCalls.size(); ++I) {
    const auto &D = O.X64ReturningCalls[I];
    const bool V1 =
        D.Version == 1 &&
        D.Profile == SpecializationX64CallProfile::ConditionalNormalReturnV1 &&
        D.PreservedFrameRanges.empty() && D.ResultObjectExclusions.empty() &&
        D.FrameOutputs.empty();
    const bool V2 =
        D.Version == 2 &&
        D.Profile ==
            SpecializationX64CallProfile::ConditionalNormalReturnFrameSpansV2 &&
        !D.PreservedFrameRanges.empty() &&
        D.PreservedFrameRanges.size() <=
            limits::analysis::kX64ReturningCallPreservedFrameRangesLimit &&
        D.ResultObjectExclusions.empty() && D.FrameOutputs.empty();
    const bool V3 =
        D.Version == 3 &&
        D.Profile == SpecializationX64CallProfile::
                         ConditionalNormalReturnResultObjectV3 &&
        !D.PreservedFrameRanges.empty() &&
        D.PreservedFrameRanges.size() <=
            limits::analysis::kX64ReturningCallPreservedFrameRangesLimit &&
        D.ResultObjectExclusions.empty() && D.FrameOutputs.empty();
    const bool V4 =
        D.Version == 4 &&
        D.Profile == SpecializationX64CallProfile::
                         ConditionalNormalReturnResultObjectExcludedV4 &&
        D.PreservedFrameRanges.size() <=
            limits::analysis::kX64ReturningCallPreservedFrameRangesLimit &&
        !D.ResultObjectExclusions.empty() &&
        D.ResultObjectExclusions.size() <=
            limits::analysis::kX64ReturningCallObjectExclusionsLimit &&
        D.FrameOutputs.empty();
    const bool V5 =
        D.Version == 5 &&
        D.Profile == SpecializationX64CallProfile::
                         ConditionalNormalReturnFrameOutputsV5 &&
        D.PreservedFrameRanges.size() <=
            limits::analysis::kX64ReturningCallPreservedFrameRangesLimit &&
        D.ResultObjectExclusions.empty() && !D.FrameOutputs.empty() &&
        D.FrameOutputs.size() <=
            limits::analysis::kX64ReturningCallFrameOutputsLimit;
    if ((!V1 && !V2 && !V3 && !V4 && !V5) || D.Address == InvalidVA ||
        D.OutgoingBytes < kHomeBytes || D.OutgoingBytes > INT64_MAX ||
        D.OutgoingBytes % kAddressBytes)
      return invalid("invalid explicit returning-call relation");
    int64_t PreviousEnd = O.EntryFrameBounds->Begin;
    for (const auto &Range : D.PreservedFrameRanges) {
      if (Range.Begin < PreviousEnd || Range.Begin >= Range.End ||
          Range.End > O.EntryFrameBounds->End)
        return invalid("returning-call preserved frame ranges must be "
                       "ordered, disjoint and contained in the entry frame");
      PreviousEnd = Range.End;
    }
    uint64_t PreviousExclusionEnd = 0;
    for (const auto &Range : D.ResultObjectExclusions) {
      if (Range.Begin < PreviousExclusionEnd || Range.Begin >= Range.End)
        return invalid("returning-call result-object exclusions must be "
                       "ordered, disjoint and nonempty");
      PreviousExclusionEnd = Range.End;
    }
    uint64_t PreviousPointer = 0;
    for (const auto &Output : D.FrameOutputs) {
      if (Output.PointerRegister <= PreviousPointer ||
          (Output.PointerRegister != x86reg::RCX &&
           Output.PointerRegister != x86reg::RDX &&
           Output.PointerRegister != x86reg::R8 &&
           Output.PointerRegister != x86reg::R9) ||
          !Output.Bytes ||
          Output.Bytes >
              limits::analysis::kX64ReturningCallFrameOutputBytesLimit)
        return invalid("returning-call frame outputs require ordered x64 "
                       "argument pointers and bounded byte extents");
      PreviousPointer = Output.PointerRegister;
    }
    for (size_t J = 0; J < I; ++J)
      if (D.Address == O.X64ReturningCalls[J].Address)
        return invalid("duplicate returning-call declaration");
    if (llvm::any_of(O.X64MachineExits,
                     [&](const auto &E) { return E.Address == D.Address; }))
      return invalid("returning call conflicts with a terminal machine exit");
  }
  return llvm::Error::success();
}

std::array<uint64_t, 4>
returningCallDeclarationWords(const SpecializationX64ReturningCall &D) {
  RecoveryDigest H(
      D.Version == 1   ? "neverd-x64-conditional-normal-return-v1"
      : D.Version == 2 ? "neverd-x64-conditional-normal-return-frame-v2"
      : D.Version == 3 ? "neverd-x64-conditional-normal-return-object-v3"
      : D.Version == 4 ? "neverd-x64-conditional-normal-return-object-v4"
                       : "neverd-x64-conditional-normal-return-outputs-v5");
  appendReturningCallDeclaration(H, D);
  const auto Bytes = llvm::fromHex(H.finish());
  std::array<uint64_t, 4> Words;
  for (size_t I = 0; I < Words.size(); ++I)
    Words[I] =
        llvm::support::endian::read64le(Bytes.data() + I * kAddressBytes);
  return Words;
}

RecoveryCallTransition
returningCallTransition(const SpecializationX64ReturningCall &D) {
  // This relation explicitly adopts these bytes, not an inferred callee ABI.
  // Preserve the current post-push RSP here; the consumer restores caller RSP.
  static const std::vector<symbolic::SymRegisterRange> Preserved = [] {
    std::vector<symbolic::SymRegisterRange> Ranges;
    for (const auto &R :
         getTargetRegInfo(Arch::X64).callPreservedRanges(BinaryFormat::COFF))
      Ranges.push_back({R.Offset, R.Bytes});
    Ranges.push_back({x86reg::DF, 1});
    Ranges.push_back({x86reg::FPU_CW, 2});
    std::sort(Ranges.begin(), Ranges.end(),
              [](const auto &A, const auto &B) { return A.Offset < B.Offset; });
    return Ranges;
  }();
  RecoveryCallTransition Policy{Preserved,
                                kArithmeticFlags,
                                NdVar::reg(x86reg::RSP, kAddressBytes),
                                NdVar::reg(x86reg::RIP, kAddressBytes),
                                {}};
  Policy.PreservedFrameRanges.reserve(D.PreservedFrameRanges.size());
  for (const auto &Range : D.PreservedFrameRanges)
    Policy.PreservedFrameRanges.push_back({Range.Begin, Range.End});
  if (D.Version == 3 || D.Version == 4)
    Policy.ReturnedObject = RecoveryCallTransition::ResultObject{
        NdVar::reg(x86reg::RAX, kAddressBytes),
        NdVar::reg(x86reg::RDX, kAddressBytes), D.ResultObjectExclusions};
  for (const auto &Output : D.FrameOutputs)
    Policy.FrameOutputs.push_back(
        {NdVar::reg(Output.PointerRegister, kAddressBytes), Output.Bytes});
  return Policy;
}

std::string returningCallDigest(const SpecializationReturningCall &Call) {
  RecoveryDigest H("neverd-returning-call-span-v1");
  H.number(Call.Version);
  H.number(Call.ResidualAddress);
  appendReturningCallDeclaration(H, Call.Declaration);
  H.variable(Call.ExpansionScratch);
  H.text(Call.OriginalOperationDigest);
  H.text(Call.ExpandedOperationDigest);
  H.text(Call.ResidualOperationDigest);
  const auto &I = Call.Original;
  H.boundary(I.Origin);
  H.number(I.Fallthrough.Address);
  H.number(static_cast<unsigned>(I.Fallthrough.Mode));
  H.number(static_cast<unsigned>(I.NativeStackControl));
  H.number(I.IsNativeCall);
  H.number(static_cast<unsigned>(I.ProfileProjection));
  H.bytes(I.NativeBytes);
  H.number(I.Ops.size());
  for (const auto &Op : I.Ops)
    H.operation(Op, std::size(Op.Inputs));
  H.text(lowPreservedStateDigest(I.PreservedState));
  H.number(static_cast<unsigned>(I.UndefinedEffects.Coverage));
  H.number(I.UndefinedEffects.OpCount);
  H.text(I.UndefinedEffects.OperationDigest);
  H.text(I.UndefinedEffects.Diagnostic);
  H.number(I.UndefinedEffects.Effects.size());
  for (const auto &E : I.UndefinedEffects.Effects) {
    H.number(E.AfterOp);
    H.variable(E.Output);
    H.number(E.BitOffset);
    H.number(E.BitCount);
    H.number(E.When.has_value());
    if (E.When)
      H.variable(*E.When);
  }
  // Terminal-import evidence is forbidden before this digest is consumed.
  H.number(I.TerminalImport.has_value());
  return H.finish();
}

llvm::Error validateReturningCalls(const SpecializationResult &Result,
                                   const SpecializationOptions &Options) {
  // Called only after the common result owner reserves all validation walks.
  if (Options.X64ReturningCalls.empty() && Result.ReturningCalls.empty()) {
    for (const auto &B : Result.Residual.Blocks)
      if (llvm::any_of(B.Ops, isLowRawReturningCall))
        return invalid(
            "raw returning call has no declared relation or receipt");
    return llvm::Error::success();
  }
  std::map<va_t, const LowInstructionBoundary *> Origins;
  for (const auto &O : Result.Origins)
    if (!Origins.emplace(O.ResidualAddress, &O.NativeInstruction).second)
      return invalid("duplicate returning-call native origin");
  std::map<va_t, const SpecializationReturningCall *> Calls;
  for (const auto &C : Result.ReturningCalls)
    if (C.Version != 1 || C.Original.TerminalImport ||
        C.IdentityDigest != returningCallDigest(C) ||
        C.Declaration.Address != C.Original.Origin.Address ||
        !Calls.emplace(C.ResidualAddress, &C).second ||
        std::count(Options.X64ReturningCalls.begin(),
                   Options.X64ReturningCalls.end(), C.Declaration) != 1)
      return invalid("invalid or duplicate returning-call receipt");
  const auto SameBoundary = [](const auto &A, const auto &B) {
    RecoveryDigest AH("boundary"), BH("boundary");
    AH.boundary(A);
    BH.boundary(B);
    return AH.finish() == BH.finish();
  };
  for (const auto &B : Result.Residual.Blocks)
    for (size_t J = 0; J < B.InstructionBoundaries.size(); ++J) {
      const auto &Boundary = B.InstructionBoundaries[J];
      if (Boundary.FirstOp > B.Ops.size() ||
          Boundary.OpCount > B.Ops.size() - Boundary.FirstOp)
        return invalid("returning call has an invalid residual span");
      const auto Span = llvm::ArrayRef<LowOp>(B.Ops).slice(Boundary.FirstOp,
                                                           Boundary.OpCount);
      const size_t Events = llvm::count_if(Span, isLowRawReturningCall);
      const auto At = Calls.find(Boundary.Address);
      if (!Events && At == Calls.end())
        continue;
      if (Events != 1 || At == Calls.end() || Span.empty() ||
          !lowRawReturningCallShapeIsValid(Span.back()) ||
          Boundary.Control != LowInstructionControl::None ||
          Boundary.ControlFlags != LowInstructionControlFlag::None ||
          Boundary.Immediate || J + 1 == B.InstructionBoundaries.size())
        return invalid("raw call lacks its unique complete returning span");
      const auto &Call = *At->second;
      const auto Origin = Origins.find(Boundary.Address);
      const auto Next = Origins.find(B.InstructionBoundaries[J + 1].Address);
      if (Origin == Origins.end() || Next == Origins.end() ||
          !SameBoundary(*Origin->second, Call.Original.Origin) ||
          Next->second->Address != Call.Original.Fallthrough.Address ||
          Next->second->Mode != Call.Original.Fallthrough.Mode ||
          lowUndefinedOperationDigest(Span) != Call.ResidualOperationDigest)
        return invalid("returning call differs from its native continuation");
      auto Expanded = expandX64ReturningCall(
          Call.Original, returningCallDeclarationWords(Call.Declaration),
          Call.ExpansionScratch);
      if (!Expanded)
        return Expanded.takeError();
      if (Expanded->Receipt.OriginalOperationDigest !=
              Call.OriginalOperationDigest ||
          Expanded->Receipt.ExpandedOperationDigest !=
              Call.ExpandedOperationDigest ||
          Expanded->Ops.size() != Span.size())
        return invalid("returning call has stale physical expansion evidence");
      // Exact metadata, including inactive operand slots, is part of the span.
      RecoveryDigest Actual("call-ops"), Expected("call-ops");
      for (size_t I = 0; I < Span.size(); ++I) {
        auto Op = Span[I];
        Op.Addr = Call.Original.Origin.Address;
        Op.Seq = static_cast<int>(I);
        Actual.operation(Op, std::size(Op.Inputs));
        Expected.operation(Expanded->Ops[I],
                           std::size(Expanded->Ops[I].Inputs));
      }
      if (Actual.finish() != Expected.finish())
        return invalid("returning call does not retain its exact invocation");
      Calls.erase(At);
    }
  if (!Calls.empty())
    return invalid("returning-call receipt has no residual span");
  return llvm::Error::success();
}

} // namespace neverd::analysis::x64
