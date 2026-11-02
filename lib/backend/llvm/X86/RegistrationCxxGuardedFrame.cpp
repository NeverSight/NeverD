//===- RegistrationCxxGuardedFrame.cpp - Current PE32 GS frame ------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Bind complete original effects before proving a final guarded LLVM frame.
//===----------------------------------------------------------------------===//

#include "neverd/backend/llvm/RegistrationCxxGuardedFrame.h"

#include "neverd/Limits.h"
#include "neverd/backend/llvm/RegistrationCxxCookieBindings.h"
#include "neverd/backend/llvm/RegistrationFrameProof.h"
#include "neverd/backend/llvm/X86/X86RegistrationLayout.h"
#include "neverd/ir/low/RegistrationABI.h"
#include "neverd/ir/low/RegistrationCallEffects.h"
#include "neverd/ir/med/MedIR.h"
#include "neverd/loader/BinaryImage.h"
#include "neverd/loader/COFF/COFFRegistrationEH.h"

#include "llvm/IR/Instructions.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Verifier.h"
#include "llvm/Support/Errc.h"

#include <map>
#include <optional>
#include <utility>
#include <vector>

namespace neverd::registration_frame {
namespace {
llvm::Error reject(const llvm::Twine &Detail) {
  return llvm::createStringError(llvm::errc::invalid_argument,
                                 "C++ guarded frame: " + Detail);
}
} // namespace

llvm::Error
validateCxxGuardedFrame(const BinaryImage &Image, const MedFunc &Source,
                        const llvm::Function &Parent,
                        const llvm::AllocaInst &Frame,
                        const X86RegistrationFrameLayout &Layout,
                        llvm::ArrayRef<CxxSourceCallBinding> Calls,
                        llvm::ArrayRef<CxxCleanupCallBinding> Cleanups,
                        const std::set<const llvm::Instruction *> &ChainReads) {
  if (!Parent.getParent() || llvm::verifyFunction(Parent) ||
      !Source.ExceptionMetadata || !Source.ExceptionMetadata->Cxx ||
      !Source.ExceptionMetadata->Registration || !Source.RegistrationStates)
    return reject("missing source contract");
  const auto &EH = *Source.ExceptionMetadata;
  const auto &States = *Source.RegistrationStates;
  if (!EH.Cxx->TryBlocks.empty() || !States.IncomingFrameAccesses.empty() ||
      !States.RuntimeObjectAccesses.empty() ||
      !EH.Registration->RegistrationOffset)
    return reject("unsupported runtime object or caller-frame domain");
  size_t Work = 0;
  auto Charge = [&] {
    if (Work >= limits::kMaxRegistrationEHStateWork)
      return false;
    ++Work;
    return true;
  };
  // Some producer occurrences precede unreachable/dead-spill removal. Compare
  // identities before dereferencing any retained instruction pointer.
  std::set<const llvm::Instruction *> Live;
  for (const auto &Block : Parent)
    for (const auto &I : Block) {
      if (!Charge())
        return reject("instruction index exceeds the shared work budget");
      Live.insert(&I);
    }
  if (!Live.count(&Frame))
    return reject("logical frame no longer exists");
  for (const auto &Call : Calls)
    if (!Charge() || !Live.count(Call.Call))
      return reject("source call no longer exists");
  for (const auto &Call : Cleanups)
    if (!Charge() || !Live.count(Call.Call) || !Live.count(Call.Pad))
      return reject("cleanup call no longer exists");
  for (const auto *Read : ChainReads)
    if (!Charge() || !Live.count(Read))
      return reject("projected chain read no longer exists");
  auto Coverage = bindCxxCookieCoverage(Image, Source, Parent, Frame, Layout,
                                        Calls, Cleanups, {}, Work);
  if (!Coverage)
    return Coverage.takeError();
  const auto Handler = coff_loader::getCheckedX86CxxHandlerABI(Image, EH);
  auto Immutable = coff_loader::getCheckedX86CxxMetadataRanges(Image, EH);
  if (!Handler || !Immutable)
    return reject("runtime image proof changed");
  auto Append = [&](llvm::ArrayRef<ExceptionAddressRange> Ranges) {
    for (const auto &Range : Ranges) {
      if (!Charge() || !Range.isValid() || Range.End > uint64_t(UINT32_MAX) + 1)
        return false;
      Immutable->push_back(Range);
    }
    return true;
  };
  if (!Append(Handler->Dispatch.CodeRanges) ||
      !Append({{Handler->Dispatch.IATVA, Handler->Dispatch.IATVA + 4}}))
    return reject("runtime code extent is incomplete");
  for (const auto &Segment : Image.Segments) {
    if (!Charge() || Segment.VA > UINT32_MAX ||
        Segment.Size > uint64_t(UINT32_MAX) + 1 - Segment.VA)
      return reject("image segment is not a bounded PE32 range");
    if (Segment.Size &&
        (!Segment.isWritable() || Segment.ReadOnlyAfterRelocations) &&
        !Append({{Segment.VA, Segment.VA + Segment.Size}}))
      return reject("immutable segment extent is incomplete");
  }
  coff_registration::RegistrationCxxFrameContract Contract;
  Contract.Image = &Image;
  Contract.SavedStackOffset =
      int64_t(Layout.Establisher) + *EH.Registration->RegistrationOffset - 4;
  auto Bind = [&](const llvm::CallBase *Call,
                  const RegistrationCalleeFrameContract &Callee,
                  std::optional<int32_t> Object) {
    if (!Charge() ||
        !Contract.ImageWrites.emplace(Call, Callee.ImageWrites).second ||
        !Append(Callee.CodeRanges))
      return false;
    for (va_t Slot : Callee.RequiredImports)
      if (!Append({{Slot, Slot + 4}}))
        return false;
    if (Object) {
      const auto Offset = Layout.runtimeOffset(*Object, 0);
      if (!Offset)
        return false;
      auto &Borrow = Contract.Borrows[Call];
      Borrow.Offset = *Offset;
      Borrow.Reads = Callee.ECXReads;
      Borrow.Writes = Callee.ECXWrites;
      Borrow.Initializer = Callee.isInitializer();
      Borrow.Initialized = Callee.ECXInitialized;
      Borrow.ScalarStackWords = Callee.StackPopBytes / 4;
    }
    return true;
  };
  for (const auto &Binding : Calls) {
    if (Binding.CallKind == CxxSourceCallBinding::Kind::Cookie)
      continue;
    const auto *Effect = States.callFrameEffect(Binding.Address, Binding.OpSeq);
    if (!Charge() || !Effect ||
        Effect->CalleeIndex >= States.CalleeContracts.size() ||
        Effect->RuntimeThrow || Effect->ECXRuntimeObject)
      return reject("ordinary call has an unbound runtime domain");
    const auto &Callee = States.CalleeContracts[Effect->CalleeIndex];
    if (Callee.isRuntimeThrow())
      return reject("direct runtime throw needs caller-wide closure");
    if (Callee.isThrow()) {
      const auto Fresh =
          getCheckedX86RegistrationThrowCalleeABI(Image, Callee.Target, &Work);
      if (!Fresh || !hasSameX86RegistrationThrowEffects(Callee, *Fresh, Work) ||
          !Append({{Fresh->ImportIATVA, Fresh->ImportIATVA + 4}}))
        return reject("private throw effects changed");
      if (!Fresh->IsRethrow &&
          (!Append(Fresh->ThrowInfo.ReadOnlyRanges) ||
           !Append({Fresh->ThrowInfo.TypeDescriptorRange})))
        return reject("private throw metadata extent changed");
    } else if (Callee.isInitializer()) {
      const auto Fresh = getCheckedX86RegistrationInitializerCalleeABI(
          Image, Callee.Target, &Work);
      if (!Fresh ||
          !hasSameX86RegistrationLeafEffects(Callee, Fresh->Object, Work) ||
          Callee.ECXInitialized != Fresh->Initialized)
        return reject("initializer effects changed");
    } else if (Callee.isCxxParent()) {
      const auto Fresh =
          getCheckedX86RegistrationCxxCalleeABI(Image, Callee.Target, &Work);
      if (!Fresh || !hasSameX86RegistrationCxxEffects(Callee, *Fresh, Work))
        return reject("C++ parent effects changed");
      for (const auto &Info : Fresh->RuntimeThrowInfos) {
        const auto Object =
            getCheckedX86RegistrationThrowObjectABI(Image, Info.Address, &Work);
        if (!Object || !Append(Object->Info.ReadOnlyRanges) ||
            !Append({Object->Info.TypeDescriptorRange}))
          return reject("C++ parent throw metadata changed");
      }
    } else {
      const auto Fresh = getCheckedX86RegistrationMemberCalleeABI(
          Image, Callee.Target, &Work, RegistrationLeafResult::Discarded);
      if (!Fresh || !hasSameX86RegistrationLeafEffects(Callee, *Fresh, Work) ||
          Callee.HasIndependentScalarReturn !=
              Fresh->HasIndependentScalarReturn)
        return reject("ordinary callee effects changed");
    }
    if (!Bind(Binding.Call, Callee, Effect->ECXFrameOffset))
      return reject("ordinary call effects could not be bound");
  }
  std::map<uint32_t, const RegistrationCleanupFrameContract *> CleanupSources;
  for (const auto &C : States.CleanupContracts)
    if (!Charge() || !CleanupSources.emplace(C.ActionState, &C).second)
      return reject("cleanup source set is duplicated or exceeds the budget");
  for (const auto &Binding : Cleanups) {
    const auto Found = CleanupSources.find(Binding.ActionState);
    // Coverage already reauthenticated the complete ordered relay effects.
    if (!Charge() || Found == CleanupSources.end() ||
        Binding.Ordinal >= Found->second->Calls.size())
      return reject("cleanup source occurrence changed");
    const auto &Call = Found->second->Calls[Binding.Ordinal];
    const auto Offset =
        EH.Registration->cxxSourceFrameOffset(Call.ObjectFrameOffset);
    if (!Offset || !Bind(Binding.Call, Call.Leaf, *Offset))
      return reject("cleanup object effects could not be bound");
  }
  Contract.CookieCalls = std::move(Coverage->Calls);
  Contract.CookieDispatches = std::move(Coverage->Dispatches);
  return coff_registration::validateFramePrivacy(
      Parent, {}, &Frame, {}, {}, {}, ChainReads, Coverage->SecurityCookieVA,
      *Immutable, &Contract);
}
} // namespace neverd::registration_frame
