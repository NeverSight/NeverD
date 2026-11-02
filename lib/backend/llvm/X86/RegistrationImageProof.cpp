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

#include "llvm/IR/CFG.h"
#include "llvm/IR/GlobalVariable.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Operator.h"
#include "llvm/Support/ModRef.h"

#include <set>
#include <vector>

namespace neverd::registration_frame {

namespace {
std::optional<uint32_t> originalArgumentAddress(const llvm::CallBase &Call,
                                                unsigned Index, size_t &Work,
                                                bool PointerArgument) {
  if (Work >= limits::kMaxRegistrationEHStateWork) {
    Work = limits::kMaxRegistrationEHStateWork;
    return std::nullopt;
  }
  ++Work;
  const auto *Function =
      Call.getParent() ? Call.getParent()->getParent() : nullptr;
  const auto *Module = Function ? Function->getParent() : nullptr;
  if (!Module || Index >= Call.arg_size() ||
      Module->getDataLayout().getPointerSizeInBits(0) != 32 ||
      Module->getDataLayout().isNonIntegralAddressSpace(0))
    return std::nullopt;
  const auto *Value = Call.getArgOperand(Index);
  if (const auto *Instruction = llvm::dyn_cast<llvm::Instruction>(Value);
      Instruction && (!Instruction->getParent() ||
                      Instruction->getParent()->getParent() != Function))
    return std::nullopt;
  uint32_t Offset = 0;
  if (PointerArgument) {
    if (!Value->getType()->isPointerTy() ||
        Value->getType()->getPointerAddressSpace())
      return std::nullopt;
    if (const auto *GEP = llvm::dyn_cast<llvm::GEPOperator>(Value)) {
      const auto *Index =
          GEP->getNumIndices() == 1
              ? llvm::dyn_cast<llvm::ConstantInt>(GEP->getOperand(1))
              : nullptr;
      if (!GEP->getSourceElementType()->isIntegerTy(8) || !Index ||
          (Index->getBitWidth() != 32 && Index->getBitWidth() != 64) ||
          Index->getValue().isNegative() || Index->getValue().ugt(INT32_MAX))
        return std::nullopt;
      Offset = uint32_t(Index->getZExtValue());
      Value = GEP->getPointerOperand();
    }
  } else {
    const auto *Cast = llvm::dyn_cast<llvm::Operator>(Value);
    if (!Value->getType()->isIntegerTy(32) || !Cast ||
        Cast->getOpcode() != llvm::Instruction::PtrToInt ||
        Cast->getNumOperands() != 1)
      return std::nullopt;
    Value = Cast->getOperand(0);
  }
  const auto *Global = llvm::dyn_cast<llvm::GlobalVariable>(Value);
  if (!Global || Global->getParent() != Module || !Global->isDeclaration() ||
      !Global->hasExternalLinkage() || Global->isThreadLocal() ||
      Global->getAddressSpace() ||
      Global->getDLLStorageClass() != llvm::GlobalValue::DefaultStorageClass ||
      Global->getUnnamedAddr() != llvm::GlobalValue::UnnamedAddr::None)
    return std::nullopt;
  // PE32 names have at most eight hexadecimal digits. Refuse an overlong
  // name before parsing it, then bill the bounded spelling comparison.
  const auto Name = Global->getName();
  if (Name.size() > makeNdDataSymbol(UINT32_MAX).size())
    return std::nullopt;
  if (Name.size() + 1 > limits::kMaxRegistrationEHStateWork - Work) {
    Work = limits::kMaxRegistrationEHStateWork;
    return std::nullopt;
  }
  Work += Name.size() + 1;
  const auto Address = parseNdDataSymbol(Name);
  if (!Address || !*Address || *Address > UINT32_MAX ||
      Name != makeNdDataSymbol(*Address) || Offset > UINT32_MAX - *Address)
    return std::nullopt;
  return uint32_t(*Address) + Offset;
}
} // namespace

std::optional<uint32_t> originalImageArgumentAddress(const llvm::CallBase &Call,
                                                     unsigned Index,
                                                     size_t &Work) {
  return originalArgumentAddress(Call, Index, Work, false);
}

std::optional<uint32_t>
originalImagePointerArgumentAddress(const llvm::CallBase &Call, unsigned Index,
                                    size_t &Work) {
  return originalArgumentAddress(Call, Index, Work, true);
}

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

bool ImageProof::preservesBefore(const ExceptionAddressRange &Range,
                                 const llvm::Instruction &Before) {
  if (!charge() || !Addresses.hasPE32Layout() || !Range.isValid() ||
      Range.End > uint64_t(UINT32_MAX) + 1 || !Before.getParent() ||
      !Before.getParent()->getParent() || !Addresses.isIndexed(Before))
    return false;
  const auto *Function = Before.getFunction();
  bool Included = false;
  for (const auto *Candidate : Functions) {
    if (!charge())
      return false;
    if (Candidate == Function) {
      Included = true;
      break;
    }
  }
  if (!Included || !Function || Function->isDeclaration())
    return false;

  using Point = std::pair<const llvm::BasicBlock *, const llvm::Instruction *>;
  std::vector<Point> Pending{{Before.getParent(), Before.getPrevNode()}};
  std::set<Point> Seen;
  bool SawEntry = false;
  while (!Pending.empty()) {
    if (!charge())
      return false;
    const auto [Block, Through] = Pending.back();
    Pending.pop_back();
    if (!Block || Block->getParent() != Function)
      return false;
    // The initial prefix and a full predecessor block are different points,
    // even when a loop leads back into the original query block.
    if (!Seen.emplace(Block, Through).second)
      continue;
    for (const auto *I = Through; I; I = I->getPrevNode())
      if (!charge() || I->getParent() != Block || !Addresses.isIndexed(*I) ||
          !preserves(*I, Range))
        return false;
    SawEntry |= Block == &Function->getEntryBlock();
    for (const auto *Pred : llvm::predecessors(Block)) {
      if (!charge() || Pred->getParent() != Function || !Pred->getTerminator())
        return false;
      Pending.emplace_back(Pred, Pred->getTerminator());
    }
  }
  return SawEntry && !Exhausted;
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
