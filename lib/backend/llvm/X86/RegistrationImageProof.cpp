//===- RegistrationImageProof.cpp - Current PE32 image writes ------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Keep image write preservation independent of private-frame call effects.
//===----------------------------------------------------------------------===//

#include "neverd/backend/llvm/RegistrationImageProof.h"

#include "neverd/Limits.h"

#include "llvm/IR/GlobalVariable.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/IR/Module.h"
#include "llvm/Support/ModRef.h"

namespace neverd::registration_frame {

std::optional<ExceptionAddressRange>
imageRange(const Cell &Where, uint64_t Bytes, va_t SecurityCookieVA) {
  const auto *Global = llvm::dyn_cast_or_null<llvm::GlobalVariable>(Where.Root);
  if (!Global || !Global->isDeclaration() || !Global->hasExternalLinkage() ||
      Global->isThreadLocal() || Global->getAddressSpace() ||
      Global->hasDLLImportStorageClass() || !Bytes)
    return std::nullopt;
  auto Base = parseNdDataSymbol(Global->getName());
  if (!Base)
    Base = parseNdCodePtrSymbol(Global->getName());
  if (!Base && SecurityCookieVA && Global->getName() == "__security_cookie")
    Base = SecurityCookieVA;
  if (!Base || *Base > UINT32_MAX || Where.Offset < -int64_t(*Base) ||
      Where.Offset > int64_t(UINT32_MAX - *Base))
    return std::nullopt;
  const va_t Address = va_t(int64_t(*Base) + Where.Offset);
  if (Bytes > uint64_t(UINT32_MAX) + 1 - Address)
    return std::nullopt;
  return ExceptionAddressRange{Address, Address + Bytes};
}

bool ImageProof::charge() {
  if (Exhausted || Work >= limits::kMaxRegistrationEHStateWork) {
    Exhausted = true;
    return false;
  }
  ++Work;
  return true;
}

bool ImageProof::preserves(const ExceptionAddressRange &Range) {
  if (!charge() || !Addresses.hasPE32Layout() || !Range.isValid() ||
      Range.End > uint64_t(UINT32_MAX) + 1 || Functions.empty())
    return false;
  const auto Key = std::make_pair(Range.Begin, Range.End);
  if (const auto Found = Preserved.find(Key); Found != Preserved.end())
    return Found->second;
  bool Result = true;
  for (const auto *Function : Functions) {
    if (!charge() || !Function || Function->isDeclaration()) {
      Result = false;
      break;
    }
    for (const auto &Block : *Function) {
      for (const auto &I : Block)
        if (!charge() || !Addresses.isIndexed(I) || !preserves(I, Range)) {
          Result = false;
          break;
        }
      if (!Result)
        break;
    }
    if (!Result)
      break;
  }
  if (!Exhausted)
    Preserved.emplace(Key, Result);
  return Result && !Exhausted;
}

bool ImageProof::preserves(const llvm::Instruction &I,
                           const ExceptionAddressRange &Range) {
  if (const auto *Store = llvm::dyn_cast<llvm::StoreInst>(&I)) {
    const auto Size = I.getModule()->getDataLayout().getTypeStoreSize(
        Store->getValueOperand()->getType());
    if (Size.isScalable() || !Size.getFixedValue())
      return false;
    const auto Where = Addresses.locate(Store->getPointerOperand());
    if (Where) {
      if (const auto *Root = llvm::dyn_cast<llvm::AllocaInst>(Where->Root)) {
        const auto Extent =
            Root->getAllocationSize(I.getModule()->getDataLayout());
        return Root->isStaticAlloca() && Extent && !Extent->isScalable() &&
               Where->Offset >= 0 &&
               uint64_t(Where->Offset) <= Extent->getFixedValue() &&
               Size.getFixedValue() <=
                   Extent->getFixedValue() - uint64_t(Where->Offset) &&
               Addresses.isAliveAt(*Root, I);
      }
      if (llvm::isa<llvm::GlobalVariable>(Where->Root)) {
        const auto Written =
            imageRange(*Where, Size.getFixedValue(), SecurityCookieVA);
        return Written && !Written->overlaps(Range);
      }
    }
    return Project && Project(I, Range);
  }
  // Volatile reads participate in LLVM's ordering classification, but do not
  // modify the image. Intrinsic effects come from the canonical definition.
  if (llvm::isa<llvm::LoadInst>(I))
    return true;
  if (const auto *Intrinsic = llvm::dyn_cast<llvm::IntrinsicInst>(&I)) {
    if (llvm::isa<llvm::LifetimeIntrinsic>(Intrinsic)) {
      const auto *Value = Intrinsic->getArgOperand(0);
      return llvm::isa<llvm::PoisonValue, llvm::AllocaInst>(Value);
    }
    switch (Intrinsic->getIntrinsicID()) {
    case llvm::Intrinsic::stackrestore:
    case llvm::Intrinsic::write_register:
    case llvm::Intrinsic::eh_sjlj_setjmp:
    case llvm::Intrinsic::eh_sjlj_longjmp:
      return Project && Project(I, Range);
    default:
      break;
    }
    const auto Attributes = llvm::Intrinsic::getAttributes(
        I.getContext(), Intrinsic->getIntrinsicID(),
        Intrinsic->getFunctionType());
    if (Attributes.getMemoryEffects()
            .getWithoutLoc(llvm::IRMemLocation::InaccessibleMem)
            .onlyReadsMemory())
      return true;
    return Project && Project(I, Range);
  }
  if (llvm::isa<llvm::CallBase, llvm::CatchPadInst>(I) || I.mayWriteToMemory())
    return Project && Project(I, Range);
  return true;
}

} // namespace neverd::registration_frame
