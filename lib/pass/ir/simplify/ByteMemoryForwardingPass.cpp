//===- ByteMemoryForwardingPass.cpp - Forward overlapping stack bytes
//-------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "neverd/pass/ir/simplify/ByteMemoryForwardingPass.h"

#include "neverd/pass/ir/simplify/SymSimplifyPass.h"

#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/Analysis/ValueTracking.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/NoFolder.h"
#include "llvm/IR/Operator.h"

#include <algorithm>
#include <map>
#include <optional>

namespace neverd {
namespace {

struct Address {
  llvm::AllocaInst *Object;
  uint64_t Offset;
  uint64_t Size;
};

struct StoredByte {
  llvm::StoreInst *Writer;
  unsigned Index;
};

bool charge(uint64_t &Used, uint64_t Amount, uint64_t Limit,
            ByteMemoryForwardingResult &Result) {
  if (Amount > Limit - Used) {
    Result.BudgetExhausted = true;
    return false;
  }
  Used += Amount;
  return true;
}

std::optional<Address> address(llvm::Value *Pointer, const llvm::DataLayout &DL,
                               const ByteMemoryForwardingOptions &Options,
                               ByteMemoryForwardingResult &Result) {
  if (Pointer->getType()->getPointerAddressSpace() != 0 ||
      DL.isNonIntegralPointerType(Pointer->getType()))
    return std::nullopt;
  const unsigned Width = DL.getIndexSizeInBits(0);
  // A narrower GEP index can wrap independently of the pointer representation.
  if ((Width != 32 && Width != 64) || Width != DL.getPointerSizeInBits(0))
    return std::nullopt;
  llvm::APInt Offset(Width, 0);
  for (;;) {
    if (!charge(Result.AddressSteps, 1, Options.MaxAddressSteps, Result))
      return std::nullopt;
    if (auto *Object = llvm::dyn_cast<llvm::AllocaInst>(Pointer)) {
      auto *Bytes = llvm::dyn_cast<llvm::ArrayType>(Object->getAllocatedType());
      auto *Count = llvm::dyn_cast<llvm::ConstantInt>(Object->getArraySize());
      if (!Object->isStaticAlloca() || Object->getAddressSpace() != 0 ||
          !Bytes || !Bytes->getElementType()->isIntegerTy(8) || !Count ||
          !Count->isOne() || Offset.isNegative())
        return std::nullopt;
      const uint64_t Size = Bytes->getNumElements();
      if (!Size || Size > 65536 || Offset.getZExtValue() >= Size)
        return std::nullopt;
      return Address{Object, Offset.getZExtValue(), Size};
    }
    // In particular, never strip inttoptr, addrspacecast, PHI or select. A
    // common underlying allocation alone does not establish an exact address.
    auto *GEP = llvm::dyn_cast<llvm::GEPOperator>(Pointer);
    if (!GEP || !charge(Result.AddressSteps, GEP->getNumIndices(),
                        Options.MaxAddressSteps, Result))
      return std::nullopt;
    llvm::APInt Delta(Width, 0);
    if (!GEP->accumulateConstantOffset(DL, Delta))
      return std::nullopt;
    bool Overflow = false;
    Offset = Offset.sadd_ov(Delta, Overflow);
    if (Overflow)
      return std::nullopt;
    Pointer = GEP->getPointerOperand();
  }
}

std::optional<unsigned> scalarBytes(llvm::Type *Type) {
  auto *Integer = llvm::dyn_cast<llvm::IntegerType>(Type);
  if (!Integer || Integer->getBitWidth() < 8 || Integer->getBitWidth() > 128 ||
      Integer->getBitWidth() % 8)
    return std::nullopt;
  return Integer->getBitWidth() / 8;
}

bool needsSnapshot(llvm::Value *Value, llvm::StoreInst *Store) {
  // Restrict the optional definedness query to leaves with a constant-time
  // answer. Recursing into a PHI can visit arbitrarily many incoming edges,
  // even with ValueTracking's depth limit. An extra freeze is conservative;
  // ordinary LLVM cleanup can remove it after proving the expression defined.
  if (!llvm::isa<llvm::ConstantInt, llvm::Argument, llvm::FreezeInst>(Value))
    return true;
  return !llvm::isGuaranteedNotToBeUndefOrPoison(Value, nullptr, Store);
}

struct NumericAddress {
  llvm::Value *Root;
  llvm::APInt Offset;
};

std::optional<NumericAddress>
numericAddress(llvm::Value *Pointer, const llvm::DataLayout &DL,
               const ByteMemoryForwardingOptions &Options,
               ByteMemoryForwardingResult &Result) {
  if (Pointer->getType()->getPointerAddressSpace() != 0 ||
      DL.isNonIntegralPointerType(Pointer->getType()))
    return std::nullopt;
  const unsigned Width = DL.getPointerSizeInBits(0);
  if ((Width != 32 && Width != 64) || Width != DL.getIndexSizeInBits(0))
    return std::nullopt;
  llvm::APInt Offset(Width, 0);
  for (;;) {
    if (!charge(Result.AddressSteps, 1, Options.MaxAddressSteps, Result))
      return std::nullopt;
    auto *GEP = llvm::dyn_cast<llvm::GEPOperator>(Pointer);
    if (!GEP)
      break;
    if (!charge(Result.AddressSteps, GEP->getNumIndices(),
                Options.MaxAddressSteps, Result))
      return std::nullopt;
    llvm::APInt Delta(Width, 0);
    if (!GEP->accumulateConstantOffset(DL, Delta))
      return std::nullopt;
    Offset += Delta;
    Pointer = GEP->getPointerOperand();
  }
  auto *Cast = llvm::dyn_cast<llvm::IntToPtrInst>(Pointer);
  if (!Cast || !Cast->getOperand(0)->getType()->isIntegerTy(Width))
    return std::nullopt;
  llvm::Value *Value = Cast->getOperand(0);
  for (;;) {
    if (!charge(Result.AddressSteps, 1, Options.MaxAddressSteps, Result))
      return std::nullopt;
    auto *Binary = llvm::dyn_cast<llvm::BinaryOperator>(Value);
    if (Binary && (Binary->getOpcode() == llvm::Instruction::Add ||
                   Binary->getOpcode() == llvm::Instruction::Sub)) {
      llvm::Value *Next = Binary->getOperand(0);
      auto *Constant = llvm::dyn_cast<llvm::ConstantInt>(Binary->getOperand(1));
      if (!Constant && Binary->getOpcode() == llvm::Instruction::Add) {
        Constant = llvm::dyn_cast<llvm::ConstantInt>(Next);
        Next = Binary->getOperand(1);
      }
      if (Constant) {
        Offset += Binary->getOpcode() == llvm::Instruction::Add
                      ? Constant->getValue()
                      : -Constant->getValue();
        Value = Next;
        continue;
      }
    }
    // Preserve every other operation as an exact SSA root. In particular,
    // never strip masks, truncation, extension or address-space casts.
    return NumericAddress{Value, std::move(Offset)};
  }
}

// Two scans keep forwarding facts separate from dead-store candidates. The
// second scan sees only the reads that really survived the first scan,
// including reads retained because of a budget or unsupported coercion.
bool simplifyNumericMemory(llvm::Function &F,
                           const ByteMemoryForwardingOptions &Options,
                           ByteMemoryForwardingResult &Result, bool Delete) {
  const auto &DL = F.getParent()->getDataLayout();
  for (auto &BB : F) {
    llvm::Value *Root = nullptr;
    // Every modular offset is valid, including DenseMap's integral sentinel
    // keys (-1 and -2). An ordered map keeps these addresses distinct.
    std::map<uint64_t, StoredByte> Bytes;
    const auto Clear = [&] {
      Root = nullptr;
      Bytes.clear();
    };
    const auto Step = [&](uint64_t Count = 1) {
      return charge(Result.MemorySteps, Count, Options.MaxMemorySteps, Result);
    };
    for (auto It = BB.begin(); It != BB.end();) {
      llvm::Instruction *I = &*It++;
      if (!charge(Result.Instructions, 1, Options.MaxInstructions, Result))
        return false;
      auto *Store = llvm::dyn_cast<llvm::StoreInst>(I);
      auto *Load = llvm::dyn_cast<llvm::LoadInst>(I);
      if (!Store && (!Load || Delete)) {
        if (llvm::isa<llvm::CallBase, llvm::FenceInst>(I) || I->mayThrow() ||
            I->mayWriteToMemory() || (Delete && I->mayReadFromMemory()))
          Clear();
        continue;
      }
      auto Width = scalarBytes(Store ? Store->getValueOperand()->getType()
                                     : Load->getType());
      if (!Width || (Store && !Store->isSimple()) ||
          (Load && !Load->isSimple())) {
        if (Store || (Load && !Load->isSimple()))
          Clear();
        continue;
      }
      auto A = numericAddress(Store ? Store->getPointerOperand()
                                    : Load->getPointerOperand(),
                              DL, Options, Result);
      if (Result.BudgetExhausted)
        return false;
      if (!A) {
        if (Store)
          Clear();
        continue;
      }
      const auto Offset = [&](unsigned Index) {
        return (A->Offset + Index).getZExtValue();
      };
      if (Store) {
        // Different roots, including numeric vs alloca pointers, are MayAlias.
        if (Root != A->Root)
          Clear();
        Root = A->Root;
        llvm::SmallPtrSet<llvm::StoreInst *, 16> Seen;
        for (unsigned J = 0; J != *Width; ++J) {
          if (!Step())
            return false;
          auto At = Bytes.find(Offset(J));
          if (!Delete || At == Bytes.end())
            continue;
          const StoredByte Old = At->second;
          if (!Seen.insert(Old.Writer).second)
            continue;
          const unsigned OldWidth =
              *scalarBytes(Old.Writer->getValueOperand()->getType());
          const llvm::APInt Start = A->Offset + J - Old.Index;
          const llvm::APInt Delta = Start - A->Offset;
          if (OldWidth > *Width || Delta.ugt(*Width - OldWidth))
            continue;
          // At most sixteen cached bytes belong to any one writer. Charge
          // each lookup, including those already replaced by another store.
          for (unsigned K = 0; K != OldWidth; ++K) {
            if (!Step())
              return false;
            auto Part = Bytes.find((Start + K).getZExtValue());
            if (Part != Bytes.end() && Part->second.Writer == Old.Writer)
              Bytes.erase(Part);
          }
          Old.Writer->eraseFromParent();
          ++Result.RemovedStores;
        }
        for (unsigned J = 0; J != *Width; ++J) {
          if (!Step())
            return false;
          const uint64_t Key = Offset(J);
          if (!Bytes.contains(Key) && Bytes.size() >= Options.MaxTrackedBytes) {
            Result.BudgetExhausted = true;
            return false;
          }
          Bytes[Key] = {Store, J};
        }
        Result.PeakTrackedBytes =
            std::max(Result.PeakTrackedBytes, uint64_t(Bytes.size()));
        continue;
      }
      if (Root != A->Root)
        continue;
      std::optional<StoredByte> First;
      bool Complete = true;
      for (unsigned J = 0; J != *Width; ++J) {
        if (!Step())
          return false;
        auto At = Bytes.find(Offset(J));
        if (At == Bytes.end() ||
            (First && (At->second.Writer != First->Writer ||
                       At->second.Index != First->Index + J))) {
          Complete = false;
          break;
        }
        if (!First)
          First = At->second;
      }
      if (!Complete)
        continue;
      llvm::Value *Value = First->Writer->getValueOperand();
      const unsigned StoredWidth = *scalarBytes(Value->getType());
      const unsigned Shift =
          8 * (DL.isLittleEndian() ? First->Index
                                   : StoredWidth - *Width - First->Index);
      for (const llvm::Use &Use : Load->uses()) {
        (void)Use;
        if (!charge(Result.UseSteps, 2, Options.MaxUseSteps, Result))
          return false;
      }
      if (!charge(Result.NewInstructions,
                  unsigned(Shift != 0) +
                      unsigned(Value->getType() != Load->getType()),
                  Options.MaxNewInstructions, Result))
        return false;
      // One writer, used once: ordinary LLVM store-to-load coercion. No
      // byte composition and no additional choices of undef/poison values.
      llvm::IRBuilder<llvm::NoFolder> Builder(Load);
      if (Shift)
        Value = Builder.CreateLShr(Value, Shift);
      if (Value->getType() != Load->getType())
        Value = Builder.CreateTrunc(Value, Load->getType());
      Load->replaceAllUsesWith(Value);
      Load->eraseFromParent();
      ++Result.ForwardedLoads;
    }
  }
  return true;
}

} // namespace

ByteMemoryForwardingResult
ByteMemoryForwardingPass::forward(llvm::Function &F,
                                  ByteMemoryForwardingOptions Options) {
  ByteMemoryForwardingResult Result;
  if (F.isDeclaration() || F.hasFnAttribute(kObfuscatedFnAttr))
    return Result;
  const llvm::DataLayout &DL = F.getParent()->getDataLayout();
  for (llvm::BasicBlock &BB : F) {
    llvm::DenseMap<llvm::AllocaInst *, llvm::DenseMap<uint64_t, StoredByte>>
        Memory;
    uint64_t TrackedBytes = 0;
    auto Invalidate = [&] {
      Memory.clear();
      TrackedBytes = 0;
    };
    for (auto It = BB.begin(); It != BB.end();) {
      llvm::Instruction *I = &*It++;
      if (!charge(Result.Instructions, 1, Options.MaxInstructions, Result))
        return Result;
      auto *Store = llvm::dyn_cast<llvm::StoreInst>(I);
      auto *Load = llvm::dyn_cast<llvm::LoadInst>(I);
      if (!Store && !Load) {
        if (llvm::isa<llvm::CallBase, llvm::FenceInst>(I) ||
            I->mayWriteToMemory())
          Invalidate();
        continue;
      }
      auto Width = scalarBytes(Store ? Store->getValueOperand()->getType()
                                     : Load->getType());
      if (!Width || (Store && !Store->isSimple()) ||
          (Load && !Load->isSimple())) {
        if (Store || (Load && !Load->isSimple()))
          Invalidate();
        continue;
      }
      auto A = address(Store ? Store->getPointerOperand()
                             : Load->getPointerOperand(),
                       DL, Options, Result);
      if (!A || *Width > A->Size - A->Offset) {
        if (Store)
          Invalidate();
        continue;
      }
      auto Found = Memory.find(A->Object);
      if (Store) {
        uint64_t Added = 0;
        for (unsigned J = 0; J != *Width; ++J)
          Added +=
              Found == Memory.end() || !Found->second.contains(A->Offset + J);
        if (Added > Options.MaxTrackedBytes - TrackedBytes) {
          Result.BudgetExhausted = true;
          Invalidate();
          continue;
        }
        auto &Bytes = Memory[A->Object];
        for (unsigned J = 0; J != *Width; ++J)
          Bytes[A->Offset + J] = {Store,
                                  DL.isLittleEndian() ? J : *Width - 1 - J};
        TrackedBytes += Added;
        Result.PeakTrackedBytes =
            std::max(Result.PeakTrackedBytes, TrackedBytes);
        continue;
      }
      if (Found == Memory.end())
        continue;
      llvm::SmallVector<StoredByte, 16> Parts;
      for (unsigned J = 0; J != *Width; ++J) {
        auto Part = Found->second.find(A->Offset + J);
        if (Part == Found->second.end())
          break;
        Parts.push_back(Part->second);
      }
      if (Parts.size() != *Width)
        continue;

      bool UsesFit = true;
      for (const llvm::Use &Use : Load->uses()) {
        (void)Use;
        // Pay for this preflight walk and the corresponding RAUW visit.
        if (!charge(Result.UseSteps, 2, Options.MaxUseSteps, Result)) {
          UsesFit = false;
          break;
        }
      }
      if (!UsesFit)
        continue;

      llvm::SmallPtrSet<llvm::StoreInst *, 16> Seen;
      llvm::SmallVector<llvm::StoreInst *, 16> Snapshots;
      uint64_t Cost = 0;
      for (const StoredByte &Part : Parts) {
        llvm::Value *Value = Part.Writer->getValueOperand();
        Cost += 3 + !Value->getType()->isIntegerTy(8) + (*Width > 1);
        if (Seen.insert(Part.Writer).second &&
            needsSnapshot(Value, Part.Writer))
          Snapshots.push_back(Part.Writer);
      }
      Cost += Snapshots.size();
      if (!Options.AllowStoreSnapshots && !Snapshots.empty())
        continue;
      // Reserve the entire reconstruction before touching either the store or
      // load. NoFolder makes this an exact count, independent of constants.
      if (!charge(Result.NewInstructions, Cost, Options.MaxNewInstructions,
                  Result))
        continue;
      for (llvm::StoreInst *Writer : Snapshots) {
        llvm::IRBuilder<llvm::NoFolder> AtStore(Writer);
        Writer->setOperand(0, AtStore.CreateFreeze(Writer->getValueOperand(),
                                                   "byte.snapshot"));
        ++Result.FrozenStores;
      }
      llvm::IRBuilder<llvm::NoFolder> B(Load);
      llvm::Value *Value = llvm::ConstantInt::get(Load->getType(), 0);
      for (unsigned J = 0; J != *Width; ++J) {
        const StoredByte Part = Parts[J];
        llvm::Value *Slice =
            B.CreateLShr(Part.Writer->getValueOperand(), Part.Index * 8);
        Slice = B.CreateTruncOrBitCast(Slice, B.getInt8Ty());
        Slice = B.CreateZExtOrTrunc(Slice, Load->getType());
        Slice =
            B.CreateShl(Slice, (DL.isLittleEndian() ? J : *Width - 1 - J) * 8);
        Value = B.CreateOr(Value, Slice);
      }
      Load->replaceAllUsesWith(Value);
      Load->eraseFromParent();
      ++Result.ForwardedLoads;
    }
  }
  if (Options.SimplifyNumericMemory && !Result.BudgetExhausted &&
      simplifyNumericMemory(F, Options, Result, false))
    simplifyNumericMemory(F, Options, Result, true);
  return Result;
}

llvm::PreservedAnalyses
ByteMemoryForwardingPass::run(llvm::Function &F,
                              llvm::FunctionAnalysisManager &) {
  const auto Result = forward(F, Options);
  return Result.ForwardedLoads || Result.RemovedStores
             ? llvm::PreservedAnalyses::none()
             : llvm::PreservedAnalyses::all();
}

} // namespace neverd
