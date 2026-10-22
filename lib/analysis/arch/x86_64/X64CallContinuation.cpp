//===- X64CallContinuation.cpp - Conditional raw call semantics ---------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file Architecture-owned conditional normal-return policy and integrity.

#include "X64CallContinuation.h"

#include "../../core/RecoveryDigest.h"
#include "NativeStackControl.h"

#include "neverd/ir/TargetRegInfo.h"
#include "neverd/ir/low/LowRawCall.h"
#include "neverd/lift/X86Regs.h"

#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/Endian.h"

#include <algorithm>
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

void declaration(RecoveryDigest &H, const SpecializationX64ReturningCall &D) {
  H.number(D.Version);
  H.number(D.Address);
  H.number(static_cast<unsigned>(D.Profile));
  H.number(D.OutgoingBytes);
}
} // namespace

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
    if (D.Version != 1 || D.Address == InvalidVA ||
        D.Profile != SpecializationX64CallProfile::ConditionalNormalReturnV1 ||
        D.OutgoingBytes < kHomeBytes || D.OutgoingBytes > INT64_MAX ||
        D.OutgoingBytes % kAddressBytes)
      return invalid("invalid explicit returning-call relation");
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
  RecoveryDigest H("neverd-x64-conditional-normal-return-v1");
  declaration(H, D);
  const auto Bytes = llvm::fromHex(H.finish());
  std::array<uint64_t, 4> Words;
  for (size_t I = 0; I < Words.size(); ++I)
    Words[I] =
        llvm::support::endian::read64le(Bytes.data() + I * kAddressBytes);
  return Words;
}

RecoveryCallTransition returningCallTransition() {
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
  return {Preserved, kArithmeticFlags, NdVar::reg(x86reg::RSP, kAddressBytes),
          NdVar::reg(x86reg::RIP, kAddressBytes)};
}

std::string returningCallDigest(const SpecializationReturningCall &Call) {
  RecoveryDigest H("neverd-returning-call-span-v1");
  H.number(Call.Version);
  H.number(Call.ResidualAddress);
  declaration(H, Call.Declaration);
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
    H.operation(Op);
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
        Actual.operation(Op);
        Expected.operation(Expanded->Ops[I]);
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
