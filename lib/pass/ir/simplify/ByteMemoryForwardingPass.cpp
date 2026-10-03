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
#include "llvm/IR/Dominators.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/NoFolder.h"
#include "llvm/IR/Operator.h"

#include <algorithm>
#include <deque>
#include <map>
#include <optional>
#include <set>

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

// Bottom means an unresolved cyclic definition, not an arbitrary value. A
// later conflicting predecessor moves the fact to Top and invalidates every
// dependent relation before any address is rewritten.
struct AffineAddress {
  enum Kind { Bottom, Exact, Top } State = Bottom;
  llvm::Value *Root = nullptr;
  llvm::APInt Offset{1, 0};

  bool operator==(const AffineAddress &) const = default;

  static AffineAddress unknown() { return {Top, nullptr, llvm::APInt(1, 0)}; }
  static AffineAddress exact(llvm::Value *Root, llvm::APInt Offset) {
    return {Exact, Root, std::move(Offset)};
  }
};

AffineAddress join(AffineAddress A, AffineAddress B) {
  if (A.State == AffineAddress::Bottom)
    return B;
  if (B.State == AffineAddress::Bottom)
    return A;
  return A == B ? A : AffineAddress::unknown();
}

// A dominating masked equality can turn a bitwise address operation into an
// exact modular displacement. The proof belongs to the operation's execution
// point, not to all uses of the input or to a guessed entry alignment.
bool collectGuardedDisplacements(
    llvm::Function &F, const ByteMemoryForwardingOptions &Options,
    ByteMemoryForwardingResult &Result,
    llvm::DenseMap<llvm::Instruction *, llvm::APInt> &Deltas) {
  const auto Step = [&](uint64_t Count = 1) {
    return charge(Result.AddressSteps, Count, Options.MaxAddressSteps, Result);
  };
  struct Guard {
    llvm::BranchInst *Branch;
    unsigned Successor;
    llvm::APInt Mask;
    llvm::APInt Bits;
    uint64_t Predecessors;
  };
  const unsigned Width = F.getParent()->getDataLayout().getPointerSizeInBits(0);
  llvm::DenseMap<llvm::Value *, llvm::SmallVector<Guard, 2>> Guards;
  for (auto &BB : F) {
    if (!Step(1 + BB.getTerminator()->getNumSuccessors()))
      return false;
    auto *Branch = llvm::dyn_cast<llvm::BranchInst>(BB.getTerminator());
    if (!Branch || !Branch->isConditional() ||
        Branch->getSuccessor(0) == Branch->getSuccessor(1))
      continue;
    auto *Cmp = llvm::dyn_cast<llvm::ICmpInst>(Branch->getCondition());
    if (!Cmp || !Cmp->isEquality() ||
        !Cmp->getOperand(0)->getType()->isIntegerTy(Width))
      continue;
    auto *Expected = llvm::dyn_cast<llvm::ConstantInt>(Cmp->getOperand(1));
    auto *And = llvm::dyn_cast<llvm::BinaryOperator>(Cmp->getOperand(0));
    if (!Expected || !And || And->getOpcode() != llvm::Instruction::And)
      continue;
    auto *Mask = llvm::dyn_cast<llvm::ConstantInt>(And->getOperand(1));
    llvm::Value *Input = And->getOperand(0);
    if (!Mask) {
      Mask = llvm::dyn_cast<llvm::ConstantInt>(Input);
      Input = And->getOperand(1);
    }
    if (!Mask || !(Expected->getValue() & ~Mask->getValue()).isZero())
      continue;
    const unsigned Successor = Cmp->getPredicate() == llvm::ICmpInst::ICMP_NE;
    uint64_t Predecessors = 0;
    for (auto *Pred : llvm::predecessors(Branch->getSuccessor(Successor))) {
      (void)Pred;
      if (!Step())
        return false;
      ++Predecessors;
    }
    Guards[Input].push_back({Branch, Successor, Mask->getValue(),
                             Expected->getValue(), Predecessors});
  }
  if (Guards.empty())
    return true;
  // Graph vertices and edges were charged before constructing this analysis.
  llvm::DominatorTree DT(F);
  for (auto &BB : F)
    for (auto &I : BB) {
      if (!Step())
        return false;
      auto *Binary = llvm::dyn_cast<llvm::BinaryOperator>(&I);
      if (!Binary || !Binary->getType()->isIntegerTy(Width) ||
          !DT.isReachableFromEntry(&BB))
        continue;
      unsigned Op = Binary->getOpcode();
      if (Op != llvm::Instruction::And && Op != llvm::Instruction::Or &&
          Op != llvm::Instruction::Xor)
        continue;
      auto *K = llvm::dyn_cast<llvm::ConstantInt>(Binary->getOperand(1));
      llvm::Value *V = Binary->getOperand(0);
      if (!K) {
        K = llvm::dyn_cast<llvm::ConstantInt>(V);
        V = Binary->getOperand(1);
      }
      if (!K)
        continue;
      auto Found = Guards.find(V);
      if (Found == Guards.end())
        continue;
      const llvm::APInt Changed =
          Op == llvm::Instruction::And ? ~K->getValue() : K->getValue();
      llvm::APInt Known(Changed.getBitWidth(), 0), Bits(Known);
      for (const auto &G : Found->second) {
        // Edge dominance may inspect all predecessors of the selected target.
        if (!Step(1 + G.Predecessors))
          return false;
        if (!DT.dominates(
                llvm::BasicBlockEdge(G.Branch->getParent(),
                                     G.Branch->getSuccessor(G.Successor)),
                &BB))
          continue;
        if (!((Bits ^ G.Bits) & Known & G.Mask).isZero()) {
          Known.clearAllBits();
          break;
        }
        Known |= G.Mask;
        Bits |= G.Bits;
      }
      if (!(Changed & ~Known).isZero())
        continue;
      llvm::APInt Delta(Changed.getBitWidth(), 0);
      if (Op != llvm::Instruction::And)
        Delta += Changed & ~Bits;
      if (Op != llvm::Instruction::Or)
        Delta -= Changed & Bits;
      Deltas[&I] = std::move(Delta);
    }
  return true;
}

