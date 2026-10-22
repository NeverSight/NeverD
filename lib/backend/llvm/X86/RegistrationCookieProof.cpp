//===- RegistrationCookieProof.cpp - Current PE32 GS protocol ------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Establish successful checks in causal order before projecting their effects.
//===----------------------------------------------------------------------===//

#include "neverd/backend/llvm/RegistrationCookieProof.h"

#include "neverd/Limits.h"
#include "neverd/backend/RewriteSourceIdentity.h"
#include "neverd/backend/llvm/RegistrationFrameXor.h"
#include "neverd/backend/llvm/WindowsRegistrationFrame.h"
#include "neverd/loader/COFF/COFFRegistrationEH.h"

#include "llvm/ADT/STLExtras.h"
#include "llvm/IR/Attributes.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Module.h"

#include <map>
#include <optional>
#include <set>
#include <utility>

namespace neverd::registration_frame {

bool hasExactCookieCallABI(const llvm::CallBase &Call, va_t CheckVA) {
  const auto *Callee = Call.getCalledFunction();
  if (!CheckVA || CheckVA > UINT32_MAX || !Call.getParent() ||
      !Call.getFunction() || !Call.getModule() || !Callee ||
      Callee->getParent() != Call.getModule() || Callee->isIntrinsic() ||
      !hasX86RegistrationSecurityCheckABI(*Callee) ||
      !llvm::isa<llvm::CallInst>(Call) || Call.isInlineAsm() ||
      Call.isTailCall() || Call.arg_size() != 1 ||
      Call.getFunctionType() != Callee->getFunctionType() ||
      Call.getCallingConv() != llvm::CallingConv::X86_FastCall ||
      Call.getNumOperandBundles() > 1 ||
      (Call.getNumOperandBundles() && !Call.getOperandBundle("funclet")))
    return false;
  if (auto Bundle = Call.getOperandBundle("funclet"))
    if (Bundle->Inputs.size() != 1 ||
        !Bundle->Inputs[0]->getType()->isTokenTy())
      return false;
  auto Attributes = [](const llvm::AttributeList &List) {
    if (List.getFnAttrs().hasAttributes() ||
        List.getRetAttrs().hasAttributes() ||
        !List.hasParamAttr(0, llvm::Attribute::InReg))
      return false;
    return llvm::all_of(List.getParamAttrs(0), [](llvm::Attribute Attribute) {
      return Attribute.isEnumAttribute() &&
             Attribute.getKindAsEnum() == llvm::Attribute::InReg;
    });
  };
  if (!Attributes(Call.getAttributes()) || !Attributes(Callee->getAttributes()))
    return false;
  auto Identity = rewrite_source::getOriginalVA(*Callee);
  if (!Identity) {
    llvm::consumeError(Identity.takeError());
    return false;
  }
  return *Identity && **Identity == CheckVA;
}

bool proveCookieProtocol(const BinaryImage &Image,
                         const CookieProofContext &Context,
                         llvm::ArrayRef<CookieCall> Calls,
                         llvm::ArrayRef<CookieDispatch> Dispatches,
                         size_t &Work, bool &Exhausted) {
  auto Charge = [&] {
    if (Exhausted || Work >= limits::kMaxRegistrationEHStateWork) {
      Exhausted = true;
      return false;
    }
    ++Work;
    return true;
  };
  if (!Charge() || !Context.Parent.getParent() ||
      Context.Parent.isDeclaration() || !Context.SecurityCookieVA ||
      Context.SecurityCookieVA > UINT32_MAX - 3 || Context.Functions.empty())
    return false;
  bool IncludesParent = false;
  for (const auto *Function : Context.Functions) {
    if (!Charge())
      return false;
    IncludesParent |= Function == &Context.Parent;
  }
  if (!IncludesParent)
    return false;
  const ExceptionAddressRange CookieRange{Context.SecurityCookieVA,
                                          Context.SecurityCookieVA + 4};
  std::map<const llvm::CallBase *, va_t> Pending;
  std::set<const llvm::Instruction *> KnownCalls, Certified;
  std::set<std::pair<va_t, va_t>> CheckerRanges;
  auto Authenticate = [&](va_t CheckVA) {
    if (!Charge())
      return false;
    const auto Success =
        coff_loader::getCheckedX86CookieCheckSuccessPath(Image, CheckVA);
    if (!Success || Success->SecurityCookieVA != Context.SecurityCookieVA)
      return false;
    CheckerRanges.emplace(Success->CodeRange.Begin, Success->CodeRange.End);
    return true;
  };
  for (const auto &Check : Calls) {
    if (!Charge() || !Check.Call || !Check.Call->getParent() ||
        Check.Call->getFunction() != &Context.Parent ||
        !Context.Stores.contains(*Check.Call) ||
        !hasExactCookieCallABI(*Check.Call, Check.CheckVA) ||
        !Authenticate(Check.CheckVA) ||
        !Pending.emplace(Check.Call, Check.CheckVA).second)
      return false;
    KnownCalls.insert(Check.Call);
  }
  std::set<const llvm::Instruction *> DispatchPoints;
  for (const auto &Dispatch : Dispatches) {
    if (!Charge() || !Dispatch.Before || !Dispatch.Before->getParent() ||
        Dispatch.Guards.empty() ||
        Dispatch.Before->getFunction() != &Context.Parent ||
        !llvm::isa<llvm::CallBase>(Dispatch.Before) ||
        !Context.Stores.contains(*Dispatch.Before) ||
        !DispatchPoints.insert(Dispatch.Before).second)
      return false;
    for (const auto &Guard : Dispatch.Guards)
      if (!Authenticate(Guard.CheckVA))
        return false;
  }
  // Each query sees one immutable set. Its address/value/image caches die
  // before another check can be added to Certified.
  auto Query =
      [&](llvm::function_ref<bool(AddressProof &, XorProof &, ImageProof &)>
              Check) {
        auto Memory = [&](const llvm::Instruction &I, const Cell &Slot,
                          uint64_t Bytes) {
          if (KnownCalls.count(&I))
            return Certified.count(&I) != 0;
          return Context.PreservesMemory &&
                 Context.PreservesMemory(I, Slot, Bytes);
        };
        auto Writes = [&](const llvm::Instruction &I,
                          const ExceptionAddressRange &Range) {
          if (KnownCalls.count(&I))
            return Certified.count(&I) != 0;
          return Context.PreservesImage && Context.PreservesImage(I, Range);
        };
        AddressProof Addresses(Context.Parent, Context.Escape,
                               Context.SecurityCookieVA, Context.Stores, Work,
                               Exhausted, Context.ProjectLoad,
                               Context.Predecessors, Context.Bounds, Memory);
        XorProof Values(Addresses, Context.SecurityCookieVA, Work, Exhausted);
        ImageProof ImageOwner(Addresses, Context.Functions,
                              Context.SecurityCookieVA, Work, Exhausted,
                              Writes);
        return Addresses.hasPE32Layout() &&
               Check(Addresses, Values, ImageOwner) && !Exhausted;
      };
  while (!Pending.empty()) {
    bool Progress = false;
    for (auto It = Pending.begin(); It != Pending.end();) {
      if (!Charge())
        return false;
      const auto *Call = It->first;
      const bool Success =
          Query([&](AddressProof &, XorProof &Values, ImageProof &ImageOwner) {
            if (!ImageOwner.preservesBefore(CookieRange, *Call))
              return false;
            for (const auto &[Begin, End] : CheckerRanges)
              if (!Charge() || !ImageOwner.preservesBefore({Begin, End}, *Call))
                return false;
            const auto Value = Values.evaluate(Call->getArgOperand(0));
            return Value && *Value == XorValue{0, true, {}};
          });
      if (Exhausted)
        return false;
      if (!Success) {
        ++It;
        continue;
      }
      Certified.insert(Call);
      It = Pending.erase(It);
      Progress = true;
    }
    if (!Progress)
      return false;
  }
  return Query(
      [&](AddressProof &Addresses, XorProof &Values, ImageProof &ImageOwner) {
        for (const auto &Dispatch : Dispatches) {
          if (!Charge() ||
              !ImageOwner.preservesBefore(CookieRange, *Dispatch.Before) ||
              !Context.PreservesImage ||
              !Context.PreservesImage(*Dispatch.Before, CookieRange))
            return false;
          for (const auto &[Begin, End] : CheckerRanges)
            if (!Charge() ||
                !ImageOwner.preservesBefore({Begin, End}, *Dispatch.Before) ||
                !Context.PreservesImage(*Dispatch.Before, {Begin, End}))
              return false;
          for (const auto &Guard : Dispatch.Guards) {
            const auto *Root =
                llvm::dyn_cast_or_null<llvm::AllocaInst>(Guard.Slot.Root);
            const auto Extent =
                Root ? Root->getAllocationSize(
                           Context.Parent.getParent()->getDataLayout())
                     : std::nullopt;
            if (!Charge() || !Root || !Root->getParent() ||
                Root->getFunction() != &Context.Parent ||
                !Context.Stores.contains(*Root) || !Root->isStaticAlloca() ||
                !Extent || Extent->isScalable() || Guard.Slot.Offset < 0 ||
                uint64_t(Guard.Slot.Offset) > Extent->getFixedValue() ||
                4 > Extent->getFixedValue() - uint64_t(Guard.Slot.Offset) ||
                Guard.XORBase.Root != Root || Guard.XORBase.Offset < 0 ||
                uint64_t(Guard.XORBase.Offset) > Extent->getFixedValue() ||
                !Addresses.isAliveAt(*Root, *Dispatch.Before) ||
                !Context.PreservesMemory ||
                !Context.PreservesMemory(*Dispatch.Before, Guard.Slot, 4))
              return false;
            const auto Value = Values.readWordAt(Guard.Slot, *Dispatch.Before);
            if (!Value || *Value != XorValue{0, true, {Guard.XORBase}})
              return false;
          }
        }
        if (!ImageOwner.preserves(CookieRange))
          return false;
        for (const auto &[Begin, End] : CheckerRanges)
          if (!Charge() || !ImageOwner.preserves({Begin, End}))
            return false;
        return true;
      });
}

} // namespace neverd::registration_frame