// Recover address algebra only. Memory contents never cross a block.
// In particular an entry relation alone cannot authorize a cyclic rewrite.
bool canonicalizeNumericAddresses(llvm::Function &F,
                                  const ByteMemoryForwardingOptions &Options,
                                  ByteMemoryForwardingResult &Result) {
  const auto &DL = F.getParent()->getDataLayout();
  const unsigned Width = DL.getPointerSizeInBits(0);
  if ((Width != 32 && Width != 64) || Width != DL.getIndexSizeInBits(0))
    return true;
  const auto Step = [&](uint64_t Count = 1) {
    return charge(Result.AddressSteps, Count, Options.MaxAddressSteps, Result);
  };
  const auto Eligible = [&](llvm::Type *T) {
    return T->isIntegerTy(Width) ||
           (T->isPointerTy() && !T->getPointerAddressSpace() &&
            !DL.isNonIntegralPointerType(T));
  };
  llvm::DenseMap<llvm::Instruction *, llvm::APInt> GuardedDeltas;
  if (!collectGuardedDisplacements(F, Options, Result, GuardedDeltas))
    return false;
  llvm::DenseMap<llvm::Value *, AffineAddress> Facts;
  std::deque<llvm::Instruction *> Work;
  llvm::SmallPtrSet<llvm::Instruction *, 32> Queued;
  const auto Enqueue = [&](llvm::Instruction *I) {
    if (Eligible(I->getType()) && Queued.insert(I).second)
      Work.push_back(I);
  };
  const auto Read = [&](llvm::Value *V) {
    if (!Eligible(V->getType()) || llvm::isa<llvm::UndefValue>(V))
      return AffineAddress::unknown();
    if (auto *C = llvm::dyn_cast<llvm::ConstantInt>(V))
      return AffineAddress::exact(nullptr, C->getValue());
    if (llvm::isa<llvm::Instruction>(V))
      return Facts.lookup(V);
    return AffineAddress::exact(V, llvm::APInt(Width, 0));
  };
  for (auto &BB : F)
    for (auto &I : BB) {
      if (!Step())
        return false;
      Enqueue(&I);
    }
  while (!Work.empty()) {
    if (!Step())
      return false;
    auto *I = Work.front();
    Work.pop_front();
    Queued.erase(I);
    AffineAddress Next = AffineAddress::exact(I, llvm::APInt(Width, 0));
    if (auto *Phi = llvm::dyn_cast<llvm::PHINode>(I)) {
      Next = {};
      for (llvm::Value *V : Phi->incoming_values()) {
        if (!Step())
          return false;
        Next = join(Next, Read(V));
      }
    } else if (auto *Select = llvm::dyn_cast<llvm::SelectInst>(I)) {
      if (!Step(2))
        return false;
      Next = join(Read(Select->getTrueValue()), Read(Select->getFalseValue()));
    } else if (llvm::isa<llvm::IntToPtrInst>(I)) {
      Next = Read(I->getOperand(0));
    } else if (auto *GEP = llvm::dyn_cast<llvm::GetElementPtrInst>(I)) {
      if (!Step(GEP->getNumIndices()))
        return false;
      llvm::APInt Delta(Width, 0);
      if (GEP->accumulateConstantOffset(DL, Delta)) {
        Next = Read(GEP->getPointerOperand());
        if (Next.State == AffineAddress::Exact)
          Next.Offset += Delta;
      }
    } else if (auto *Binary = llvm::dyn_cast<llvm::BinaryOperator>(I)) {
      auto *K = llvm::dyn_cast<llvm::ConstantInt>(Binary->getOperand(1));
      llvm::Value *V = Binary->getOperand(0);
      if (Binary->isCommutative() && !K) {
        K = llvm::dyn_cast<llvm::ConstantInt>(V);
        V = Binary->getOperand(1);
      }
      if (K && (Binary->getOpcode() == llvm::Instruction::Add ||
                Binary->getOpcode() == llvm::Instruction::Sub)) {
        Next = Read(V);
        if (Next.State == AffineAddress::Exact)
          Next.Offset += Binary->getOpcode() == llvm::Instruction::Add
                             ? K->getValue()
                             : -K->getValue();
      } else if (auto G = GuardedDeltas.find(I); G != GuardedDeltas.end()) {
        Next = Read(V);
        if (Next.State == AffineAddress::Exact)
          Next.Offset += G->second;
      }
    }
    // Casts losing bits, freeze and other operations retain their own root.
    // Do not expose optimistic facts until the entire worklist has converged.
    Next = join(Facts.lookup(I), std::move(Next));
    if (Next == Facts.lookup(I))
      continue;
    Facts[I] = std::move(Next);
    for (llvm::User *U : I->users()) {
      if (!Step())
        return false;
      if (auto *User = llvm::dyn_cast<llvm::Instruction>(U))
        Enqueue(User);
    }
  }
  for (auto &BB : F)
    for (auto &I : BB) {
      if (!Step())
        return false;
      auto *Load = llvm::dyn_cast<llvm::LoadInst>(&I);
      auto *Store = llvm::dyn_cast<llvm::StoreInst>(&I);
      if ((!Load && !Store) || (Load && !Load->isSimple()) ||
          (Store && !Store->isSimple()))
        continue;
      llvm::Value *P =
          Load ? Load->getPointerOperand() : Store->getPointerOperand();
      const auto A = Read(P);
      if (A.State != AffineAddress::Exact || !A.Root ||
          !A.Root->getType()->isIntegerTy(Width))
        continue;
      // An entry value is evaluated once before every reachable loop. Do not
      // equate separate dynamic executions of a loop-local opaque operation.
      const auto *RootInst = llvm::dyn_cast<llvm::Instruction>(A.Root);
      if (!llvm::isa<llvm::Argument>(A.Root) &&
          (!RootInst || RootInst->getParent() != &F.getEntryBlock()))
        continue;
      auto Existing = numericAddress(P, DL, Options, Result);
      if (Result.BudgetExhausted)
        return false;
      if (Existing && Existing->Root == A.Root && Existing->Offset == A.Offset)
        continue;
      if (!charge(Result.NewInstructions, 1 + !A.Offset.isZero(),
                  Options.MaxNewInstructions, Result))
        return false;
      llvm::IRBuilder<llvm::NoFolder> Builder(&I);
      llvm::Value *Address = A.Root;
      if (!A.Offset.isZero())
        Address = Builder.CreateAdd(
            Address, llvm::ConstantInt::get(Address->getType(), A.Offset),
            "numeric.address");
      Address = Builder.CreateIntToPtr(Address, P->getType());
      I.setOperand(Load ? 0 : 1, Address);
      ++Result.CanonicalizedAddresses;
    }
  return true;
}

bool forwardNumericMemory(llvm::Function &F,
                          const ByteMemoryForwardingOptions &Options,
                          ByteMemoryForwardingResult &Result) {
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
      if (!Store && !Load) {
        if (llvm::isa<llvm::CallBase, llvm::FenceInst>(I) || I->mayThrow() ||
            I->mayWriteToMemory())
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

// Analyze observations after forwarding, so retained reads (including an
// unsupported coercion) still protect the bytes they see. A reverse scan
// records bytes overwritten before any subsequent observation. This permits
// several smaller stores to kill one larger store without ever partially
// deleting a writer whose other bytes remain observable.
bool deleteNumericStores(llvm::Function &F,
                         const ByteMemoryForwardingOptions &Options,
                         ByteMemoryForwardingResult &Result) {
  const auto &DL = F.getParent()->getDataLayout();
  const auto Step = [&](uint64_t Count = 1) {
    return charge(Result.MemorySteps, Count, Options.MaxMemorySteps, Result);
  };
  for (auto &BB : F) {
    llvm::Value *Root = nullptr;
    // Offsets are modular bitvectors, including zero and all sentinel values.
    std::set<uint64_t> Overwritten;
    const auto Clear = [&] {
      Root = nullptr;
      Overwritten.clear();
    };
    for (auto It = BB.end(); It != BB.begin();) {
      llvm::Instruction *I = &*--It;
      if (!charge(Result.Instructions, 1, Options.MaxInstructions, Result))
        return false;
      auto *Store = llvm::dyn_cast<llvm::StoreInst>(I);
      auto *Load = llvm::dyn_cast<llvm::LoadInst>(I);
      if (!Store && !Load) {
        if (llvm::isa<llvm::CallBase, llvm::FenceInst>(I) || I->mayThrow() ||
            I->mayReadOrWriteMemory())
          Clear();
        continue;
      }
      auto Width = scalarBytes(Store ? Store->getValueOperand()->getType()
                                     : Load->getType());
      if (!Width || (Store && !Store->isSimple()) ||
          (Load && !Load->isSimple())) {
        Clear();
        continue;
      }
      auto A = numericAddress(Store ? Store->getPointerOperand()
                                    : Load->getPointerOperand(),
                              DL, Options, Result);
      if (Result.BudgetExhausted)
        return false;
      if (!A) {
        Clear();
        continue;
      }
      // Different SSA roots may alias. Neither the integer representation nor
      // a disjoint displacement proves independence from an unrelated root.
      if (Root != A->Root)
        Clear();
      Root = A->Root;
      const auto Offset = [&](unsigned Index) {
        return (A->Offset + Index).getZExtValue();
      };
      if (Load) {
        for (unsigned J = 0; J != *Width; ++J) {
          if (!Step())
            return false;
          Overwritten.erase(Offset(J));
        }
        continue;
      }
      bool Dead = true;
      for (unsigned J = 0; J != *Width; ++J) {
        if (!Step())
          return false;
        Dead &= Overwritten.contains(Offset(J));
      }
      if (Dead) {
        // No IR is changed until every byte of this writer has been checked.
        It = Store->eraseFromParent();
        ++Result.RemovedStores;
        continue;
      }
      for (unsigned J = 0; J != *Width; ++J) {
        if (!Step())
          return false;
        const uint64_t Key = Offset(J);
        if (!Overwritten.contains(Key) &&
            Overwritten.size() >= Options.MaxTrackedBytes) {
          Result.BudgetExhausted = true;
          return false;
        }
        Overwritten.insert(Key);
      }
      Result.PeakTrackedBytes =
          std::max(Result.PeakTrackedBytes, uint64_t(Overwritten.size()));
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
      canonicalizeNumericAddresses(F, Options, Result) &&
      forwardNumericMemory(F, Options, Result))
    deleteNumericStores(F, Options, Result);
  return Result;
}

llvm::PreservedAnalyses
ByteMemoryForwardingPass::run(llvm::Function &F,
                              llvm::FunctionAnalysisManager &) {
  const auto Result = forward(F, Options);
  return Result.ForwardedLoads || Result.RemovedStores ||
                 Result.CanonicalizedAddresses
             ? llvm::PreservedAnalyses::none()
             : llvm::PreservedAnalyses::all();
}

} // namespace neverd
