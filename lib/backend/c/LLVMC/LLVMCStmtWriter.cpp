//===- LLVMCStmtWriter.cpp - LLVM IR instruction rendering ----*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Instruction rendering for the LLVM IR C emitter: converts individual
/// LLVM instructions into C source lines.  Function-level orchestration
/// lives in LLVMCFuncWriter.cpp.
///
//===----------------------------------------------------------------------===//

#include "../UnalignedMemory.h"
#include "../render/X86FPStateHelpers.h"
#include "LLVMCIntegerMinMax.h"
#include "LLVMCScalarUnary.h"
#include "LLVMCWriter.h"

#include "neverd/Common.h"
#include "neverd/Limits.h"
#include "neverd/backend/llvm/LLVMName.h"
#include "neverd/backend/llvm/LLVMX86AddressSpaces.h"
#include "neverd/backend/llvm/LLVMX86FPStateAsm.h"
#include "neverd/backend/llvm/LLVMX86ShadowStackAsm.h"
#include "neverd/backend/llvm/LLVMX86X87StateAsm.h"
#include "neverd/ir/intrinsics/Intrinsics.h"
#include "neverd/loader/BinaryImage.h"

#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/Analysis/ValueTracking.h"
#include "llvm/IR/CFG.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/Intrinsics.h"
#include "llvm/IR/IntrinsicsAArch64.h"
#include "llvm/IR/Operator.h"
#include "llvm/Support/ErrorHandling.h"

#include <cctype>
#include <iterator>
#include <set>
#include <stdexcept>
#include <string_view>

namespace neverd {

namespace {
constexpr uint32_t kCxxCatchConst = 0x1u;
constexpr uint32_t kCxxCatchVolatile = 0x2u;
constexpr uint32_t kCxxCatchReference = 0x8u;
constexpr uint32_t kCxxCatchAll = 0x40u;

// LLVM's definedness query deliberately stops at short expression depths and
// at unconstrained loads. C nevertheless has a total bitvector projection for
// this small integer sublanguage. Using that value for freeze is a valid
// refinement: it equals the LLVM value when defined, and chooses one stable
// value when poison-producing flags would make the LLVM result poison. This
// does not introduce C undefined behavior on an LLVM-defined execution.
// Variable shifts, FP conversion and uninitialized PHI cycles remain
// unsupported.
bool freezeHasDefinedCProjection(const llvm::Value *Root) {
  std::vector<const llvm::Value *> Work{Root}, Anchors;
  std::set<const llvm::Value *> Seen, Defined;
  std::map<const llvm::Value *, std::vector<const llvm::Value *>> Users;
  size_t Budget = 65536;
  while (!Work.empty()) {
    const auto *Value = Work.back();
    Work.pop_back();
    if (!Budget--)
      return false;
    if (!Seen.insert(Value).second)
      continue;
    auto *Type = llvm::dyn_cast<llvm::IntegerType>(Value->getType());
    if (!Type || Type->getBitWidth() > 64 ||
        llvm::isa<llvm::UndefValue, llvm::PoisonValue>(Value))
      return false;
    if (llvm::isa<llvm::ConstantInt, llvm::Argument, llvm::FreezeInst>(Value)) {
      Anchors.push_back(Value);
      continue;
    }
    if (const auto *Load = llvm::dyn_cast<llvm::LoadInst>(Value)) {
      if (!Load->isSimple() || Load->getPointerAddressSpace() != 0)
        return false;
      // The existing memory projection materializes this access. In C an
      // integer byte representation contains no LLVM poison bit.
      Anchors.push_back(Value);
      continue;
    }
    const auto *Inst = llvm::dyn_cast<llvm::Instruction>(Value);
    if (!Inst)
      return false;
    if (Inst->getOpcode() == llvm::Instruction::SDiv ||
        Inst->getOpcode() == llvm::Instruction::UDiv ||
        Inst->getOpcode() == llvm::Instruction::SRem ||
        Inst->getOpcode() == llvm::Instruction::URem) {
      // Zero divisors and signed overflow are immediate LLVM UB, matching C.
      // A poison *operand*, however, may yield poison that an enclosing freeze
      // must define. Require LLVM-defined inputs, rather than reusing a
      // wrapping C refinement of an upstream poison-producing expression.
      // Dropping an `exact` flag then merely chooses the ordinary rounded
      // quotient.
      if (!llvm::all_of(Inst->operands(), [&](const llvm::Use &Operand) {
            return llvm::isGuaranteedNotToBeUndefOrPoison(Operand, nullptr,
                                                          Inst);
          }))
        return false;
      Anchors.push_back(Value);
      continue;
    }
    switch (Inst->getOpcode()) {
    case llvm::Instruction::PHI:
      if (!Inst->getNumOperands())
        return false;
      break;
    case llvm::Instruction::Add:
    case llvm::Instruction::Sub:
    case llvm::Instruction::Mul:
    case llvm::Instruction::And:
    case llvm::Instruction::Or:
    case llvm::Instruction::Xor:
    case llvm::Instruction::Trunc:
    case llvm::Instruction::ZExt:
    case llvm::Instruction::SExt:
    case llvm::Instruction::ICmp:
    case llvm::Instruction::Select:
      break;
    case llvm::Instruction::Shl:
    case llvm::Instruction::LShr:
    case llvm::Instruction::AShr: {
      const auto *Count =
          llvm::dyn_cast<llvm::ConstantInt>(Inst->getOperand(1));
      if (!Count || Count->getValue().uge(Type->getBitWidth()))
        return false;
      break;
    }
    default:
      return false;
    }
    for (const auto &Operand : Inst->operands()) {
      Users[Operand].push_back(Value);
      Work.push_back(Operand);
    }
  }
  while (!Anchors.empty()) {
    const auto *Value = Anchors.back();
    Anchors.pop_back();
    if (!Budget--)
      return false;
    if (Defined.insert(Value).second)
      if (auto It = Users.find(Value); It != Users.end())
        Anchors.insert(Anchors.end(), It->second.begin(), It->second.end());
  }
  return Defined.size() == Seen.size();
}

std::optional<std::string> peelWrappedNot(const std::string &S) {
  if (S.size() < 3 || S[0] != '!' || S[1] != '(' || S.back() != ')')
    return std::nullopt;
  int Depth = 0;
  for (size_t I = 1; I < S.size(); ++I) {
    if (S[I] == '(')
      ++Depth;
    else if (S[I] == ')') {
      --Depth;
      if (Depth == 0 && I + 1 != S.size())
        return std::nullopt;
    }
  }
  if (Depth != 0)
    return std::nullopt;
  return S.substr(2, S.size() - 3);
}

std::string orSkipConds(const std::string &A, const std::string &B) {
  const auto Left = peelWrappedNot(A);
  const auto Right = peelWrappedNot(B);
  if (Left && Right)
    return "!(" + *Left + " && " + *Right + ")";
  auto Group = [](const std::string &S) {
    if (S.empty() || S.front() == '(' || S.front() == '!')
      return S;
    // && binds tighter than ||. A comparison or call does not.
    if (S.find("&&") != std::string::npos)
      return "(" + S + ")";
    return S;
  };
  return Group(A) + " || " + Group(B);
}

bool isCIdentifier(llvm::StringRef Name) {
  if (Name.empty() ||
      (!std::isalpha(static_cast<unsigned char>(Name.front())) &&
       Name.front() != '_'))
    return false;
  return llvm::all_of(Name, [](char Ch) {
    return std::isalnum(static_cast<unsigned char>(Ch)) || Ch == '_';
  });
}

std::string cxxTypeFromDescriptor(const BinaryImage *Img, va_t DescriptorVA) {
  if (!Img || !DescriptorVA)
    return {};
  const size_t PointerSize = Img->is64Bit() ? 8 : 4;
  if (DescriptorVA > InvalidVA - 2 * PointerSize)
    return {};
  const va_t NameVA = DescriptorVA + 2 * PointerSize;
  std::string Name;
  for (size_t I = 0; I < limits::kMaxMsvcTypeDescriptorNameBytes; ++I) {
    if (I > InvalidVA - NameVA)
      return {};
    const uint8_t *Byte = Img->readVA(NameVA + I, 1);
    if (!Byte)
      return {};
    if (*Byte == 0)
      break;
    Name.push_back(static_cast<char>(*Byte));
  }
  llvm::StringRef Mangled(Name);
  if (Mangled.starts_with(".?A") && Mangled.size() > 4) {
    llvm::StringRef Rest = Mangled.drop_front(4);
    const size_t At = Rest.find('@');
    if (At != llvm::StringRef::npos)
      Rest = Rest.take_front(At);
    if (isCIdentifier(Rest))
      return Rest.str();
  }
  return Name;
}
} // namespace

void LLVMCWriter::emitIndent(int N) { emitCIndent(OS.stream(), N); }

const llvm::AllocaInst *
LLVMCWriter::asAllocaPointer(const llvm::Value *V) const {
  if (!V)
    return nullptr;
  const auto *Alloca = llvm::dyn_cast<llvm::AllocaInst>(V->stripPointerCasts());
  // Arrays are backing memory, not assignable scalar homes. In particular,
  // a scalar access at offset zero must not become an array assignment or a
  // cached value for the whole array.
  return Alloca && !Alloca->getAllocatedType()->isArrayTy() ? Alloca : nullptr;
}

bool LLVMCWriter::isThisFieldAddress(const llvm::Value *V) const {
  const auto Peeled = peelPointerOffset(V);
  return Peeled && DebugThisArg && Peeled->first == DebugThisArg &&
         Peeled->second != 0;
}

bool LLVMCWriter::isThisFieldValueHome(const llvm::AllocaInst *Slot) const {
  if (!Slot || !DebugThisArg)
    return false;
  auto Home = AllocaHomeValues.find(Slot);
  if (Home == AllocaHomeValues.end() || !Home->second)
    return false;
  const auto *LI = llvm::dyn_cast<llvm::LoadInst>(Home->second);
  if (!LI)
    return false;
  return isThisFieldAddress(LI->getPointerOperand());
}

bool LLVMCWriter::isFrameFieldValueHome(const llvm::AllocaInst *Slot) const {
  if (!Slot || !SyntheticFrame)
    return false;
  auto Home = AllocaHomeValues.find(Slot);
  if (Home == AllocaHomeValues.end() || !Home->second)
    return false;
  const auto *LI = llvm::dyn_cast<llvm::LoadInst>(Home->second);
  if (!LI)
    return false;
  const auto Peeled = peelPointerOffset(LI->getPointerOperand());
  return Peeled && Peeled->first == SyntheticFrame;
}

bool LLVMCWriter::isTypedRecordCursorSlot(const llvm::AllocaInst *Slot) const {
  if (!Slot || ThisHomes.count(Slot))
    return false;
  auto NamedCursor = [&] {
    auto Ty = AllocaTypes.find(Slot);
    return Ty != AllocaTypes.end() && Ty->second &&
           Ty->second->Kind == NdTypeKind::Ptr && Ty->second->Pointee &&
           Ty->second->Pointee->Kind == NdTypeKind::Struct &&
           !Ty->second->Pointee->SourceName.empty();
  };
  // The checks below only exclude named record cursors. Most homes have no
  // such type, and their field checks can recursively peel other homes.
  if (!NamedCursor())
    return false;
  // `isThisFieldAddress` peels. A cursor whose last home is `load(cursor+k)`
  // (`m_pNext`) would recurse peel → cursor → this-field → peel.
  static thread_local int Depth = 0;
  if (Depth)
    return true;
  struct DepthGuard {
    int &D;
    explicit DepthGuard(int &D) : D(D) { ++D; }
    ~DepthGuard() { --D; }
  } Guard(Depth);
  // A home that holds `this+imm` is the address of a this-field, not a
  // heap cursor.  Walking through it names `this->m_pGem.p`.  Stopping
  // treats the slot as `CGemBase*` and reprints `*(uint64_t*)t`.
  if (auto Home = AllocaHomeValues.find(Slot);
      Home != AllocaHomeValues.end() && isThisFieldAddress(Home->second))
    return false;
  if (isFrameFieldValueHome(Slot))
    return false;
  return true;
}

bool LLVMCWriter::isTypedRecordCursorValue(const llvm::Value *V) const {
  if (!V)
    return false;
  if (const auto *LI = llvm::dyn_cast<llvm::LoadInst>(V))
    return isTypedRecordCursorSlot(asAllocaPointer(LI->getPointerOperand()));
  if (const auto *AI = llvm::dyn_cast<llvm::AllocaInst>(V))
    return isTypedRecordCursorSlot(AI);
  return false;
}

bool LLVMCWriter::allocaHasLoad(const llvm::AllocaInst *Slot) const {
  if (!Slot)
    return false;
  for (const llvm::User *U : Slot->users())
    if (llvm::isa<llvm::LoadInst>(U))
      return true;
  return false;
}

bool LLVMCWriter::cursorSlotIsObserved(const llvm::AllocaInst *Slot) const {
  if (!Slot)
    return false;
  for (const llvm::User *U : Slot->users()) {
    const auto *LI = llvm::dyn_cast<llvm::LoadInst>(U);
    if (!LI)
      continue;
    for (const llvm::User *LU : LI->users()) {
      if (llvm::isa<llvm::ICmpInst, llvm::CondBrInst, llvm::UncondBrInst,
                    llvm::ReturnInst>(LU))
        return true;
      if (const auto *SI = llvm::dyn_cast<llvm::StoreInst>(LU);
          SI && SI->getValueOperand() == LI)
        continue;
      if (llvm::isa<llvm::BinaryOperator, llvm::IntToPtrInst,
                    llvm::GetElementPtrInst, llvm::LoadInst>(LU))
        return true;
    }
  }
  return false;
}

const llvm::Value *
LLVMCWriter::allocaStoredValueBefore(const llvm::LoadInst *Load,
                                     bool BoundExpansion) const {
  if (!Load || !Load->isSimple() || !Load->getFunction())
    return nullptr;
  const auto *Slot = asAllocaPointer(Load->getPointerOperand());
  if (!Slot || allocaAddressTaken(Slot))
    return nullptr;
  // Printing a stored expression may revisit a load from before that store.
  // The current printer state is not reaching-definition evidence for it.
  // Index exact local stores once. Cross-block loads need the separate
  // reaching-definition proof below.
  if (LocalLoadValuesFor != Load->getFunction()) {
    LocalLoadValuesFor = Load->getFunction();
    LocalLoadValues.clear();
    LocalReachingValues.clear();
    ExactLocalLoadSlots.clear();
    for (const auto &B : *LocalLoadValuesFor) {
      llvm::DenseMap<const llvm::AllocaInst *, const llvm::Value *> Stores;
      llvm::DenseMap<const llvm::Value *, unsigned> ExpansionDepth;
      llvm::DenseMap<const llvm::Value *, unsigned> ExpansionCost;
      constexpr unsigned MaxForwardDepth = 8;
      constexpr unsigned MaxForwardCost = 32;
      auto Expansion = [&](const llvm::Value *V) {
        // A definition outside this local index may itself inline a large
        // expression. Do not silently price an unmeasured instruction as a
        // leaf, including when basic blocks appear out of dominance order.
        if (llvm::isa<llvm::Instruction>(V) && !ExpansionCost.count(V))
          return std::pair(MaxForwardDepth, MaxForwardCost);
        return std::pair(ExpansionDepth.lookup(V),
                         std::max(1u, ExpansionCost.lookup(V)));
      };
      for (const auto &I : B) {
        unsigned Depth = 0, Cost = 1;
        for (const auto &Operand : I.operands()) {
          const auto [OperandDepth, OperandCost] = Expansion(Operand.get());
          Depth = std::max(Depth, OperandDepth);
          // Count duplicate operands twice: printed expressions are trees.
          Cost = std::min(MaxForwardCost, Cost + OperandCost);
        }
        if (!I.getType()->isVoidTy()) {
          ExpansionDepth[&I] = std::min(MaxForwardDepth, Depth + 1);
          ExpansionCost[&I] = Cost;
        }
        if (const auto *S = llvm::dyn_cast<llvm::StoreInst>(&I)) {
          if (const auto *A = asAllocaPointer(S->getPointerOperand()))
            Stores[A] = S->isSimple() ? S->getValueOperand() : nullptr;
        } else if (const auto *L = llvm::dyn_cast<llvm::LoadInst>(&I)) {
          // Long chains of mutable updates must not become deeply nested C
          // expressions or repeated recursive folding. A retained load is a
          // real snapshot and a fresh expression boundary.
          ExpansionDepth[L] = 0;
          ExpansionCost[L] = 1;
          const auto *A = asAllocaPointer(L->getPointerOperand());
          auto It = Stores.find(A);
          if (It != Stores.end() && It->second &&
              It->second->getType() == L->getType())
            LocalReachingValues[L] = It->second;
          if (It != Stores.end() && It->second &&
              It->second->getType() == L->getType() &&
              Expansion(It->second).first < MaxForwardDepth &&
              Expansion(It->second).second < MaxForwardCost) {
            LocalLoadValues[L] = It->second;
            ExpansionDepth[L] = ExpansionDepth.lookup(It->second) + 1;
            ExpansionCost[L] = ExpansionCost.lookup(It->second) + 1;
          }
        }
      }
    }
  }
  auto Exact = ExactLocalLoadSlots.find(Slot);
  if (Exact == ExactLocalLoadSlots.end()) {
    bool Valid = Slot->getAllocatedType()->isIntegerTy() ||
                 Slot->getAllocatedType()->isPointerTy();
    size_t Budget = 8 * 1024 * 1024;
    for (const auto *U : Slot->users()) {
      if (!Budget-- || !Valid) {
        Valid = false;
        break;
      }
      if (const auto *L = llvm::dyn_cast<llvm::LoadInst>(U))
        Valid &= L->isSimple() && L->getPointerOperand() == Slot &&
                 L->getType() == Slot->getAllocatedType();
      else if (const auto *S = llvm::dyn_cast<llvm::StoreInst>(U))
        Valid &= S->isSimple() && S->getPointerOperand() == Slot &&
                 S->getValueOperand() != Slot &&
                 S->getValueOperand()->getType() == Slot->getAllocatedType();
      else
        Valid = false;
    }
    Exact = ExactLocalLoadSlots.insert({Slot, Valid}).first;
  }
  if (!Exact->second)
    return nullptr;
  const auto &Definitions =
      BoundExpansion ? LocalLoadValues : LocalReachingValues;
  auto It = Definitions.find(Load);
  if (It != Definitions.end())
    return It->second;
  if (InitializedAllocaLoads.count(Load))
    return CrossBlockLoadValues.lookup(Load);
  if (!Dominators.isReachableFromEntry(Load->getParent()))
    return nullptr;
  // A cross-block replacement needs the same reaching value on every path.
  // Query immutable stores, never the state left by printing another block.
  const llvm::Value *Common = nullptr;
  bool Conflicting = false;
  unsigned Budget = 4096;
  auto FindStore = [&](const llvm::Instruction *End) {
    for (const auto *I = End; I; I = I->getPrevNode()) {
      if (Budget == 0)
        return false;
      --Budget;
      if (const auto *Store = llvm::dyn_cast<llvm::StoreInst>(I);
          Store && Store->getPointerOperand() == Slot) {
        const auto *Value = Store->getValueOperand();
        Conflicting |= Common && Common != Value;
        Common = Value;
        return true;
      }
    }
    return false;
  };
  bool Initialized = true;
  if (!FindStore(Load->getPrevNode())) {
    llvm::SmallVector<const llvm::BasicBlock *, 16> Pending;
    llvm::SmallPtrSet<const llvm::BasicBlock *, 32> Seen;
    if (llvm::pred_empty(Load->getParent()))
      Initialized = false;
    else
      llvm::append_range(Pending, llvm::predecessors(Load->getParent()));
    while (Initialized && !Pending.empty()) {
      const auto *Predecessor = Pending.pop_back_val();
      if (!Seen.insert(Predecessor).second)
        continue;
      if (FindStore(Predecessor->getTerminator()))
        continue;
      if (Budget == 0 || llvm::pred_empty(Predecessor))
        Initialized = false;
      else
        llvm::append_range(Pending, llvm::predecessors(Predecessor));
    }
  }
  Initialized &= Budget != 0;
  InitializedAllocaLoads[Load] = Initialized;
  if (!Initialized || Conflicting || !Common)
    return nullptr;
  // Reprinting a memory read or call would move its observation past effects.
  // Admit only bounded, immutable arithmetic/address expressions here.
  unsigned Work = 32;
  auto Stable = [&](auto &&Self, const llvm::Value *Value,
                    unsigned Depth) -> bool {
    if (Work == 0 || Depth > 8)
      return false;
    --Work;
    if (llvm::isa<llvm::Constant, llvm::Argument, llvm::AllocaInst>(Value))
      return true;
    const auto *Instruction = llvm::dyn_cast<llvm::Instruction>(Value);
    if (!Instruction ||
        !llvm::isa<llvm::BinaryOperator, llvm::CastInst,
                   llvm::GetElementPtrInst, llvm::ICmpInst>(Instruction))
      return false;
    return llvm::all_of(Instruction->operands(), [&](const llvm::Use &Operand) {
      return Self(Self, Operand.get(), Depth + 1);
    });
  };
  if (!Stable(Stable, Common, 0))
    return nullptr;
  CrossBlockLoadValues[Load] = Common;
  return Common;
}

void LLVMCWriter::noteAllocaStore(const llvm::AllocaInst *Slot,
                                  const llvm::Value *Stored) {
  if (!Slot || !Stored)
    return;
  AllocaLastValues[Slot] = Stored;
  if (auto Imm = foldImmediate(Stored))
    AllocaImmediates[Slot] = *Imm;
  else
    AllocaImmediates.erase(Slot);
  if (DebugThisArg) {
    if (auto Peeled = peelPointerOffset(Stored);
        Peeled && Peeled->first == DebugThisArg && Peeled->second == 0)
      ThisHomes.insert(Slot);
    else
      ThisHomes.erase(Slot);
  }
  if (TypeRef Ty = typeOfValue(Stored)) {
    AllocaTypes[Slot] = Ty;
    if (auto Text = ValueTexts.find(Stored); Text != ValueTexts.end())
      AllocaTexts[Slot] = Text->second;
    else if (!(Ty->Kind == NdTypeKind::Ptr && Ty->Pointee &&
               Ty->Pointee->Kind == NdTypeKind::Ptr))
      AllocaTexts.erase(Slot);
  } else if (auto It = AllocaTypes.find(Slot);
             It == AllocaTypes.end() || !It->second ||
             It->second->Kind != NdTypeKind::Ptr || !It->second->Pointee ||
             It->second->Pointee->Kind != NdTypeKind::Struct ||
             It->second->Pointee->SourceName.empty() ||
             It->second->Pointee->IsEnum) {
    AllocaTypes.erase(Slot);
    AllocaTexts.erase(Slot);
  }
}

bool LLVMCWriter::allocaLoadHasReachingStore(const llvm::LoadInst *Load) const {
  if (!Load || !Load->isSimple())
    return false;
  if (auto It = InitializedAllocaLoads.find(Load);
      It != InitializedAllocaLoads.end())
    return It->second;
  if (allocaStoredValueBefore(Load))
    return true;
  return InitializedAllocaLoads.lookup(Load);
}

bool LLVMCWriter::computedAllocaLoadIsForwarded(
    const llvm::LoadInst *LI) const {
  if (!LI)
    return false;
  const llvm::AllocaInst *Slot = asAllocaPointer(LI->getPointerOperand());
  if (!Slot || allocaAddressTaken(Slot) || joinFieldDefaultText(Slot))
    return false;
  if (const auto *Stored = allocaStoredValueBefore(LI))
    return !foldImmediate(Stored);
  return false;
}

std::optional<uint64_t>
LLVMCWriter::syntheticFrameOwnerOff(const llvm::Value *V) const {
  std::set<const llvm::Value *> Seen;
  while (V && Seen.insert(V).second) {
    if (const auto Peeled = peelPointerOffset(V);
        Peeled && Peeled->first == SyntheticFrame) {
      auto It = FrameSlots.find(Peeled->second);
      if (It != FrameSlots.end())
        return It->first;
      for (const auto &Slot : FrameSlots) {
        uint16_t Size = Slot.second.Type && Slot.second.Type->Size
                            ? Slot.second.Type->Size
                            : 0;
        if (!Size && Slot.second.CallType)
          Size = Slot.second.CallType->Size;
        if (Size < 8 || Slot.first > Peeled->second ||
            Peeled->second >= Slot.first + static_cast<uint64_t>(Size))
          continue;
        return Slot.first;
      }
      return Peeled->second;
    }
    if (const auto *LI = llvm::dyn_cast<llvm::LoadInst>(V)) {
      if (const auto Peeled = peelPointerOffset(LI->getPointerOperand());
          Peeled && Peeled->first == SyntheticFrame) {
        V = LI->getPointerOperand();
        continue;
      }
      if (const llvm::AllocaInst *Home =
              asAllocaPointer(LI->getPointerOperand())) {
        if (auto It = AllocaHomeValues.find(Home);
            It != AllocaHomeValues.end() && It->second && It->second != V) {
          V = It->second;
          continue;
        }
        if (const llvm::Value *Prev = allocaStoredValueBefore(LI);
            Prev && Prev != V) {
          V = Prev;
          continue;
        }
      }
      V = LI->getPointerOperand();
      continue;
    }
    if (const auto *Cast = llvm::dyn_cast<llvm::CastInst>(V)) {
      V = Cast->getOperand(0);
      continue;
    }
    return std::nullopt;
  }
  return std::nullopt;
}

bool LLVMCWriter::slotIsArgList(uint64_t Off) const {
  auto It = FrameSlots.find(Off);
  if (It == FrameSlots.end())
    return false;
  auto NamedArgList = [](const TypeRef &Ty) {
    if (!Ty || Ty->Kind != NdTypeKind::Struct || Ty->IsEnum ||
        Ty->SourceName.empty())
      return false;
    if (Ty->SourceName == "ArgList")
      return true;
    if (Ty->FieldDisplayOffsets.size() != Ty->FieldDisplayNames.size())
      return false;
    bool Types = false;
    bool Values = false;
    for (size_t I = 0; I < Ty->FieldDisplayOffsets.size(); ++I) {
      if (Ty->FieldDisplayOffsets[I] == 0 &&
          Ty->FieldDisplayNames[I] == "types_")
        Types = true;
      if (Ty->FieldDisplayOffsets[I] == 8 &&
          Ty->FieldDisplayNames[I] == "values_")
        Values = true;
    }
    return Types && Values;
  };
  return NamedArgList(It->second.CallType) || NamedArgList(It->second.Type);
}

bool LLVMCWriter::isLeftoverArgListSibling(uint64_t Off) const {
  auto It = FrameSlots.find(Off);
  if (It == FrameSlots.end() || It->second.AddressTaken || !slotIsArgList(Off))
    return false;
  for (const auto &Slot : FrameSlots) {
    if (Slot.first >= Off || !Slot.second.AddressTaken ||
        !slotIsArgList(Slot.first))
      continue;
    uint16_t Size =
        Slot.second.Type && Slot.second.Type->Size ? Slot.second.Type->Size : 0;
    if (!Size && Slot.second.CallType)
      Size = Slot.second.CallType->Size;
    if (Size < 8)
      Size = 16;
    if (Slot.first + static_cast<uint64_t>(Size) == Off)
      return true;
  }
  return false;
}

const llvm::CallBase *
LLVMCWriter::leftoverPackedCallResult(const llvm::BasicBlock *BB) const {
  if (!BB)
    return nullptr;
  auto IntegerCall = [&](const llvm::Value *V) -> const llvm::CallBase * {
    std::set<const llvm::Value *> Seen;
    while (V && Seen.insert(V).second) {
      if (const auto *Cast = llvm::dyn_cast<llvm::CastInst>(V)) {
        if (llvm::isa<llvm::ZExtInst, llvm::SExtInst, llvm::TruncInst>(Cast)) {
          V = Cast->getOperand(0);
          continue;
        }
      }
      if (const auto *Fr = llvm::dyn_cast<llvm::FreezeInst>(V)) {
        V = Fr->getOperand(0);
        continue;
      }
      if (const auto *CB = llvm::dyn_cast<llvm::CallBase>(V)) {
        llvm::Type *Ty = CB->getType();
        if (Ty && Ty->isIntegerTy() && !Ty->isIntegerTy(1))
          return CB;
        return nullptr;
      }
      if (const auto *LI = llvm::dyn_cast<llvm::LoadInst>(V)) {
        if (const llvm::AllocaInst *Home =
                asAllocaPointer(LI->getPointerOperand())) {
          const llvm::Value *Best = nullptr;
          if (const llvm::BasicBlock *LoadBB = LI->getParent()) {
            for (const llvm::Instruction &Inst : *LoadBB) {
              if (&Inst == LI)
                break;
              const auto *St = llvm::dyn_cast<llvm::StoreInst>(&Inst);
              if (St && asAllocaPointer(St->getPointerOperand()) == Home)
                Best = St->getValueOperand();
            }
          }
          if (Best && Best != V) {
            V = Best;
            continue;
          }
          if (auto It = AllocaHomeValues.find(Home);
              It != AllocaHomeValues.end() && It->second && It->second != V) {
            V = It->second;
            continue;
          }
          if (const llvm::Value *Prev = allocaStoredValueBefore(LI);
              Prev && Prev != V) {
            V = Prev;
            continue;
          }
        }
      }
      return nullptr;
    }
    return nullptr;
  };
  llvm::SmallPtrSet<const llvm::BasicBlock *, 16> Seen;
  const llvm::BasicBlock *Cur = BB;
  auto uniqueNormalPred =
      [](const llvm::BasicBlock *Block) -> const llvm::BasicBlock * {
    const llvm::BasicBlock *Found = nullptr;
    for (const llvm::BasicBlock *P : llvm::predecessors(Block)) {
      if (P->isEHPad())
        continue;
      if (const auto *II =
              llvm::dyn_cast<llvm::InvokeInst>(P->getTerminator())) {
        if (II->getUnwindDest() == Block)
          continue;
      }
      if (Found)
        return nullptr;
      Found = P;
    }
    return Found;
  };
  for (unsigned I = 0; I < 12 && Cur && Seen.insert(Cur).second; ++I) {
    const llvm::BasicBlock *Pred = uniqueNormalPred(Cur);
    if (!Pred)
      break;
    const llvm::Instruction *Term = Pred->getTerminator();
    const llvm::Value *Cond = nullptr;
    const llvm::BasicBlock *Succ0 = nullptr;
    const llvm::BasicBlock *Succ1 = nullptr;
    if (const auto *CBI = llvm::dyn_cast<llvm::CondBrInst>(Term)) {
      Cond = CBI->getCondition();
      Succ0 = CBI->getSuccessor(0);
      Succ1 = CBI->getSuccessor(1);
    } else if (Term && Term->getNumSuccessors() == 2 &&
               llvm::StringRef(Term->getOpcodeName()) == "br") {
      Cond = Term->getOperand(0);
      Succ0 = Term->getSuccessor(0);
      Succ1 = Term->getSuccessor(1);
    }
    if (Cond && Succ0 && Succ1) {
      std::set<const llvm::Value *> Peel;
      while (Cond && Peel.insert(Cond).second) {
        if (const auto *Fr = llvm::dyn_cast<llvm::FreezeInst>(Cond)) {
          Cond = Fr->getOperand(0);
          continue;
        }
        if (const auto *Cast = llvm::dyn_cast<llvm::CastInst>(Cond)) {
          if (llvm::isa<llvm::ZExtInst, llvm::SExtInst, llvm::TruncInst>(
                  Cast) &&
              Cast->getType()->isIntegerTy(1)) {
            Cond = Cast->getOperand(0);
            continue;
          }
        }
        break;
      }
      const llvm::Value *Tested = nullptr;
      if (const auto *Cmp = llvm::dyn_cast<llvm::ICmpInst>(Cond)) {
        if (Cmp->isEquality()) {
          const llvm::Value *LHS = Cmp->getOperand(0);
          const llvm::Value *RHS = Cmp->getOperand(1);
          if (const auto *CI = llvm::dyn_cast<llvm::ConstantInt>(RHS);
              CI && CI->isZero())
            Tested = LHS;
          else if (const auto *CI = llvm::dyn_cast<llvm::ConstantInt>(LHS);
                   CI && CI->isZero())
            Tested = RHS;
        }
      }
      if (Tested) {
        const llvm::CallBase *CB = IntegerCall(Tested);
        if (!CB) {
          for (const llvm::Instruction &Inst : *Pred) {
            const auto *Cand = llvm::dyn_cast<llvm::CallBase>(&Inst);
            if (!Cand)
              continue;
            llvm::Type *Ty = Cand->getType();
            if (Ty && Ty->isIntegerTy() && !Ty->isIntegerTy(1))
              CB = Cand;
          }
        }
        if (CB) {
          auto leadsToBB = [&](const llvm::BasicBlock *From) {
            llvm::SmallPtrSet<const llvm::BasicBlock *, 16> Lead;
            while (From && Lead.insert(From).second) {
              if (From == BB)
                return true;
              const llvm::Instruction *LeadTerm = From->getTerminator();
              if (!LeadTerm)
                return false;
              if (const auto *II = llvm::dyn_cast<llvm::InvokeInst>(LeadTerm)) {
                From = II->getNormalDest();
                continue;
              }
              if (LeadTerm->getNumSuccessors() != 1)
                return false;
              From = LeadTerm->getSuccessor(0);
            }
            return false;
          };
          if (leadsToBB(Succ0) || leadsToBB(Succ1))
            return CB;
        }
      }
    }
    Cur = Pred;
  }
  return nullptr;
}

bool LLVMCWriter::peelsToUnknown(const llvm::Value *V) const {
  std::set<const llvm::Value *> Seen;
  while (V && Seen.insert(V).second) {
    if (isUnknownPlaceholder(V))
      return true;
    if (const auto *Cast = llvm::dyn_cast<llvm::CastInst>(V)) {
      V = Cast->getOperand(0);
      continue;
    }
    if (const auto *Fr = llvm::dyn_cast<llvm::FreezeInst>(V)) {
      V = Fr->getOperand(0);
      continue;
    }
    if (const auto *LI = llvm::dyn_cast<llvm::LoadInst>(V)) {
      if (const llvm::AllocaInst *Home =
              asAllocaPointer(LI->getPointerOperand())) {
        if (auto It = AllocaHomeValues.find(Home);
            It != AllocaHomeValues.end() && It->second && It->second != V) {
          V = It->second;
          continue;
        }
        if (const llvm::Value *Prev = allocaStoredValueBefore(LI);
            Prev && Prev != V) {
          V = Prev;
          continue;
        }
      }
    }
    return false;
  }
  return false;
}

bool LLVMCWriter::priorUnknownStoreTo(const llvm::Instruction *SI,
                                      uint64_t Off) const {
  if (!SI)
    return false;
  unsigned Looked = 0;
  for (const llvm::Instruction *Prev = SI->getPrevNode(); Prev && Looked < 12;
       Prev = Prev->getPrevNode()) {
    const auto *St = llvm::dyn_cast<llvm::StoreInst>(Prev);
    if (!St) {
      if (llvm::isa<llvm::AllocaInst, llvm::GetElementPtrInst, llvm::CastInst,
                    llvm::FreezeInst, llvm::PHINode, llvm::LoadInst,
                    llvm::CallBase>(Prev))
        continue;
      ++Looked;
      continue;
    }
    ++Looked;
    if (peelsToUnknown(St->getValueOperand())) {
      if (auto Src = syntheticFrameOwnerOff(St->getPointerOperand());
          Src && *Src == Off)
        return true;
    }
    if (llvmAccessSize(St->getValueOperand()->getType()) >= 16) {
      if (auto Src = syntheticFrameOwnerOff(St->getValueOperand());
          Src && *Src == Off)
        continue;
    }
  }
  return false;
}

const llvm::CallBase *
LLVMCWriter::leftoverPackedSiblingFill(const llvm::StoreInst &SI) const {
  const llvm::CallBase *Call = leftoverPackedCallResult(SI.getParent());
  if (!Call)
    return nullptr;
  const llvm::Value *Stored = SI.getValueOperand();
  const uint16_t Size = llvmAccessSize(Stored->getType());
  auto DestOff = syntheticFrameOwnerOff(SI.getPointerOperand());
  if (!DestOff || !isLeftoverArgListSibling(*DestOff))
    return nullptr;
  if (isUnknownPlaceholder(Stored) || peelsToUnknown(Stored))
    return Call;
  if (Size < 16)
    return nullptr;
  auto SrcOff = syntheticFrameOwnerOff(Stored);
  if (!SrcOff || *SrcOff == *DestOff)
    return nullptr;
  if (!priorUnknownStoreTo(&SI, *SrcOff))
    return nullptr;
  return Call;
}

bool LLVMCWriter::storeIsLeftoverHomeUndef(const llvm::StoreInst &SI) const {
  if (!peelsToUnknown(SI.getValueOperand()))
    return false;
  if (!leftoverPackedCallResult(SI.getParent()))
    return false;
  auto Home = syntheticFrameOwnerOff(SI.getPointerOperand());
  if (!Home)
    return false;
  unsigned Looked = 0;
  for (const llvm::Instruction *Next = SI.getNextNode(); Next && Looked < 12;
       Next = Next->getNextNode()) {
    const auto *St = llvm::dyn_cast<llvm::StoreInst>(Next);
    if (!St) {
      if (llvm::isa<llvm::AllocaInst, llvm::GetElementPtrInst, llvm::CastInst,
                    llvm::FreezeInst, llvm::PHINode, llvm::LoadInst,
                    llvm::CallBase>(Next))
        continue;
      ++Looked;
      continue;
    }
    ++Looked;
    if (leftoverPackedSiblingFill(*St))
      return true;
  }
  return false;
}

const llvm::CallBase *
LLVMCWriter::integerCallValue(const llvm::Value *V) const {
  std::set<const llvm::Value *> Seen;
  while (V && Seen.insert(V).second) {
    if (const auto *Cast = llvm::dyn_cast<llvm::CastInst>(V)) {
      if (llvm::isa<llvm::ZExtInst, llvm::SExtInst, llvm::TruncInst>(Cast)) {
        V = Cast->getOperand(0);
        continue;
      }
    }
    if (const auto *Fr = llvm::dyn_cast<llvm::FreezeInst>(V)) {
      V = Fr->getOperand(0);
      continue;
    }
    if (const auto *CB = llvm::dyn_cast<llvm::CallBase>(V)) {
      llvm::Type *Ty = CB->getType();
      if (Ty && Ty->isIntegerTy() && !Ty->isIntegerTy(1))
        return CB;
      return nullptr;
    }
    if (auto Fwd = Analysis.ForwardedLoads.find(V);
        Fwd != Analysis.ForwardedLoads.end() && Fwd->second &&
        Fwd->second != V) {
      V = Fwd->second;
      continue;
    }
    if (const auto *LI = llvm::dyn_cast<llvm::LoadInst>(V)) {
      if (const llvm::AllocaInst *Home =
              asAllocaPointer(LI->getPointerOperand())) {
        const llvm::Value *Best = nullptr;
        if (const llvm::BasicBlock *LoadBB = LI->getParent()) {
          for (const llvm::Instruction &Inst : *LoadBB) {
            if (&Inst == LI)
              break;
            const auto *St = llvm::dyn_cast<llvm::StoreInst>(&Inst);
            if (St && asAllocaPointer(St->getPointerOperand()) == Home)
              Best = St->getValueOperand();
          }
        }
        if (Best && Best != V) {
          V = Best;
          continue;
        }
        if (const llvm::Value *Prev = allocaStoredValueBefore(LI);
            Prev && Prev != V) {
          V = Prev;
          continue;
        }
        if (auto It = AllocaHomeValues.find(Home);
            It != AllocaHomeValues.end() && It->second && It->second != V) {
          V = It->second;
          continue;
        }
      }
    }
    return nullptr;
  }
  return nullptr;
}

void LLVMCWriter::collectLeftoverNarrowCallStores(llvm::Function &Fn) {
  OmittedLeftoverNarrow.clear();
  LeftoverSiblingFill.clear();
  if (!SyntheticFrame)
    return;
  const auto SavedLast = AllocaLastValues;
  AllocaLastValues.clear();
  for (llvm::BasicBlock &BB : Fn) {
    for (llvm::Instruction &Inst : BB) {
      const auto *SI = llvm::dyn_cast<llvm::StoreInst>(&Inst);
      if (!SI)
        continue;
      if (const llvm::AllocaInst *Slot =
              asAllocaPointer(SI->getPointerOperand())) {
        AllocaLastValues[Slot] = SI->getValueOperand();
        continue;
      }
      const uint16_t Narrow = llvmAccessSize(SI->getValueOperand()->getType());
      if (!Narrow || Narrow >= 16)
        continue;
      const llvm::CallBase *Call = integerCallValue(SI->getValueOperand());
      if (!Call || leftoverPackedCallResult(SI->getParent()) != Call)
        continue;
      auto DestOff = syntheticFrameOwnerOff(SI->getPointerOperand());
      if (!DestOff)
        continue;
      auto Snap = AllocaLastValues;
      unsigned Looked = 0;
      const llvm::StoreInst *Sibling = nullptr;
      const llvm::StoreInst *ArgListCopy = nullptr;
      const uint16_t FillSize = llvmAccessSize(Call->getType());
      for (llvm::Instruction *Next = Inst.getNextNode(); Next && Looked < 32;
           Next = Next->getNextNode()) {
        const auto *St = llvm::dyn_cast<llvm::StoreInst>(Next);
        if (!St)
          continue;
        if (const llvm::AllocaInst *Slot =
                asAllocaPointer(St->getPointerOperand())) {
          AllocaLastValues[Slot] = St->getValueOperand();
          continue;
        }
        ++Looked;
        auto EarlyOff = syntheticFrameOwnerOff(St->getPointerOperand());
        if (EarlyOff && *EarlyOff == *DestOff &&
            llvmAccessSize(St->getValueOperand()->getType()) < 16)
          break;
        if (llvmAccessSize(St->getValueOperand()->getType()) < 16)
          continue;
        auto SrcOff = syntheticFrameOwnerOff(St->getValueOperand());
        auto WideOff = syntheticFrameOwnerOff(St->getPointerOperand());
        if (!SrcOff || !WideOff || *SrcOff != *DestOff || *WideOff == *DestOff)
          continue;
        auto Member =
            frameSlotAccess(St->getPointerOperand(), FillSize ? FillSize : 4,
                            /*AddressOf=*/false,
                            /*Synthesize=*/false,
                            /*Overlay=*/true);
        if (!Member || !(llvm::StringRef(Member->Text).ends_with(".types_") ||
                         llvm::StringRef(Member->Text).ends_with("->types_")))
          continue;
        if (!ArgListCopy)
          ArgListCopy = St;
        // An adjacent leftover sibling wins over a farther ArgList copy.
        if (isLeftoverArgListSibling(*WideOff)) {
          Sibling = St;
          break;
        }
      }
      AllocaLastValues = std::move(Snap);
      const llvm::StoreInst *Wide = Sibling ? Sibling : ArgListCopy;
      if (!Wide)
        continue;
      OmittedLeftoverNarrow.insert(SI);
      LeftoverSiblingFill[Wide] = Call;
    }
  }
  AllocaLastValues = SavedLast;
}

const llvm::BasicBlock *
LLVMCWriter::impliedPointerSuccessor(const llvm::CondBrInst *Br) const {
  if (!Br)
    return nullptr;
  struct Key {
    const llvm::Value *Base = nullptr;
    uint64_t Off = 0;
    uint64_t Bits = 0;
    bool operator==(const Key &O) const {
      return Base == O.Base && Off == O.Off && Bits == O.Bits;
    }
  };
  auto KeyOfPtr = [&](const llvm::Value *Ptr) -> std::optional<Key> {
    if (!Ptr)
      return std::nullopt;
    if (const auto Peeled = peelPointerOffset(Ptr))
      return Key{Peeled->first, Peeled->second};
    if (const llvm::AllocaInst *Slot = asAllocaPointer(Ptr))
      return Key{Slot, 0};
    return std::nullopt;
  };
  struct Fact {
    Key K;
    bool TrueMeansNonZero = false;
    const llvm::LoadInst *Observation = nullptr;
  };
  auto LastStored = [&](const llvm::LoadInst *Load) -> const llvm::Value * {
    const llvm::AllocaInst *Slot = nullptr;
    if (Load)
      Slot = llvm::dyn_cast<llvm::AllocaInst>(
          Load->getPointerOperand()->stripPointerCasts());
    if (!Load || !Load->isSimple() || !Slot || allocaAddressTaken(Slot))
      return nullptr;
    const llvm::Value *Last = nullptr;
    for (const llvm::Instruction &Inst : *Load->getParent()) {
      if (&Inst == Load)
        break;
      const auto *SI = llvm::dyn_cast<llvm::StoreInst>(&Inst);
      if (!SI)
        continue;
      if (llvm::dyn_cast<llvm::AllocaInst>(
              SI->getPointerOperand()->stripPointerCasts()) == Slot)
        Last = SI->isSimple() ? SI->getValueOperand() : nullptr;
    }
    return Last;
  };
  auto Canonical =
      [&](const llvm::LoadInst *Load,
          const llvm::LoadInst *&Observation) -> std::optional<Key> {
    Observation = Load;
    if (!Load || !Load->isSimple())
      return std::nullopt;
    std::optional<Key> Own = KeyOfPtr(Load->getPointerOperand());
    if (!Own)
      return std::nullopt;
    Own->Bits = Load->getType()->getScalarSizeInBits();
    const llvm::Value *Stored = LastStored(Load);
    while (const auto *Cast = llvm::dyn_cast_or_null<llvm::CastInst>(Stored)) {
      if (!llvm::isa<llvm::ZExtInst, llvm::SExtInst>(Cast))
        break;
      Stored = Cast->getOperand(0);
    }
    if (const auto *Src = llvm::dyn_cast_or_null<llvm::LoadInst>(Stored)) {
      if (Src->isSimple())
        if (std::optional<Key> Inner = KeyOfPtr(Src->getPointerOperand())) {
          Inner->Bits = Src->getType()->getScalarSizeInBits();
          if (!(*Inner == *Own)) {
            Observation = Src;
            return Inner;
          }
        }
    }
    return Own;
  };
  auto ZeroTest = [&](const llvm::CmpInst *Cmp) -> std::optional<Fact> {
    if (!Cmp || (Cmp->getPredicate() != llvm::CmpInst::ICMP_EQ &&
                 Cmp->getPredicate() != llvm::CmpInst::ICMP_NE))
      return std::nullopt;
    const llvm::Value *Val = nullptr;
    if (const auto *CI = llvm::dyn_cast<llvm::ConstantInt>(Cmp->getOperand(1));
        CI && CI->isZero())
      Val = Cmp->getOperand(0);
    else if (const auto *CI =
                 llvm::dyn_cast<llvm::ConstantInt>(Cmp->getOperand(0));
             CI && CI->isZero())
      Val = Cmp->getOperand(1);
    if (!Val)
      return std::nullopt;
    while (const auto *Cast = llvm::dyn_cast<llvm::CastInst>(Val)) {
      if (!llvm::isa<llvm::ZExtInst, llvm::SExtInst>(Cast))
        break;
      Val = Cast->getOperand(0);
    }
    const auto *InnerCmp = llvm::dyn_cast<llvm::ICmpInst>(Val);
    const auto *Load = llvm::dyn_cast<llvm::LoadInst>(Val);
    if (!InnerCmp && Load)
      if (const llvm::Value *Stored = LastStored(Load)) {
        const llvm::Value *Peeled = Stored;
        while (const auto *Cast = llvm::dyn_cast<llvm::CastInst>(Peeled)) {
          if (!llvm::isa<llvm::ZExtInst, llvm::SExtInst>(Cast))
            break;
          Peeled = Cast->getOperand(0);
        }
        if (const auto *AsCmp = llvm::dyn_cast<llvm::ICmpInst>(Peeled))
          InnerCmp = AsCmp;
      }
    if (InnerCmp && (InnerCmp->getPredicate() == llvm::CmpInst::ICMP_EQ ||
                     InnerCmp->getPredicate() == llvm::CmpInst::ICMP_NE)) {
      const llvm::Value *PtrVal = nullptr;
      if (const auto *CI =
              llvm::dyn_cast<llvm::ConstantInt>(InnerCmp->getOperand(1));
          CI && CI->isZero())
        PtrVal = InnerCmp->getOperand(0);
      else if (const auto *CI =
                   llvm::dyn_cast<llvm::ConstantInt>(InnerCmp->getOperand(0));
               CI && CI->isZero())
        PtrVal = InnerCmp->getOperand(1);
      while (const auto *Cast =
                 llvm::dyn_cast_or_null<llvm::CastInst>(PtrVal)) {
        if (!llvm::isa<llvm::ZExtInst, llvm::SExtInst>(Cast))
          break;
        PtrVal = Cast->getOperand(0);
      }
      if (const auto *PtrLoad =
              llvm::dyn_cast_or_null<llvm::LoadInst>(PtrVal)) {
        const llvm::LoadInst *Observation = nullptr;
        if (const std::optional<Key> K = Canonical(PtrLoad, Observation)) {
          const bool InnerNonZero =
              InnerCmp->getPredicate() == llvm::CmpInst::ICMP_NE;
          const bool OuterNonZero =
              Cmp->getPredicate() == llvm::CmpInst::ICMP_NE;
          return Fact{*K, InnerNonZero == OuterNonZero, Observation};
        }
      }
    }
    if (!Load || !Load->isSimple())
      return std::nullopt;
    const llvm::LoadInst *Observation = nullptr;
    if (const std::optional<Key> K = Canonical(Load, Observation))
      return Fact{*K, Cmp->getPredicate() == llvm::CmpInst::ICMP_NE,
                  Observation};
    return std::nullopt;
  };
  const auto *Here = llvm::dyn_cast<llvm::ICmpInst>(Br->getCondition());
  const std::optional<Fact> HereFact = ZeroTest(Here);
  if (!Here || !HereFact)
    return nullptr;
  const Key &HereKey = HereFact->K;
  const auto *PrivateSlot = llvm::dyn_cast<llvm::AllocaInst>(HereKey.Base);
  const bool Private = PrivateSlot && !allocaAddressTaken(PrivateSlot);
  auto Writes = [&](const llvm::BasicBlock *BB, const llvm::Instruction *Stop,
                    const llvm::Instruction *After = nullptr) {
    bool Active = !After;
    for (const llvm::Instruction &Inst : *BB) {
      if (&Inst == After) {
        Active = true;
        continue;
      }
      if (!Active)
        continue;
      if (Stop && &Inst == Stop)
        break;
      if (!Inst.mayWriteToMemory())
        continue;
      if (const auto *SI = llvm::dyn_cast<llvm::StoreInst>(&Inst)) {
        const std::optional<Key> Stored = KeyOfPtr(SI->getPointerOperand());
        if (Stored && Stored->Base == HereKey.Base &&
            Stored->Off == HereKey.Off)
          return true;
        // An unescaped register home cannot alias an object field or another
        // home. Any other store or writing call can invalidate a field fact.
        const auto *Slot = asAllocaPointer(SI->getPointerOperand());
        if (Slot && !allocaAddressTaken(Slot))
          continue;
      }
      if (!Private)
        return true;
    }
    return false;
  };
  if (Writes(Br->getParent(), Br))
    return nullptr;
  const bool HereNonZero = HereFact->TrueMeansNonZero;
  const llvm::BasicBlock *Cur = Br->getParent();
  llvm::SmallPtrSet<const llvm::BasicBlock *, 16> Seen;
  Seen.insert(Cur);
  for (int Step = 0; Step < 16; ++Step) {
    const llvm::BasicBlock *Pred = Cur->getSinglePredecessor();
    if (!Pred || !Seen.insert(Pred).second)
      return nullptr;
    if (const auto *PBr =
            llvm::dyn_cast<llvm::CondBrInst>(Pred->getTerminator())) {
      if (const auto *Dom =
              llvm::dyn_cast<llvm::ICmpInst>(PBr->getCondition())) {
        const std::optional<Fact> DomFact = ZeroTest(Dom);
        const bool TrueEdge = PBr->getSuccessor(0) == Cur;
        const bool FalseEdge = PBr->getSuccessor(1) == Cur;
        if (DomFact && DomFact->K == HereKey && (TrueEdge || FalseEdge)) {
          // A condition tests its captured load. A later write in that same
          // block invalidates the memory fact before control leaves it.
          if (!DomFact->Observation ||
              DomFact->Observation->getParent() != Pred ||
              Writes(Pred, PBr, DomFact->Observation))
            return nullptr;
          const bool DomNonZero =
              TrueEdge ? DomFact->TrueMeansNonZero : !DomFact->TrueMeansNonZero;
          const bool TakenTrue = DomNonZero == HereNonZero;
          const llvm::BasicBlock *Taken =
              TakenTrue ? Br->getSuccessor(0) : Br->getSuccessor(1);
          if (Taken && Taken != Br->getParent())
            return Taken;
          return nullptr;
        }
      }
    }
    if (Writes(Pred, nullptr))
      return nullptr;
    Cur = Pred;
  }
  return nullptr;
}

bool LLVMCWriter::deadNullAssignBlocks(
    const llvm::CondBrInst *Br, const llvm::BasicBlock *Taken,
    llvm::SmallVectorImpl<const llvm::BasicBlock *> &Blocks) {
  Blocks.clear();
  if (!Br || !Taken)
    return false;
  const llvm::BasicBlock *Other =
      Taken == Br->getSuccessor(0) ? Br->getSuccessor(1) : Br->getSuccessor(0);
  if (!Other || Other == Taken || Other == Br->getParent())
    return false;
  if (Other->getSinglePredecessor() != Br->getParent())
    return false;
  llvm::SmallPtrSet<const llvm::AllocaInst *, 4> Slots;
  const llvm::BasicBlock *Pred = Br->getParent();
  const llvm::BasicBlock *Cur = Other;
  llvm::SmallPtrSet<const llvm::BasicBlock *, 8> Seen;
  while (Cur && Seen.insert(Cur).second) {
    if (Cur != Other && Cur->getSinglePredecessor() != Pred)
      break;
    for (const llvm::Instruction &Inst : *Cur) {
      if (Inst.isTerminator() || llvm::isa<llvm::AllocaInst>(&Inst))
        continue;
      if (!instructionIsPrinted(Inst))
        continue;
      const auto *SI = llvm::dyn_cast<llvm::StoreInst>(&Inst);
      const llvm::AllocaInst *Slot =
          SI ? asAllocaPointer(SI->getPointerOperand()) : nullptr;
      auto Imm = Slot ? foldImmediate(SI->getValueOperand()) : std::nullopt;
      if (!Slot || !Imm || *Imm != "0")
        return false;
      Slots.insert(Slot);
    }
    Blocks.push_back(Cur);
    const auto *Go = llvm::dyn_cast<llvm::UncondBrInst>(Cur->getTerminator());
    if (!Go)
      break;
    const llvm::BasicBlock *Next = Go->getSuccessor(0);
    if (!Next || Next == Cur || Next->getSinglePredecessor() != Cur)
      break;
    Pred = Cur;
    Cur = Next;
  }
  if (Slots.empty())
    return false;
  llvm::SmallPtrSet<const llvm::AllocaInst *, 4> Defined;
  Pred = Br->getParent();
  Cur = Taken;
  Seen.clear();
  if (!Cur || Cur->getSinglePredecessor() != Pred)
    return false;
  while (Cur && Seen.insert(Cur).second) {
    if (Cur != Taken && Cur->getSinglePredecessor() != Pred)
      break;
    for (const llvm::Instruction &Inst : *Cur) {
      const auto *SI = llvm::dyn_cast<llvm::StoreInst>(&Inst);
      if (!SI)
        continue;
      const llvm::AllocaInst *Slot = asAllocaPointer(SI->getPointerOperand());
      if (Slot && Slots.count(Slot))
        Defined.insert(Slot);
    }
    if (Defined.size() == Slots.size())
      return true;
    const auto *Go = llvm::dyn_cast<llvm::UncondBrInst>(Cur->getTerminator());
    if (!Go)
      break;
    const llvm::BasicBlock *Next = Go->getSuccessor(0);
    if (!Next || Next == Cur || Next->getSinglePredecessor() != Cur)
      break;
    Pred = Cur;
    Cur = Next;
  }
  return false;
}

void LLVMCWriter::writeInstruction(llvm::Instruction &Inst, int Indent) {
  auto Event =
      SourceRecorder ? SourceRecorder->instruction(Inst) : std::nullopt;
  if (Event)
    OS << SourceRecorder->begin(*Event);
  writeInstructionImpl(Inst, Indent);
  if (Event)
    OS << SourceRecorder->end(*Event);
}

void LLVMCWriter::writeInstructionImpl(llvm::Instruction &Inst, int Indent) {
  if (llvm::isa<llvm::DbgInfoIntrinsic>(&Inst))
    return;
  // Aggregate SSA values and storage arrays have distinct C declarations but
  // identical byte layouts. A byte copy also preserves unaligned accesses.
  auto AggregateAddress = [&](const llvm::Value *Ptr) {
    std::string Text = valueStr(Ptr);
    if (llvm::isa<llvm::GlobalVariable>(Ptr))
      Text = "&" + Text;
    return Text;
  };
  if (auto *Load = llvm::dyn_cast<llvm::LoadInst>(&Inst);
      Load && Load->getType()->isAggregateType()) {
    if (!Load->isSimple() || Load->getPointerAddressSpace() != 0)
      throw std::runtime_error(
          "C projection has an unsupported aggregate load");
    emitIndent(Indent);
    OS << "__builtin_memcpy(&" << getName(Load) << ", "
       << AggregateAddress(Load->getPointerOperand()) << ", "
       << CurMod->getDataLayout().getTypeStoreSize(Load->getType()) << ");\n";
    return;
  }
  if (auto *Store = llvm::dyn_cast<llvm::StoreInst>(&Inst);
      Store && Store->getValueOperand()->getType()->isAggregateType()) {
    if (!Store->isSimple() || Store->getPointerAddressSpace() != 0)
      throw std::runtime_error(
          "C projection has an unsupported aggregate store");
    auto *Value = Store->getValueOperand();
    const auto Temp = freshVar();
    emitIndent(Indent);
    OS << "{ " << typeToCLLVM(Value->getType()) << " " << Temp << " = "
       << valueStr(Value) << ";\n";
    emitIndent(Indent + 1);
    OS << "__builtin_memcpy(" << AggregateAddress(Store->getPointerOperand())
       << ", &" << Temp << ", "
       << CurMod->getDataLayout().getTypeStoreSize(Value->getType())
       << "); }\n";
    return;
  }
  if (auto *Freeze = llvm::dyn_cast<llvm::FreezeInst>(&Inst)) {
    if (Freeze->use_empty() || isCallClobberValue(Freeze))
      return;
    const auto Name = getName(Freeze);
    auto *Value = Freeze->getOperand(0);
    auto *Ty = Value->getType();
    if (Ty->isIntegerTy() || Ty->isPointerTy() || Ty->isFloatingPointTy()) {
      // Explicit undef/poison permits an arbitrary stable choice. Copying a
      // possibly-poison expression into C could instead introduce undefined
      // behavior before freeze has a chance to define its result.
      bool ChooseZero = llvm::isa<llvm::UndefValue, llvm::PoisonValue>(Value);
      if (ChooseZero ||
          llvm::isGuaranteedNotToBeUndefOrPoison(Value, nullptr, Freeze,
                                                 &Dominators) ||
          freezeHasDefinedCProjection(Value)) {
        emitIndent(Indent);
        OS << Name << " = " << (ChooseZero ? "0" : valueStr(Value)) << ";\n";
        return;
      }
    }
    if (GuardAnalysisOnlyFunctions)
      throw std::runtime_error("C freeze operand is not proved defined: " +
                               Name);
  }

  // A synthesized image byte array can be accessed at several overlapping
  // widths.  Copy through its original base+offset so a scalar access never
  // creates a separate C object or assumes alignment at an interior offset.
  if (auto *Load = llvm::dyn_cast<llvm::LoadInst>(&Inst);
      Load && Load->getPointerAddressSpace() == 0) {
    const unsigned Size = imageIntegerAccessSize(Load->getType());
    if (Size)
      if (auto Pointer =
              imageByteArrayPointer(Load->getPointerOperand(), Size)) {
        if (Load->isAtomic())
          llvm::report_fatal_error(
              "unsupported atomic image byte-array C projection");
        const std::string Name = getName(Load);
        emitIndent(Indent);
        OS << Name << " = 0;\n";
        if (Load->isVolatile()) {
          const std::string Index = freshVar("image_byte");
          emitIndent(Indent);
          OS << "for (unsigned " << Index << " = 0; " << Index << " < " << Size
             << "; ++" << Index << ")\n";
          emitIndent(Indent + 1);
          OS << "((uint8_t*)&" << Name << ")[" << Index
             << "] = ((volatile uint8_t*)" << *Pointer << ")[" << Index
             << "];\n";
        } else {
          emitIndent(Indent);
          OS << "__builtin_memcpy(&" << Name << ", " << *Pointer << ", " << Size
             << ");\n";
        }
        return;
      }
  }
  if (auto *Store = llvm::dyn_cast<llvm::StoreInst>(&Inst);
      Store && Store->getPointerAddressSpace() == 0) {
    const unsigned Size =
        imageIntegerAccessSize(Store->getValueOperand()->getType());
    if (Size)
      if (auto Pointer =
              imageByteArrayPointer(Store->getPointerOperand(), Size)) {
        if (Store->isAtomic())
          llvm::report_fatal_error(
              "unsupported atomic image byte-array C projection");
        const std::string Bits = freshVar("image_bits");
        emitIndent(Indent);
        OS << "{ " << typeToCLLVM(Store->getValueOperand()->getType()) << " "
           << Bits << " = " << valueStr(Store->getValueOperand()) << ";\n";
        if (Store->isVolatile()) {
          const std::string Index = freshVar("image_byte");
          emitIndent(Indent + 1);
          OS << "for (unsigned " << Index << " = 0; " << Index << " < " << Size
             << "; ++" << Index << ")\n";
          emitIndent(Indent + 2);
          OS << "((volatile uint8_t*)" << *Pointer << ")[" << Index
             << "] = ((const uint8_t*)&" << Bits << ")[" << Index << "];\n";
        } else {
          emitIndent(Indent + 1);
          OS << "__builtin_memcpy(" << *Pointer << ", &" << Bits << ", " << Size
             << ");\n";
        }
        emitIndent(Indent);
        OS << "}\n";
        return;
      }
  }
  // LLVM i80 is the exact ten-byte x87 memory image.  C has no scalar with a
  // ten-byte object size, so use a zero-extended carrier and copy only ten
  // bytes.  A typed __uint128_t dereference would read/write six extra bytes.
  if (auto *Load = llvm::dyn_cast<llvm::LoadInst>(&Inst);
      Load && Load->getType()->isIntegerTy(80) &&
      Load->getPointerAddressSpace() == 0) {
    if (Load->isAtomic())
      llvm::report_fatal_error("unsupported atomic i80 C projection");
    const llvm::Value *Address = Load->getPointerOperand();
    std::string Pointer = valueStr(Address);
    if (std::string Image = imageDataCName(Address); !Image.empty())
      Pointer = "&" + Image;
    const std::string Name = getName(Load);
    emitIndent(Indent);
    OS << Name << " = 0;\n";
    if (Load->isVolatile()) {
      const std::string Index = freshVar("x87_byte");
      emitIndent(Indent);
      OS << "for (unsigned " << Index << " = 0; " << Index << " < 10; ++"
         << Index << ")\n";
      emitIndent(Indent + 1);
      OS << "((uint8_t*)&" << Name << ")[" << Index
         << "] = ((const volatile uint8_t*)(" << Pointer << "))[" << Index
         << "];\n";
    } else {
      emitIndent(Indent);
      OS << "__builtin_memcpy(&" << Name << ", (const void*)(" << Pointer
         << "), 10);\n";
    }
    return;
  }
  if (auto *Store = llvm::dyn_cast<llvm::StoreInst>(&Inst);
      Store && Store->getValueOperand()->getType()->isIntegerTy(80) &&
      Store->getPointerAddressSpace() == 0) {
    if (Store->isAtomic())
      llvm::report_fatal_error("unsupported atomic i80 C projection");
    const llvm::Value *Address = Store->getPointerOperand();
    std::string Pointer = valueStr(Address);
    if (std::string Image = imageDataCName(Address); !Image.empty())
      Pointer = "&" + Image;
    const std::string Bits = freshVar("x87_bits");
    emitIndent(Indent);
    OS << "{ __uint128_t " << Bits << " = "
       << valueStr(Store->getValueOperand()) << ";\n";
    if (Store->isVolatile()) {
      const std::string Index = freshVar("x87_byte");
      emitIndent(Indent + 1);
      OS << "for (unsigned " << Index << " = 0; " << Index << " < 10; ++"
         << Index << ")\n";
      emitIndent(Indent + 2);
      OS << "((volatile uint8_t*)(" << Pointer << "))[" << Index
         << "] = ((const uint8_t*)&" << Bits << ")[" << Index << "];\n";
    } else {
      emitIndent(Indent + 1);
      OS << "__builtin_memcpy((void*)(" << Pointer << "), &" << Bits
         << ", 10);\n";
    }
    emitIndent(Indent);
    OS << "}\n";
    return;
  }
  if (const auto *SI = llvm::dyn_cast<llvm::StoreInst>(&Inst)) {
    if (const llvm::AllocaInst *Slot =
            asAllocaPointer(SI->getPointerOperand())) {
      if (isJoinArmStore(SI)) {
        const llvm::Value *Stored = SI->getValueOperand();
        noteAllocaStore(Slot, Stored);
        if (storeFeedsPhiCallTail(SI))
          return;
        if (ForwardedFieldArgs.count(Slot)) {
          auto Imm = foldImmediate(Stored);
          if (!Imm || *Imm != "0")
            return;
        }
        emitIndent(Indent);
        OS << getName(Slot) << " = " << enumStoredText(Slot, Stored) << ";\n";
        return;
      }
    }
  }
  if (const auto *Load = llvm::dyn_cast<llvm::LoadInst>(&Inst);
      Load && !Load->isSimple() && Load->getPointerAddressSpace() == 0) {
    const std::string Type = typeToCLLVM(Load->getType());
    const llvm::Value *Address = Load->getPointerOperand();
    std::string AddressText;
    if (std::string Image = imageDataCName(Address); !Image.empty())
      AddressText = "&" + Image;
    else if (const auto *Slot = asAllocaPointer(Address))
      AddressText = "&" + getName(Slot);
    else if (llvm::isa<llvm::GlobalVariable>(Address->stripPointerCasts()))
      AddressText = "&" + valueStr(Address->stripPointerCasts());
    else
      AddressText = valueStr(Address);
    if (!OnlyFunction && !Load->isAtomic())
      if (const auto *Global = llvm::dyn_cast<llvm::GlobalVariable>(
              llvm::getUnderlyingObject(Address));
          Global && parseNdCodePtrSymbol(Global->getName())) {
        // Relocated pointer mirrors have a packed byte layout. Keep one
        // volatile scalar access without claiming its original alignment or
        // a separate scalar object's C effective type.
        emitIndent(Indent);
        OS << getName(Load)
           << " = ((const volatile struct __attribute__((packed, may_alias)) { "
           << Type << " value; } *)(" << AddressText << "))->value;\n";
        return;
      }
    const std::string Pointer = "(" + Type +
                                (Load->isVolatile() ? " volatile" : "") +
                                "*)(" + AddressText + ")";
    emitIndent(Indent);
    if (Load->isAtomic()) {
      const char *Ordering = "__ATOMIC_RELAXED";
      if (Load->getOrdering() == llvm::AtomicOrdering::Acquire)
        Ordering = "__ATOMIC_ACQUIRE";
      else if (Load->getOrdering() ==
               llvm::AtomicOrdering::SequentiallyConsistent)
        Ordering = "__ATOMIC_SEQ_CST";
      OS << "__atomic_load(" << Pointer << ", &" << getName(Load) << ", "
         << Ordering << ");\n";
    } else {
      OS << getName(Load) << " = *(" << Pointer << ");\n";
    }
    return;
  }
  if (auto *Load = llvm::dyn_cast<llvm::LoadInst>(&Inst);
      Load && !Load->isSimple() &&
      isLLVMX86SegmentedAddressSpace(Load->getPointerAddressSpace())) {
    if (Load->isAtomic())
      throw std::runtime_error("unsupported atomic segmented C load");
    auto Segment = renderX86SegmentedLoad(
        Opts.TheArch, *Load,
        [this](const llvm::Value *Value) { return valueStr(Value); });
    if (Segment.empty())
      throw std::runtime_error(
          "unsupported segmented C load width or architecture");
    if (Load->getType()->isPointerTy())
      Segment =
          "(" + typeToCLLVM(Load->getType()) + ")(uintptr_t)(" + Segment + ")";
    // Ordered loads receive a materialized ValueTexts name. That name is
    // their future use, not a replacement for the effect defining it.
    emitIndent(Indent);
    OS << getName(Load) << " = " << Segment << ";\n";
    HasCIntrinsics = true;
    return;
  }
  if (Analysis.Inlinable.count(&Inst))
    return;
  if (!Inst.getType()->isVoidTy()) {
    if (auto Imm = foldImmediate(&Inst)) {
      KnownImmediates[&Inst] = *Imm;
      if (!Inst.use_empty())
        return;
      // Leftover after composing `.rdata` scalars / image addresses into
      // calls. Unused junk arithmetic still prints.
      if (imageDataVA(&Inst))
        return;
      if (const auto *LI = llvm::dyn_cast<llvm::LoadInst>(&Inst)) {
        if (imageDataVA(LI->getPointerOperand()) || foldImmediate(LI))
          return;
      }
    }
  }
  if (Analysis.DeadFrameStores.count(&Inst))
    return;
  if (isUnusedCallClobber(Inst))
    return;
  if ((llvm::isa<llvm::CastInst, llvm::FreezeInst, llvm::PHINode>(&Inst) ||
       isUnknownCopyInst(Inst)) &&
      usersAreDeadCopies(&Inst))
    return;
  if (AfterCxxThrow && (llvm::isa<llvm::ReturnInst>(&Inst) ||
                        llvm::isa<llvm::UnreachableInst>(&Inst)))
    return;

  auto Name = Inst.getType()->isVoidTy() ? "" : getName(&Inst);

  if (Inst.isBinaryOp()) {
    if (auto Identity =
            targetPointerMaskIdentity(llvm::cast<llvm::BinaryOperator>(Inst))) {
      emitIndent(Indent);
      OS << Name << " = " << *Identity << ";\n";
      return;
    }
    if (Inst.getOpcode() == llvm::Instruction::URem ||
        Inst.getOpcode() == llvm::Instruction::SRem) {
      if (std::string Rem = indexExprStr(&Inst); !Rem.empty()) {
        emitIndent(Indent);
        OS << Name << " = " << Rem << ";\n";
        return;
      }
    }
    const bool NeedsIntegerPointerOperand = Inst.getType()->isIntegerTy();
    auto LHS =
        logicalShiftLhs(Inst, NeedsIntegerPointerOperand
                                  ? integerPointerOperandStr(Inst.getOperand(0))
                                  : valueStr(Inst.getOperand(0)));
    auto RHS = NeedsIntegerPointerOperand
                   ? integerPointerOperandStr(Inst.getOperand(1))
                   : valueStr(Inst.getOperand(1));
    emitIndent(Indent);
    OS << Name << " = " << binopStr(Inst.getOpcode(), LHS, RHS, Inst.getType())
       << ";\n";
    return;
  }

  if (auto *CI = llvm::dyn_cast<llvm::ICmpInst>(&Inst)) {
    emitIndent(Indent);
    OS << Name << " = " << icmpInlineText(*CI) << ";\n";
    return;
  }

  if (auto *FCI = llvm::dyn_cast<llvm::FCmpInst>(&Inst)) {
    emitIndent(Indent);
    OS << Name << " = " << fcmpInlineText(*FCI) << ";\n";
    return;
  }

  if (Inst.getOpcode() == llvm::Instruction::FNeg) {
    emitIndent(Indent);
    OS << Name << " = " << renderInline(Inst) << ";\n";
    return;
  }

  if (auto *LI = llvm::dyn_cast<llvm::LoadInst>(&Inst)) {
    if (isImportCalleeOnlyLoad(LI) || computedAllocaLoadIsForwarded(LI) ||
        !valueFeedsPrintedUse(LI) ||
        isJoinCallArgAlloca(asAllocaPointer(LI->getPointerOperand())) ||
        joinFieldDefaultText(asAllocaPointer(LI->getPointerOperand())))
      return;
    if (auto Text = ValueTexts.find(LI);
        Text != ValueTexts.end() && !Text->second.empty())
      return;
    if (const llvm::AllocaInst *Slot =
            asAllocaPointer(LI->getPointerOperand())) {
      if (auto It = AllocaTypes.find(Slot); It != AllocaTypes.end())
        ValueTypes[&Inst] = It->second;
      if (isTypedRecordCursorSlot(Slot))
        return;
      emitIndent(Indent);
      OS << Name << " = " << getName(Slot) << ";\n";
      if (auto Text = AllocaTexts.find(Slot); Text != AllocaTexts.end())
        ValueTexts[&Inst] = Text->second;
      return;
    }
    if (auto Member = typedRecordAccess(LI->getPointerOperand(),
                                        llvmAccessSize(LI->getType()))) {
      ValueTypes[&Inst] = Member->Type;
      ValueTexts[&Inst] = Member->Text;
      return;
    }
    if (auto Member = frameSlotAccess(LI->getPointerOperand(),
                                      llvmAccessSize(LI->getType()),
                                      /*AddressOf=*/false)) {
      ValueTypes[&Inst] = Member->Type;
      ValueTexts[&Inst] = Member->Text;
      return;
    }
    if (auto Index = typedIndexAccess(LI->getPointerOperand())) {
      ValueTypes[&Inst] = Index->Type;
      ValueTexts[&Inst] = Index->Text;
      return;
    }
    if (writeRawMemoryCopy(Inst, Indent))
      return;
    emitIndent(Indent);
    if (std::string Seg = renderX86SegmentedLoad(
            Opts.TheArch, *LI,
            [this](const llvm::Value *V) { return valueStr(V); });
        !Seg.empty()) {
      OS << Name << " = " << Seg << ";\n";
      HasCIntrinsics = true;
      return;
    }
    if (auto VA = imageDataVA(LI->getPointerOperand())) {
      const auto *LoadTy = LI->getType();
      const uint16_t Size =
          LoadTy && LoadTy->isIntegerTy()
              ? static_cast<uint16_t>(LoadTy->getIntegerBitWidth() / 8)
              : 0;
      if (auto Imm = foldReadonlyScalar(*VA, Size)) {
        const std::string Hex = "0x" + llvm::utohexstr(*Imm);
        KnownImmediates[&Inst] = Hex;
        OS << Name << " = " << Hex << ";\n";
        return;
      }
      if (auto Text = ValueTexts.find(LI);
          Text != ValueTexts.end() && !Text->second.empty())
        return;
      if (std::string Image = imageDataCName(LI->getPointerOperand());
          !Image.empty())
        OS << Name << " = " << Image << ";\n";
      else
        OS << Name << " = *(" << typeToCLLVM(LI->getType()) << "*)"
           << valueStr(LI->getPointerOperand()) << ";\n";
    } else
      OS << Name << " = *(" << typeToCLLVM(LI->getType()) << "*)"
         << valueStr(LI->getPointerOperand()) << ";\n";
    return;
  }

  if (auto *SI = llvm::dyn_cast<llvm::StoreInst>(&Inst)) {
    if (const llvm::AllocaInst *Slot =
            asAllocaPointer(SI->getPointerOperand())) {
      const llvm::Value *Stored = SI->getValueOperand();
      noteAllocaStore(Slot, Stored);
      if (isKilledEntryZeroStore(SI, Slot))
        return;
      if (storeFeedsPhiCallTail(SI))
        return;
      if (!isJoinArmStore(SI) &&
          (OmittedInlined.count(SI) || allocaStoreIsHidden(Slot, Stored)))
        return;
      if (ForwardedFieldArgs.count(Slot)) {
        auto Imm = foldImmediate(Stored);
        if (!Imm || *Imm != "0")
          return;
      }
      emitIndent(Indent);
      if (isTypedRecordCursorSlot(Slot)) {
        std::string Text = ultimateComposedText(Stored);
        if (const auto *LI = llvm::dyn_cast<llvm::LoadInst>(Stored)) {
          if (const llvm::AllocaInst *Src =
                  asAllocaPointer(LI->getPointerOperand());
              Src && Src != Slot) {
            auto SrcTyped = [&] {
              if (isTypedRecordCursorSlot(Src))
                return true;
              auto Ty = AllocaTypes.find(Src);
              return Ty != AllocaTypes.end() && Ty->second &&
                     Ty->second->Kind == NdTypeKind::Ptr &&
                     Ty->second->Pointee &&
                     Ty->second->Pointee->Kind == NdTypeKind::Struct &&
                     !Ty->second->Pointee->SourceName.empty() &&
                     !Ty->second->Pointee->IsEnum;
            };
            if (SrcTyped()) {
              auto SameText = [&](const llvm::Value *Value) {
                if (!Value || Text.empty())
                  return false;
                auto Cached = ValueTexts.find(Value);
                return Cached != ValueTexts.end() && Cached->second == Text;
              };
              bool UseName =
                  Text.empty() || Text.find("m_ppBins[") != std::string::npos;
              if (!UseName) {
                if (auto Cached = AllocaTexts.find(Src);
                    Cached != AllocaTexts.end() && Cached->second == Text)
                  UseName = true;
                if (!UseName) {
                  if (auto Home = AllocaHomeValues.find(Src);
                      Home != AllocaHomeValues.end())
                    UseName = SameText(Home->second);
                }
              }
              const llvm::AllocaInst *Named = nullptr;
              if (UseName) {
                const llvm::AllocaInst *Cur = Src;
                llvm::SmallPtrSet<const llvm::AllocaInst *, 8> Seen;
                while (Cur && Seen.insert(Cur).second) {
                  const llvm::Value *HomeVal = nullptr;
                  if (auto Home = AllocaHomeValues.find(Cur);
                      Home != AllocaHomeValues.end())
                    HomeVal = Home->second;
                  if (HomeVal && !allocaStoreIsHidden(Cur, HomeVal)) {
                    Named = Cur;
                    break;
                  }
                  const auto *HomeLoad =
                      llvm::dyn_cast_or_null<llvm::LoadInst>(HomeVal);
                  const llvm::AllocaInst *Next =
                      HomeLoad ? asAllocaPointer(HomeLoad->getPointerOperand())
                               : nullptr;
                  if (!Next || Next == Slot)
                    break;
                  Cur = Next;
                }
              }
              if (Named) {
                std::string SrcName;
                if (auto Name = ValNames.find(Named);
                    Name != ValNames.end() && !Name->second.empty())
                  SrcName = Name->second;
                else
                  SrcName = getName(const_cast<llvm::AllocaInst *>(Named));
                if (!SrcName.empty() && SrcName != getName(Slot))
                  Text = std::move(SrcName);
              }
            }
          }
        }
        if (!Text.empty()) {
          OS << getName(Slot) << " = " << Text << ";\n";
          return;
        }
      }
      OS << getName(Slot) << " = " << enumStoredText(Slot, Stored) << ";\n";
    } else {
      const llvm::CallBase *Fill = nullptr;
      if (auto Packed = LeftoverSiblingFill.find(SI);
          Packed != LeftoverSiblingFill.end())
        Fill = Packed->second;
      else
        Fill = leftoverPackedSiblingFill(*SI);
      if (Fill) {
        const uint16_t FillSize = llvmAccessSize(Fill->getType());
        if (auto Member = frameSlotAccess(SI->getPointerOperand(),
                                          FillSize ? FillSize : 4,
                                          /*AddressOf=*/false,
                                          /*Synthesize=*/false,
                                          /*Overlay=*/true)) {
          emitIndent(Indent);
          OS << Member->Text << " = " << valueStr(Fill) << ";\n";
          return;
        }
      }
      if (OmittedLeftoverNarrow.count(SI) || storeIsLeftoverHomeUndef(*SI))
        return;
      if (auto Member = typedRecordAccess(
              SI->getPointerOperand(),
              llvmAccessSize(SI->getValueOperand()->getType()))) {
        emitIndent(Indent);
        if (std::string Up =
                inplaceIntegerUpdate(*SI, Member->Text, SI->getValueOperand());
            !Up.empty())
          OS << Up << ";\n";
        else
          OS << Member->Text << " = "
             << integerCallStoredText(SI->getValueOperand()) << ";\n";
        return;
      }
      const llvm::Value *Stored = SI->getValueOperand();
      const uint16_t Size = llvmAccessSize(Stored->getType());
      const bool OverlayInt =
          Stored->getType()->isIntegerTy() &&
          !llvm::isa<llvm::ConstantPointerNull>(Stored) &&
          !(llvm::isa<llvm::ConstantInt>(Stored) &&
            llvm::cast<llvm::ConstantInt>(Stored)->isZero());
      auto integerReturnCall = [&](const llvm::Value *V) {
        std::set<const llvm::Value *> Seen;
        while (V && Seen.insert(V).second) {
          if (const auto *Cast = llvm::dyn_cast<llvm::CastInst>(V)) {
            if (llvm::isa<llvm::ZExtInst, llvm::SExtInst, llvm::TruncInst>(
                    Cast)) {
              V = Cast->getOperand(0);
              continue;
            }
          }
          if (const auto *CB = llvm::dyn_cast<llvm::CallBase>(V)) {
            llvm::Type *Ty = CB->getType();
            return Ty && Ty->isIntegerTy() && !Ty->isIntegerTy(1);
          }
          if (const auto *LI = llvm::dyn_cast<llvm::LoadInst>(V)) {
            if (const llvm::AllocaInst *Home =
                    asAllocaPointer(LI->getPointerOperand())) {
              if (const llvm::Value *Prev = allocaStoredValueBefore(LI);
                  Prev && Prev != V) {
                V = Prev;
                continue;
              }
            }
          }
          return false;
        }
        return false;
      };
      auto zeroPastAddressTakenByteObject = [&]() {
        auto Imm = foldImmediate(Stored);
        if (!Imm || *Imm != "0" || !SyntheticFrame)
          return false;
        const auto Peeled = peelPointerOffset(SI->getPointerOperand());
        if (!Peeled || Peeled->first != SyntheticFrame)
          return false;
        if (FrameSlots.count(Peeled->second))
          return false;
        for (const auto &[SlotOff, Slot] : FrameSlots) {
          const TypeRef Ty = Slot.Type ? Slot.Type : Slot.CallType;
          if (Slot.Name.empty() || !Slot.AddressTaken || !Ty || !Ty->Size ||
              Ty->Size > 8)
            continue;
          if (SlotOff + static_cast<uint64_t>(Ty->Size) == Peeled->second)
            return true;
        }
        return false;
      };
      if (auto Member = frameSlotAccess(SI->getPointerOperand(), Size,
                                        /*AddressOf=*/false,
                                        /*Synthesize=*/false,
                                        /*Overlay=*/OverlayInt)) {
        if (OverlayInt && !integerReturnCall(Stored) &&
            (llvm::StringRef(Member->Text).ends_with(".values_") ||
             llvm::StringRef(Member->Text).ends_with("->values_"))) {
          if (auto Typed = frameSlotAccess(SI->getPointerOperand(), Size,
                                           /*AddressOf=*/false,
                                           /*Synthesize=*/false,
                                           /*Overlay=*/false))
            Member = std::move(Typed);
        }
        if (llvm::StringRef(Member->Text).ends_with(".m_pszData") &&
            isResultParamValue(Stored))
          return;
        std::string StoredText = integerCallStoredText(Stored);
        if (Size >= 16 && Member->Type &&
            Member->Type->Kind == NdTypeKind::Struct &&
            !Member->Type->SourceName.empty()) {
          const std::string PlainText = StoredText;
          const llvm::Value *Source = Stored;
          while (const auto *Cast = llvm::dyn_cast<llvm::CastInst>(Source))
            Source = Cast->getOperand(0);
          if (const auto *Load = llvm::dyn_cast<llvm::LoadInst>(Source)) {
            auto SourceRecord = frameSlotAccess(Load->getPointerOperand(), Size,
                                                /*AddressOf=*/false);
            auto SourceAddress = frameSlotAccess(Load->getPointerOperand(), 0,
                                                 /*AddressOf=*/true);
            const bool SameRecord =
                SourceRecord && SourceRecord->Type &&
                SourceRecord->Type->Kind == NdTypeKind::Struct &&
                SourceRecord->Type->Size == Size &&
                cNamedTypeSpelling(SourceRecord->Type->SourceName) ==
                    cNamedTypeSpelling(Member->Type->SourceName);
            if (SourceAddress && Member->Type->Size == Size && !SameRecord) {
              StoredText = "*((" +
                           cNamedTypeSpelling(Member->Type->SourceName) +
                           " *)" + SourceAddress->Text + ")";
            }
          }
          if (StoredText == PlainText) {
            const NamedFrameSlot *SourceSlot = nullptr;
            bool Ambiguous = false;
            for (const auto &[_, Slot] : FrameSlots) {
              if (Slot.Name != PlainText)
                continue;
              if (SourceSlot)
                Ambiguous = true;
              SourceSlot = &Slot;
            }
            if (!Ambiguous && SourceSlot && SourceSlot->Type &&
                Member->Type->Size == Size &&
                (SourceSlot->Type->Kind != NdTypeKind::Struct ||
                 SourceSlot->Type->Size != Size ||
                 cNamedTypeSpelling(SourceSlot->Type->SourceName) !=
                     cNamedTypeSpelling(Member->Type->SourceName))) {
              StoredText = "*((" +
                           cNamedTypeSpelling(Member->Type->SourceName) +
                           " *)&" + SourceSlot->Name + ")";
            }
          }
        }
        // The printed call prototype can be narrower or a pointer even when
        // the lifted store carries i64 machine bits. Keep the call expression
        // readable, but make the field assignment's conversion explicit.
        const llvm::StringRef MemberText(Member->Text);
        const bool ArgListField = MemberText.ends_with(".types_") ||
                                  MemberText.ends_with("->types_") ||
                                  MemberText.ends_with(".values_") ||
                                  MemberText.ends_with("->values_");
        if (ArgListField && Size == 8 && Member->Type &&
            Member->Type->Size == Size) {
          const llvm::Value *Root = Stored;
          std::set<const llvm::Value *> Seen;
          for (unsigned Depth = 0;
               Root && Depth < 8 && Seen.insert(Root).second; ++Depth) {
            if (const auto *Cast = llvm::dyn_cast<llvm::CastInst>(Root)) {
              if (llvm::isa<llvm::SExtInst, llvm::ZExtInst, llvm::TruncInst,
                            llvm::PtrToIntInst, llvm::IntToPtrInst,
                            llvm::BitCastInst>(Cast)) {
                Root = Cast->getOperand(0);
                continue;
              }
            }
            if (const auto *Load = llvm::dyn_cast<llvm::LoadInst>(Root)) {
              if (const auto *Home =
                      asAllocaPointer(Load->getPointerOperand())) {
                if (const llvm::Value *Prev = allocaStoredValueBefore(Load);
                    Prev && Prev != Root) {
                  Root = Prev;
                  continue;
                }
              }
            }
            break;
          }
          if (const auto *Call = llvm::dyn_cast_or_null<llvm::CallBase>(Root)) {
            TypeRef ReturnType;
            if (auto FS = debugCallee(*Call))
              ReturnType = FS->ReturnType;
            if (!ReturnType)
              if (const MsvcCallee *Msvc =
                      msvcCallee(printedCalleeName(*Call), Opts.Format)) {
                if (Msvc->ReturnKind == MsvcReturnKind::WCharPtr)
                  ReturnType = NdType::makePtr(NdType::makeInt(2, false));
                else if (Msvc->ReturnKind == MsvcReturnKind::VoidPtr)
                  ReturnType = NdType::makePtr();
                else if (Msvc->ReturnKind == MsvcReturnKind::Int32)
                  ReturnType = NdType::makeInt(4, true);
              }
            if (!ReturnType && Call->getType()->isPointerTy())
              ReturnType = NdType::makePtr();
            if (!ReturnType && Call->getType()->isIntegerTy())
              ReturnType =
                  NdType::makeInt(llvmAccessSize(Call->getType()), false);
            if (Member->Type->Kind == NdTypeKind::Ptr &&
                Stored->getType()->isIntegerTy()) {
              // Preserve all LLVM truncation and extension in StoredText;
              // the final field conversion must not bypass those operations.
              StoredText = "(" + typeToC(Member->Type) + ")(uintptr_t)(" +
                           StoredText + ")";
            } else if (ReturnType) {
              if (Member->Type->Kind == NdTypeKind::Int &&
                  ReturnType->Kind == NdTypeKind::Ptr &&
                  ReturnType->Size == Size && StoredText == valueStr(Call)) {
                StoredText = "(uintptr_t)(" + StoredText + ")";
              }
            }
          }
        }
        emitIndent(Indent);
        if (std::string Up = inplaceIntegerUpdate(*SI, Member->Text, Stored);
            !Up.empty())
          OS << Up << ";\n";
        else
          OS << Member->Text << " = " << StoredText << ";\n";
      } else if (std::string Image = imageDataCName(SI->getPointerOperand());
                 !Image.empty()) {
        emitIndent(Indent);
        OS << Image << " = " << valueStr(SI->getValueOperand()) << ";\n";
      } else if (Size && Size <= 4) {
        if (auto Member = frameSlotAccess(SI->getPointerOperand(), Size,
                                          /*AddressOf=*/false,
                                          /*Synthesize=*/true,
                                          /*Overlay=*/false)) {
          emitIndent(Indent);
          OS << Member->Text << " = " << integerCallStoredText(Stored) << ";\n";
          return;
        }
        const auto *Init =
            llvm::dyn_cast<llvm::ConstantInt>(SI->getValueOperand());
        if (Init && Init->getBitWidth() <= 64 && Init->getSExtValue() == -2 &&
            SyntheticFrame &&
            functionNeedsAnalysisOnlyEHWrap(*SI->getFunction())) {
          if (const auto Peeled = peelPointerOffset(SI->getPointerOperand());
              Peeled && Peeled->first == SyntheticFrame)
            return;
        }
        if (zeroPastAddressTakenByteObject())
          return;
        if (writeRawMemoryCopy(Inst, Indent))
          return;
        emitIndent(Indent);
        OS << "*(" << typeToCLLVM(SI->getValueOperand()->getType()) << "*)"
           << valueStr(SI->getPointerOperand()) << " = "
           << integerPointerOperandStr(SI->getValueOperand()) << ";\n";
      } else {
        const auto *Init =
            llvm::dyn_cast<llvm::ConstantInt>(SI->getValueOperand());
        if (Init && Init->getBitWidth() <= 64 && Init->getSExtValue() == -2 &&
            SyntheticFrame &&
            functionNeedsAnalysisOnlyEHWrap(*SI->getFunction())) {
          if (const auto Peeled = peelPointerOffset(SI->getPointerOperand());
              Peeled && Peeled->first == SyntheticFrame)
            return;
        }
        if (zeroPastAddressTakenByteObject())
          return;
        if (writeRawMemoryCopy(Inst, Indent))
          return;
        emitIndent(Indent);
        OS << "*(" << typeToCLLVM(SI->getValueOperand()->getType()) << "*)"
           << valueStr(SI->getPointerOperand()) << " = "
           << integerPointerOperandStr(SI->getValueOperand()) << ";\n";
      }
    }
    return;
  }

  if (llvm::isa<llvm::AllocaInst>(&Inst))
    return;

  if (auto *GEP = llvm::dyn_cast<llvm::GetElementPtrInst>(&Inst)) {
    writeGEP(*GEP, Name, Indent);
    return;
  }

  if (Inst.isCast()) {
    auto Src = valueStr(Inst.getOperand(0));
    if (isFloatingPointBitcast(Inst)) {
      const auto *Cast = llvm::cast<llvm::BitCastInst>(&Inst);
      const unsigned Bytes = Cast->getSrcTy()->getPrimitiveSizeInBits() / 8;
      const std::string Source = freshVar("bitcast_source");
      emitIndent(Indent);
      OS << Name << " = 0;\n";
      emitIndent(Indent);
      OS << "{ " << typeToCLLVM(Cast->getSrcTy()) << " " << Source << " = "
         << Src << ";\n";
      emitIndent(Indent + 1);
      OS << "__builtin_memcpy(&" << Name << ", &" << Source << ", " << Bytes
         << ");\n";
      emitIndent(Indent);
      OS << "}\n";
      return;
    }
    emitIndent(Indent);
    OS << Name << " = "
       << castStr(Inst.getOpcode(), Src, Inst.getOperand(0)->getType(),
                  Inst.getType())
       << ";\n";
    return;
  }

  if (auto *Call = llvm::dyn_cast<llvm::CallInst>(&Inst)) {
    writeCall(*Call, Name, Indent);
    return;
  }

  if (auto *Invoke = llvm::dyn_cast<llvm::InvokeInst>(&Inst)) {
    writeInvoke(*Invoke, Name, Indent);
    return;
  }

  if (auto *CS = llvm::dyn_cast<llvm::CatchSwitchInst>(&Inst)) {
    writeCatchSwitch(*CS, Indent);
    return;
  }

  if (llvm::isa<llvm::CatchPadInst, llvm::CleanupPadInst>(&Inst))
    return;

  if (auto *CR = llvm::dyn_cast<llvm::CatchReturnInst>(&Inst)) {
    writeGoto(Inst.getParent(), CR->getSuccessor(), Indent);
    return;
  }

  if (auto *Clean = llvm::dyn_cast<llvm::CleanupReturnInst>(&Inst)) {
    writeCleanupRet(*Clean, Indent);
    return;
  }

  if (auto *Br = llvm::dyn_cast<llvm::UncondBrInst>(&Inst)) {
    const llvm::BasicBlock *Target = Br->getSuccessor(0);
    if (!edgePrintsPhiCopy(Inst.getParent(), Target) &&
        uncondBranchFallsIntoEHBoundary(Inst.getParent(), Target)) {
      writePhiCopies(Inst.getParent(), Target, Indent);
      if (!EHSkippedMainBlocks.empty())
        EHFallthroughLabelCandidates.insert(Target);
      return;
    }
    const bool FallsThrough = joinPrintsNext(Inst.getParent(), Target);
    writeGoto(Inst.getParent(), Target, Indent, !FallsThrough);
    return;
  }

  if (auto *Br = llvm::dyn_cast<llvm::CondBrInst>(&Inst)) {
    const llvm::BasicBlock *From = Inst.getParent();
    const llvm::BasicBlock *Then = Br->getSuccessor(0);
    const llvm::BasicBlock *Else = Br->getSuccessor(1);
    if (const llvm::BasicBlock *Taken = impliedPointerSuccessor(Br)) {
      const llvm::BasicBlock *Body = Taken == Then
                                         ? straightLineTrueArm(From, Then, true)
                                         : straightLineFalseSkip(From, Else);
      const llvm::BasicBlock *Other = Taken == Then ? Else : Then;
      llvm::SmallVector<const llvm::BasicBlock *, 4> DeadNull;
      if (deadNullAssignBlocks(Br, Taken, DeadNull)) {
        for (const llvm::BasicBlock *BB : DeadNull)
          InlinedFallthroughBlocks.insert(BB);
      }
      if (Body) {
        for (llvm::Instruction &BodyInst :
             *const_cast<llvm::BasicBlock *>(Body)) {
          if (BodyInst.isTerminator() || llvm::isa<llvm::AllocaInst>(&BodyInst))
            continue;
          writeInstruction(BodyInst, Indent);
        }
        if (const auto *Go =
                llvm::dyn_cast<llvm::UncondBrInst>(Body->getTerminator())) {
          const llvm::BasicBlock *Succ = Go->getSuccessor(0);
          // Both edges already meet, so the continuation prints after this if.
          if (Succ && printBranchTarget(Succ) != printBranchTarget(Other)) {
            const bool Falls =
                joinPrintsNext(From, Succ) && !cursorForExitsAt(Succ);
            writeGoto(Body, Succ, Indent, !Falls);
          }
        } else if (auto *Ret = llvm::dyn_cast<llvm::ReturnInst>(
                       Body->getTerminator())) {
          writeInstruction(*const_cast<llvm::ReturnInst *>(Ret), Indent);
        }
        InlinedFallthroughBlocks.insert(Body);
      } else {
        writeGoto(From, Taken, Indent);
      }
      return;
    }
    std::vector<const llvm::BasicBlock *> ChainConds;
    const llvm::BasicBlock *ChainArm = nullptr;
    const llvm::BasicBlock *ChainDef = nullptr;
    const llvm::BasicBlock *ChainJoin = nullptr;
    bool ChainInvert = false;
    if (conditionChainElse(From, ChainConds, ChainArm, ChainDef, ChainJoin,
                           ChainInvert)) {
      auto Group = [](const std::string &S) {
        if (S.find("&&") != std::string::npos ||
            S.find("||") != std::string::npos)
          return "(" + S + ")";
        return S;
      };
      auto InvertText = [](std::string Cond) {
        auto Wrapped = [](const std::string &S) {
          if (S.size() < 2 || S.front() != '(' || S.back() != ')')
            return false;
          int Depth = 0;
          for (size_t I = 0; I < S.size(); ++I) {
            if (S[I] == '(')
              ++Depth;
            else if (S[I] == ')') {
              --Depth;
              if (Depth == 0 && I + 1 != S.size())
                return false;
            }
          }
          return Depth == 0;
        };
        if (Cond.size() >= 2 && Cond[0] == '!' && Wrapped(Cond.substr(1)))
          return Cond.substr(2, Cond.size() - 3);
        return "!(" + Cond + ")";
      };
      std::string Cond;
      bool CondOk = true;
      for (const llvm::BasicBlock *PieceBB : ChainConds) {
        const auto *PieceBr =
            llvm::cast<llvm::CondBrInst>(PieceBB->getTerminator());
        const auto SavedLast = AllocaLastValues;
        AllocaLastValues.clear();
        for (const llvm::Instruction &Local : *PieceBB) {
          const auto *SI = llvm::dyn_cast<llvm::StoreInst>(&Local);
          if (!SI)
            continue;
          if (const llvm::AllocaInst *Slot =
                  asAllocaPointer(SI->getPointerOperand()))
            AllocaLastValues[Slot] = SI->getValueOperand();
        }
        std::string Piece = condStr(PieceBr->getCondition());
        std::optional<std::string> Swapped;
        if (ChainInvert)
          Swapped = invertedRelationalText(PieceBr->getCondition());
        AllocaLastValues = SavedLast;
        if (ChainInvert)
          Piece = Swapped ? std::move(*Swapped) : InvertText(std::move(Piece));
        Piece = Group(Piece);
        if (Piece.empty()) {
          CondOk = false;
          break;
        }
        for (const llvm::Instruction &Local : *PieceBB) {
          const auto *Call = llvm::dyn_cast<llvm::CallInst>(&Local);
          if (!Call)
            continue;
          const std::string Callee = printedCalleeName(*Call);
          if (!Callee.empty() && Piece.find(Callee) == std::string::npos) {
            CondOk = false;
            break;
          }
        }
        if (!CondOk)
          break;
        if (!Cond.empty())
          Cond += " && ";
        Cond += Piece;
      }
      if (CondOk) {
        for (size_t I = 1; I < ChainConds.size(); ++I)
          ConditionChainBodies.insert(ChainConds[I]);
        ConditionChainBodies.insert(ChainArm);
        ConditionChainBodies.insert(ChainDef);
        const bool Falls = joinPrintsNext(From, ChainJoin);
        emitIndent(Indent);
        OS << "if (" << Cond << ") {\n";
        for (llvm::Instruction &ArmInst :
             *const_cast<llvm::BasicBlock *>(ChainArm)) {
          if (ArmInst.isTerminator() || llvm::isa<llvm::AllocaInst>(&ArmInst))
            continue;
          writeInstruction(ArmInst, Indent + 1);
        }
        if (!Falls)
          writeGoto(ChainArm, ChainJoin, Indent + 1);
        emitIndent(Indent);
        OS << "} else {\n";
        for (llvm::Instruction &DefInst :
             *const_cast<llvm::BasicBlock *>(ChainDef)) {
          if (DefInst.isTerminator() || llvm::isa<llvm::AllocaInst>(&DefInst))
            continue;
          writeInstruction(DefInst, Indent + 1);
        }
        if (!Falls)
          writeGoto(ChainDef, ChainJoin, Indent + 1);
        emitIndent(Indent);
        OS << "}\n";
        return;
      }
    }
    const llvm::BasicBlock *ThenJoin = straightAssignArmJoin(Then);
    const llvm::BasicBlock *ElseJoin = straightAssignArmJoin(Else);
    if (Then && Else && Then != Else && ThenJoin && ThenJoin == ElseJoin) {
      auto PrintedStore =
          [&](const llvm::BasicBlock *Arm) -> const llvm::StoreInst * {
        const llvm::StoreInst *Store = nullptr;
        for (const llvm::Instruction &Local : *Arm) {
          if (Local.isTerminator())
            break;
          const auto *SI = llvm::dyn_cast<llvm::StoreInst>(&Local);
          if (!SI) {
            // A ternary replaces the entire arm. A materialized load or
            // expression still needs its assignment in that arm, so retain
            // the if/else when any such statement would be discarded.
            if (instructionIsPrinted(Local))
              return nullptr;
            continue;
          }
          if (!SI->isSimple())
            return nullptr;
          if (!asAllocaPointer(SI->getPointerOperand()))
            return nullptr;
          if (!instructionIsPrinted(*SI))
            continue;
          if (Store)
            return nullptr;
          Store = SI;
        }
        return Store;
      };
      const llvm::StoreInst *ThenStore = PrintedStore(Then);
      const llvm::StoreInst *ElseStore = PrintedStore(Else);
      const llvm::AllocaInst *ThenSlot =
          ThenStore ? asAllocaPointer(ThenStore->getPointerOperand()) : nullptr;
      const llvm::AllocaInst *ElseSlot =
          ElseStore ? asAllocaPointer(ElseStore->getPointerOperand()) : nullptr;
      const bool OneImm =
          ThenStore && ElseStore &&
          (foldImmediate(ThenStore->getValueOperand()).has_value() !=
           foldImmediate(ElseStore->getValueOperand()).has_value());
      if (ThenSlot && ThenSlot == ElseSlot && !allocaAddressTaken(ThenSlot) &&
          OneImm && !edgePrintsPhiCopy(Then, ThenJoin) &&
          !edgePrintsPhiCopy(Else, ElseJoin)) {
        auto LastStore =
            [&](const llvm::BasicBlock *BB) -> const llvm::StoreInst * {
          const llvm::StoreInst *Last = nullptr;
          for (const llvm::Instruction &Local : *BB) {
            const auto *SI = llvm::dyn_cast<llvm::StoreInst>(&Local);
            if (SI && asAllocaPointer(SI->getPointerOperand()) == ThenSlot)
              Last = SI;
          }
          return Last;
        };
        llvm::DenseMap<const llvm::BasicBlock *, char> Reach;
        std::function<char(const llvm::BasicBlock *)> Carry =
            [&](const llvm::BasicBlock *BB) -> char {
          if (!BB)
            return 2;
          if (BB == Then || BB == Else)
            return 1;
          if (auto It = Reach.find(BB); It != Reach.end())
            return It->second;
          Reach[BB] = 2;
          if (llvm::pred_empty(BB))
            return 2;
          for (const llvm::BasicBlock *Pred : llvm::predecessors(BB)) {
            if (const llvm::StoreInst *Last = LastStore(Pred)) {
              if (Last != ThenStore && Last != ElseStore)
                return 2;
            } else if (Carry(Pred) != 1) {
              return 2;
            }
          }
          Reach[BB] = 1;
          return 1;
        };
        const llvm::LoadInst *UseLoad = nullptr;
        int Observed = 0;
        for (const llvm::User *U : ThenSlot->users()) {
          const auto *LI = llvm::dyn_cast<llvm::LoadInst>(U);
          if (!LI || LI->getParent() == Then || LI->getParent() == Else)
            continue;
          bool Killed = false;
          for (const llvm::Instruction &Local : *LI->getParent()) {
            if (&Local == LI)
              break;
            const auto *SI = llvm::dyn_cast<llvm::StoreInst>(&Local);
            if (SI && asAllocaPointer(SI->getPointerOperand()) == ThenSlot)
              Killed = true;
          }
          if (Killed || Carry(LI->getParent()) != 1)
            continue;
          std::function<int(const llvm::Value *)> LiveUsers =
              [&](const llvm::Value *V) -> int {
            int Count = 0;
            for (const llvm::User *LU : V->users()) {
              const auto *UserSI = llvm::dyn_cast<llvm::StoreInst>(LU);
              if (UserSI && UserSI->getValueOperand() == V &&
                  !instructionIsPrinted(*UserSI))
                continue;
              if (llvm::isa<llvm::CastInst, llvm::FreezeInst>(LU)) {
                Count += LiveUsers(llvm::cast<llvm::Value>(LU));
                continue;
              }
              ++Count;
            }
            return Count;
          };
          if (LiveUsers(LI) != 1)
            continue;
          ++Observed;
          UseLoad = LI;
        }
        auto ArmText = [&](const llvm::BasicBlock *Arm,
                           const llvm::StoreInst *Store) {
          const auto Saved = AllocaLastValues;
          AllocaLastValues.clear();
          for (const llvm::Instruction &Local : *Arm) {
            if (&Local == Store)
              break;
            const auto *SI = llvm::dyn_cast<llvm::StoreInst>(&Local);
            if (!SI)
              continue;
            if (const llvm::AllocaInst *Slot =
                    asAllocaPointer(SI->getPointerOperand()))
              AllocaLastValues[Slot] = SI->getValueOperand();
          }
          std::string Text = valueStr(Store->getValueOperand());
          AllocaLastValues = Saved;
          return Text;
        };
        const std::string Cond = condStr(Br->getCondition());
        const std::string ThenText = ArmText(Then, ThenStore);
        const std::string ElseText = ArmText(Else, ElseStore);
        if (!Cond.empty() && !ThenText.empty() && !ElseText.empty()) {
          const std::string Ternary =
              "(" + Cond + " ? " + ThenText + " : " + ElseText + ")";
          auto NoteLocal = [&](const llvm::Instruction &Local) {
            if (auto It = ValNames.find(&Local); It != ValNames.end())
              FoldedLocalNames.push_back(It->second);
          };
          for (const llvm::Instruction &Local : *Then)
            NoteLocal(Local);
          for (const llvm::Instruction &Local : *Else)
            NoteLocal(Local);
          if (Observed == 1 && UseLoad)
            SelectUseText[UseLoad] = Ternary;
          else {
            emitIndent(Indent);
            OS << getName(ThenSlot) << " = " << Ternary << ";\n";
          }
          if (Observed == 1)
            FoldedLocalNames.push_back(getName(ThenSlot));
          FoldedJoinArms.insert(Then);
          FoldedJoinArms.insert(Else);
          if (!joinPrintsNext(From, ThenJoin))
            writeGoto(From, ThenJoin, Indent);
          return;
        }
      }
      auto WriteArm = [&](const llvm::BasicBlock *Arm) {
        for (llvm::Instruction &ArmInst :
             *const_cast<llvm::BasicBlock *>(Arm)) {
          if (ArmInst.isTerminator())
            break;
          writeInstruction(ArmInst, Indent + 1);
        }
        // Folding the jump into an if/else does not remove its SSA edge.
        // Publish each arm's incoming values before the shared join runs.
        writePhiCopies(Arm, ThenJoin, Indent + 1);
      };
      emitIndent(Indent);
      OS << "if (" << condStr(Br->getCondition()) << ") {\n";
      WriteArm(Then);
      emitIndent(Indent);
      OS << "} else {\n";
      WriteArm(Else);
      emitIndent(Indent);
      OS << "}\n";
      FoldedJoinArms.insert(Then);
      FoldedJoinArms.insert(Else);
      if (!joinPrintsNext(From, ThenJoin))
        writeGoto(From, ThenJoin, Indent);
      return;
    }
    {
      llvm::SmallVector<const llvm::BasicBlock *, 4> SkipHeaders;
      llvm::SmallVector<const llvm::BasicBlock *, 4> SkipArms;
      llvm::SmallVector<char, 4> SkipOnTrue;
      llvm::SmallVector<const llvm::BasicBlock *, 32> SkipOwned;
      llvm::SmallVector<const llvm::BasicBlock *, 8> SkipConds;
      llvm::SmallVector<char, 8> SkipTaken;
      llvm::SmallVector<unsigned, 4> SkipCounts;
      llvm::SmallVector<const llvm::BasicBlock *, 4> SkipElided;
      const llvm::BasicBlock *SkipTail = nullptr;
      if (exclusiveSkipElseIf(From, SkipHeaders, SkipArms, SkipOnTrue, SkipTail,
                              SkipOwned, SkipConds, SkipTaken, SkipCounts,
                              SkipElided) &&
          writeExclusiveSkip(SkipHeaders, SkipArms, SkipOnTrue, SkipTail,
                             SkipOwned, SkipConds, SkipTaken, SkipCounts,
                             SkipElided, Indent))
        return;
    }
    // The false edge is the next printed block and carries no phi copy.
    if (Else && Then != Else && joinPrintsNext(From, Else) &&
        !edgePrintsPhiCopy(From, Else)) {
      if (straightLineFalseSkip(From, Else)) {
        writeInvertedFalseSkip(From, Else, Indent);
        return;
      }
      std::string Cond = condStr(Br->getCondition());
      std::set<std::string> SeenConds;
      SeenConds.insert(Cond);
      const llvm::BasicBlock *TrueTarget = printBranchTarget(Then);
      const llvm::BasicBlock *SelectHead = Else;
      llvm::SmallPtrSet<const llvm::BasicBlock *, 8> OrSkips;
      if (!straightLineTrueArm(From, Then, true) && TrueTarget) {
        const llvm::BasicBlock *Pred = From;
        const llvm::BasicBlock *Next = Else;
        while (pureSameTargetSkip(Next, TrueTarget, Pred)) {
          const auto *NextBr =
              llvm::cast<llvm::CondBrInst>(Next->getTerminator());
          // This block has not been printed, so AllocaLastValues still
          // holds the previous block. Preview the condition after only
          // this block's alloca stores, then restore the map.
          const auto SavedLast = AllocaLastValues;
          AllocaLastValues.clear();
          for (const llvm::Instruction &Local : *Next) {
            const auto *SI = llvm::dyn_cast<llvm::StoreInst>(&Local);
            if (!SI)
              continue;
            if (const llvm::AllocaInst *Slot =
                    asAllocaPointer(SI->getPointerOperand()))
              AllocaLastValues[Slot] = SI->getValueOperand();
          }
          const std::string NextCond = condStr(NextBr->getCondition());
          AllocaLastValues = SavedLast;
          if (NextCond.empty() || !SeenConds.insert(NextCond).second)
            break;
          bool MentionsLocal = false;
          for (const llvm::Instruction &Local : *Next) {
            if (Local.getType()->isVoidTy())
              continue;
            const std::string Name =
                getName(const_cast<llvm::Instruction *>(&Local));
            if (Name.empty())
              continue;
            size_t Pos = 0;
            while ((Pos = NextCond.find(Name, Pos)) != std::string::npos) {
              const bool Left =
                  Pos == 0 || (!std::isalnum(static_cast<unsigned char>(
                                   NextCond[Pos - 1])) &&
                               NextCond[Pos - 1] != '_');
              const size_t End = Pos + Name.size();
              const bool Right =
                  End == NextCond.size() ||
                  (!std::isalnum(static_cast<unsigned char>(NextCond[End])) &&
                   NextCond[End] != '_');
              if (Left && Right) {
                MentionsLocal = true;
                break;
              }
              Pos = End;
            }
            if (MentionsLocal)
              break;
          }
          if (MentionsLocal)
            break;
          Cond = orSkipConds(Cond, NextCond);
          InlinedFallthroughBlocks.insert(Next);
          OrSkips.insert(Next);
          Pred = Next;
          Next = NextBr->getSuccessor(1);
          if (!joinPrintsNext(Pred, Next) || edgePrintsPhiCopy(Pred, Next))
            break;
        }
        SelectHead = Next;
      }
      if (!InlinedFallthroughBlocks.count(Else)) {
        if (const llvm::BasicBlock *And = andRejoinTest(From, Else)) {
          const auto *AndBr =
              llvm::cast<llvm::CondBrInst>(And->getTerminator());
          std::string Inv;
          if (std::optional<std::string> Swapped =
                  invertedRelationalText(Br->getCondition()))
            Inv = std::move(*Swapped);
          else if (Cond.size() >= 3 && Cond[0] == '!' && Cond[1] == '(' &&
                   Cond.back() == ')')
            Inv = Cond.substr(2, Cond.size() - 3);
          else
            Inv = "!(" + Cond + ")";
          const auto SavedLast = AllocaLastValues;
          AllocaLastValues.clear();
          for (const llvm::Instruction &Local : *And) {
            const auto *SI = llvm::dyn_cast<llvm::StoreInst>(&Local);
            if (!SI)
              continue;
            if (const llvm::AllocaInst *Slot =
                    asAllocaPointer(SI->getPointerOperand()))
              AllocaLastValues[Slot] = SI->getValueOperand();
          }
          const std::string AndCond = condStr(AndBr->getCondition());
          AllocaLastValues = SavedLast;
          bool MentionsLocal = false;
          for (const llvm::Instruction &Local : *And) {
            if (Local.getType()->isVoidTy())
              continue;
            const std::string Name =
                getName(const_cast<llvm::Instruction *>(&Local));
            if (Name.empty())
              continue;
            size_t Pos = 0;
            while ((Pos = AndCond.find(Name, Pos)) != std::string::npos) {
              const bool Left =
                  Pos == 0 || (!std::isalnum(static_cast<unsigned char>(
                                   AndCond[Pos - 1])) &&
                               AndCond[Pos - 1] != '_');
              const size_t End = Pos + Name.size();
              const bool Right =
                  End == AndCond.size() ||
                  (!std::isalnum(static_cast<unsigned char>(AndCond[End])) &&
                   AndCond[End] != '_');
              if (Left && Right) {
                MentionsLocal = true;
                break;
              }
              Pos = End;
            }
            if (MentionsLocal)
              break;
          }
          if (!Inv.empty() && !AndCond.empty() && !MentionsLocal) {
            auto Group = [](const std::string &S) {
              if (S.find("||") != std::string::npos ||
                  S.find("&&") != std::string::npos)
                return "(" + S + ")";
              return S;
            };
            InlinedFallthroughBlocks.insert(And);
            emitIndent(Indent);
            OS << "if (" << Group(Inv) << " && " << Group(AndCond) << ") {\n";
            writeCondTrueEdge(And, AndBr->getSuccessor(0), Indent + 1);
            emitIndent(Indent);
            OS << "}\n";
            return;
          }
        }
      }
      llvm::SmallVector<const llvm::BasicBlock *, 8> ReleaseRegion;
      const llvm::BasicBlock *ReleaseJoin = nullptr;
      if (sameTargetReleaseRegion(From, Else, ReleaseRegion, ReleaseJoin)) {
        std::string Shown = condStr(Br->getCondition());
        if (std::optional<std::string> Swapped =
                invertedRelationalText(Br->getCondition()))
          Shown = std::move(*Swapped);
        else if (std::optional<std::string> Peeled = peelWrappedNot(Shown))
          Shown = std::move(*Peeled);
        else if (!Shown.empty())
          Shown = "!(" + Shown + ")";
        if (!Shown.empty()) {
          llvm::SmallPtrSet<const llvm::BasicBlock *, 16> ReleaseSet(
              ReleaseRegion.begin(), ReleaseRegion.end());
          emitIndent(Indent);
          OS << "if (" << Shown << ") {\n";
          for (const llvm::BasicBlock &RegionBB : *From->getParent()) {
            if (!ReleaseSet.count(&RegionBB) ||
                InlinedFallthroughBlocks.count(&RegionBB) ||
                FoldedJoinArms.count(&RegionBB) ||
                ConditionChainBodies.count(&RegionBB) ||
                DuplicatedAssignBlocks.count(&RegionBB) ||
                isPrintPassthrough(&RegionBB))
              continue;
            const auto *TermBr =
                llvm::dyn_cast<llvm::UncondBrInst>(RegionBB.getTerminator());
            const bool ExitsToJoin =
                TermBr &&
                printBranchTarget(TermBr->getSuccessor(0)) == ReleaseJoin;
            if (!ExitsToJoin) {
              writeBasicBlock(RegionBB, Indent + 1);
              continue;
            }
            AfterCxxThrow = false;
            {
              decltype(KnownImmediates) Keep;
              for (const llvm::Instruction &RegionInst : RegionBB) {
                const auto *Phi = llvm::dyn_cast<llvm::PHINode>(&RegionInst);
                if (!Phi)
                  break;
                if (auto It = KnownImmediates.find(Phi);
                    It != KnownImmediates.end())
                  Keep[Phi] = It->second;
              }
              KnownImmediates.swap(Keep);
              AllocaImmediates.clear();
              AllocaLastValues.clear();
            }
            if (ReferencedBlocks.count(&RegionBB))
              OS << blockLabel(&RegionBB) << ":\n";
            for (llvm::Instruction &RegionInst :
                 *const_cast<llvm::BasicBlock *>(&RegionBB)) {
              if (&RegionInst == RegionBB.getTerminator() ||
                  llvm::isa<llvm::AllocaInst>(&RegionInst))
                continue;
              writeInstruction(RegionInst, Indent + 1);
            }
          }
          emitIndent(Indent);
          OS << "}\n";
          for (const llvm::BasicBlock *RegionBB : ReleaseRegion)
            InlinedFallthroughBlocks.insert(RegionBB);
          if (!joinPrintsNext(From, ReleaseJoin))
            writeGoto(From, Then, Indent);
          return;
        }
      }
      llvm::SmallVector<const llvm::BasicBlock *, 4> JoinConds;
      llvm::SmallVector<const llvm::BasicBlock *, 4> JoinEdges;
      llvm::SmallVector<const llvm::BasicBlock *, 8> JoinSkip;
      const llvm::BasicBlock *JoinElse = nullptr;
      const llvm::BasicBlock *JoinTail = nullptr;
      if (privateJoinElseIf(From, JoinConds, JoinEdges, JoinElse, JoinTail,
                            JoinSkip)) {
        auto CondOf = [&](const llvm::BasicBlock *Header) {
          const auto *HeaderBr =
              llvm::cast<llvm::CondBrInst>(Header->getTerminator());
          if (Header == From)
            return condStr(HeaderBr->getCondition());
          const auto SavedLast = AllocaLastValues;
          const auto SavedImm = AllocaImmediates;
          AllocaLastValues.clear();
          AllocaImmediates.clear();
          for (const llvm::Instruction &Local : *Header) {
            const auto *SI = llvm::dyn_cast<llvm::StoreInst>(&Local);
            if (!SI)
              continue;
            if (const llvm::AllocaInst *Slot =
                    asAllocaPointer(SI->getPointerOperand()))
              AllocaLastValues[Slot] = SI->getValueOperand();
          }
          std::string Text = condStr(HeaderBr->getCondition());
          AllocaLastValues = SavedLast;
          AllocaImmediates = SavedImm;
          return Text;
        };
        auto WriteOpen = [&](const llvm::BasicBlock *Header, int Depth) {
          AfterCxxThrow = false;
          {
            decltype(KnownImmediates) Keep;
            for (const llvm::Instruction &Local : *Header) {
              const auto *Phi = llvm::dyn_cast<llvm::PHINode>(&Local);
              if (!Phi)
                break;
              if (auto It = KnownImmediates.find(Phi);
                  It != KnownImmediates.end())
                Keep[Phi] = It->second;
            }
            KnownImmediates.swap(Keep);
            AllocaImmediates.clear();
            AllocaLastValues.clear();
          }
          for (llvm::Instruction &Local :
               *const_cast<llvm::BasicBlock *>(Header)) {
            if (&Local == Header->getTerminator() ||
                llvm::isa<llvm::AllocaInst>(&Local))
              continue;
            writeInstruction(Local, Depth);
          }
        };
        std::string TopCond = CondOf(JoinConds[0]);
        llvm::SmallVector<std::string, 4> LaterCond;
        bool CondOk = !TopCond.empty();
        for (size_t I = 1; I < JoinConds.size() && CondOk; ++I) {
          LaterCond.push_back(CondOf(JoinConds[I]));
          CondOk = !LaterCond.back().empty();
        }
        if (CondOk) {
          for (const llvm::BasicBlock *Block : JoinSkip)
            InlinedFallthroughBlocks.insert(Block);
          emitIndent(Indent);
          OS << "if (" << TopCond << ") {\n";
          writeCondTrueEdge(JoinConds[0], JoinEdges[0], Indent + 1);
          emitIndent(Indent);
          OS << "} else {\n";
          int Depth = Indent + 1;
          for (size_t I = 1; I < JoinConds.size(); ++I) {
            const auto *PrevBr =
                llvm::cast<llvm::CondBrInst>(JoinConds[I - 1]->getTerminator());
            const llvm::BasicBlock *Hop = PrevBr->getSuccessor(1);
            llvm::SmallPtrSet<const llvm::BasicBlock *, 8> HopSeen;
            while (Hop && Hop != JoinConds[I] && HopSeen.insert(Hop).second) {
              for (llvm::Instruction &Local :
                   *const_cast<llvm::BasicBlock *>(Hop)) {
                if (Local.isTerminator() || llvm::isa<llvm::AllocaInst>(&Local))
                  continue;
                writeInstruction(Local, Depth);
              }
              const auto *HopBr =
                  llvm::dyn_cast<llvm::UncondBrInst>(Hop->getTerminator());
              if (!HopBr)
                break;
              Hop = HopBr->getSuccessor(0);
            }
            WriteOpen(JoinConds[I], Depth);
            emitIndent(Depth);
            OS << "if (" << LaterCond[I - 1] << ") {\n";
            writeCondTrueEdge(JoinConds[I], JoinEdges[I], Depth + 1);
            emitIndent(Depth);
            OS << "} else {\n";
            ++Depth;
          }
          llvm::SmallVector<const llvm::BasicBlock *, 4> ElseChain;
          if (singleEntryUncondTail(JoinConds.back(), JoinConds.back(),
                                    JoinElse, ElseChain)) {
            for (const llvm::BasicBlock *Block : ElseChain) {
              const auto SavedLast = AllocaLastValues;
              AllocaLastValues.clear();
              for (llvm::Instruction &Local :
                   *const_cast<llvm::BasicBlock *>(Block)) {
                if (Local.isTerminator() || llvm::isa<llvm::AllocaInst>(&Local))
                  continue;
                writeInstruction(Local, Depth);
              }
              AllocaLastValues = SavedLast;
            }
          }
          while (Depth > Indent) {
            --Depth;
            emitIndent(Depth);
            OS << "}\n";
          }
          if (joinPrintsNext(From, JoinTail))
            ReferencedBlocks.erase(JoinTail);
          else
            writeGoto(From, JoinTail, Indent);
          return;
        }
      }
      llvm::SmallVector<const llvm::BasicBlock *, 32> SkipRegion;
      const llvm::BasicBlock *SkipTarget = nullptr;
      if (regionBeforeSkipTarget(From, Else, SkipRegion, SkipTarget)) {
        std::string Shown = Cond;
        if (std::optional<std::string> Peeled = peelWrappedNot(Shown))
          Shown = std::move(*Peeled);
        else if (!Shown.empty())
          Shown = "!(" + Shown + ")";
        if (!Shown.empty()) {
          llvm::SmallPtrSet<const llvm::BasicBlock *, 32> SkipSet(
              SkipRegion.begin(), SkipRegion.end());
          llvm::SmallVector<const llvm::BasicBlock *, 8> RestoreInline;
          llvm::SmallVector<const llvm::BasicBlock *, 8> RestoreDup;
          llvm::SmallVector<const llvm::BasicBlock *, 8> RestoreChain;
          llvm::SmallVector<const llvm::BasicBlock *, 8> RestoreFolded;
          for (const llvm::BasicBlock *Block : SkipRegion) {
            if (InlinedFallthroughBlocks.erase(Block))
              RestoreInline.push_back(Block);
            if (DuplicatedAssignBlocks.erase(Block))
              RestoreDup.push_back(Block);
            if (ConditionChainBodies.erase(Block))
              RestoreChain.push_back(Block);
            if (FoldedJoinArms.erase(Block))
              RestoreFolded.push_back(Block);
          }
          emitIndent(Indent);
          OS << "if (" << Shown << ") {\n";
          for (const llvm::BasicBlock *Block : SkipRegion) {
            const llvm::BasicBlock *PhiLatch = nullptr;
            const llvm::BasicBlock *PhiStep = nullptr;
            const llvm::AllocaInst *PhiSlot = nullptr;
            if (!splitPhiCursorLoop(Block, PhiLatch, PhiStep, PhiSlot))
              continue;
            if (PhiLatch && SkipSet.count(PhiLatch))
              InlinedFallthroughBlocks.insert(PhiLatch);
            if (PhiStep && SkipSet.count(PhiStep))
              InlinedFallthroughBlocks.insert(PhiStep);
          }
          for (const llvm::BasicBlock &RegionBB : *From->getParent()) {
            if (SkipSet.count(&RegionBB))
              writeBasicBlock(RegionBB, Indent + 1);
          }
          emitIndent(Indent);
          OS << "}\n";
          for (const llvm::BasicBlock *Block : SkipRegion)
            InlinedFallthroughBlocks.insert(Block);
          for (const llvm::BasicBlock *Block : RestoreDup)
            DuplicatedAssignBlocks.insert(Block);
          for (const llvm::BasicBlock *Block : RestoreChain)
            ConditionChainBodies.insert(Block);
          for (const llvm::BasicBlock *Block : RestoreFolded)
            FoldedJoinArms.insert(Block);
          (void)RestoreInline;
          const bool PrintedInside =
              InlinedFallthroughBlocks.count(SkipTarget) ||
              FoldedJoinArms.count(SkipTarget) ||
              DuplicatedAssignBlocks.count(SkipTarget) ||
              ConditionChainBodies.count(SkipTarget) ||
              SkipSet.count(SkipTarget);
          if (!PrintedInside && joinPrintsNext(From, SkipTarget))
            ReferencedBlocks.erase(SkipTarget);
          else if (!PrintedInside)
            writeGoto(From, SkipTarget, Indent);
          return;
        }
      }
      llvm::SmallVector<const llvm::BasicBlock *, 16> JoinRegion;
      llvm::SmallVector<const llvm::BasicBlock *, 4> JoinSide;
      const llvm::BasicBlock *JoinArm = nullptr;
      const llvm::BasicBlock *JoinTarget = nullptr;
      if (!Cond.empty() &&
          laterTrueArmJoinsFallthrough(From, Else, JoinRegion, JoinSide,
                                       JoinArm, JoinTarget)) {
        llvm::SmallPtrSet<const llvm::BasicBlock *, 16> JoinSet(
            JoinRegion.begin(), JoinRegion.end());
        for (const llvm::BasicBlock *Block : JoinRegion)
          InlinedFallthroughBlocks.erase(Block);
        emitIndent(Indent);
        OS << "if (" << Cond << ") {\n";
        for (llvm::Instruction &ArmInst :
             *const_cast<llvm::BasicBlock *>(JoinArm)) {
          if (ArmInst.isTerminator() || llvm::isa<llvm::AllocaInst>(&ArmInst))
            continue;
          writeInstruction(ArmInst, Indent + 1);
        }
        emitIndent(Indent);
        OS << "} else {\n";
        for (const llvm::BasicBlock &RegionBB : *From->getParent()) {
          if (JoinSet.count(&RegionBB))
            writeBasicBlock(RegionBB, Indent + 1);
        }
        emitIndent(Indent);
        OS << "}\n";
        for (const llvm::BasicBlock *Block : JoinRegion)
          InlinedFallthroughBlocks.insert(Block);
        for (const llvm::BasicBlock *Block : JoinSide)
          InlinedFallthroughBlocks.insert(Block);
        InlinedFallthroughBlocks.insert(JoinArm);
        ReferencedBlocks.erase(JoinTarget);
        return;
      }
      // A one-line default assign that the skip and the else-if both reach
      // prints on each of those edges. An outside entry keeps the goto.
      if (!straightLineTrueArm(From, Then, true) && SelectHead && TrueTarget &&
          !InlinedFallthroughBlocks.count(SelectHead)) {
        // The select is printed later with a cleared store map. Matching it
        // here has to use that same map, or a copy store looks live and the
        // default arm is rejected.
        const auto SavedLast = AllocaLastValues;
        const auto SavedImm = AllocaImmediates;
        AllocaLastValues.clear();
        AllocaImmediates.clear();
        const auto *HeadBr =
            llvm::dyn_cast<llvm::CondBrInst>(SelectHead->getTerminator());
        const llvm::BasicBlock *HeadElse =
            HeadBr ? HeadBr->getSuccessor(1) : nullptr;
        const llvm::BasicBlock *HeadBody =
            HeadElse ? straightLineFalseSkip(SelectHead, HeadElse) : nullptr;
        llvm::SmallVector<const llvm::BasicBlock *, 4> SelConds;
        llvm::SmallVector<const llvm::BasicBlock *, 4> SelArms;
        const llvm::BasicBlock *SelDef = nullptr;
        const llvm::BasicBlock *SelJoin = nullptr;
        auto OneAssign = [&](const llvm::BasicBlock *BB) {
          if (!BB)
            return false;
          const auto *Go =
              llvm::dyn_cast<llvm::UncondBrInst>(BB->getTerminator());
          if (!Go || !Go->getSuccessor(0) || Go->getSuccessor(0) == BB)
            return false;
          unsigned Printed = 0;
          bool Store = false;
          for (const llvm::Instruction &Inst : *BB) {
            if (&Inst == Go)
              break;
            if (!instructionIsPrinted(Inst))
              continue;
            ++Printed;
            Store = llvm::isa<llvm::StoreInst>(Inst);
          }
          return Printed == 1 && Store;
        };
        auto OriginOk = [&](const llvm::BasicBlock *Pred) {
          llvm::SmallPtrSet<const llvm::BasicBlock *, 8> Seen;
          std::function<bool(const llvm::BasicBlock *)> Walk =
              [&](const llvm::BasicBlock *Cur) -> bool {
            if (!Cur || !Seen.insert(Cur).second)
              return false;
            if (isPrintPassthrough(Cur)) {
              if (Cur->getSinglePredecessor())
                return Walk(Cur->getSinglePredecessor());
              if (llvm::pred_empty(Cur))
                return false;
              for (const llvm::BasicBlock *P : llvm::predecessors(Cur))
                if (!Walk(P))
                  return false;
              return true;
            }
            auto InChain = [&](const llvm::BasicBlock *Edge) {
              return Edge == From || Edge == SelectHead ||
                     OrSkips.count(Edge) || llvm::is_contained(SelConds, Edge);
            };
            if (const auto *Br =
                    llvm::dyn_cast<llvm::CondBrInst>(Cur->getTerminator())) {
              const llvm::BasicBlock *TrueT =
                  printBranchTarget(Br->getSuccessor(0));
              const llvm::BasicBlock *FalseT =
                  printBranchTarget(Br->getSuccessor(1));
              // An earlier test of this same default, whose other edge is
              // the chain, is not a third entry.
              if ((TrueT == SelDef && InChain(FalseT)) ||
                  (FalseT == SelDef && InChain(TrueT)))
                return true;
            }
            return InChain(Cur);
          };
          return Walk(Pred);
        };
        bool PredsOk = false;
        if (HeadBody &&
            assignSelectElseIf(SelectHead, HeadBody, SelConds, SelArms, SelDef,
                               SelJoin) &&
            SelDef == TrueTarget && OneAssign(SelDef)) {
          PredsOk = !llvm::pred_empty(SelDef);
          for (const llvm::BasicBlock *Pred : llvm::predecessors(SelDef)) {
            if (!OriginOk(Pred)) {
              PredsOk = false;
              break;
            }
          }
        }
        if (PredsOk) {
          ReferencedBlocks.erase(SelDef);
          emitIndent(Indent);
          OS << "if (" << Cond << ") {\n";
          for (llvm::Instruction &DefInst :
               *const_cast<llvm::BasicBlock *>(SelDef)) {
            if (DefInst.isTerminator() || llvm::isa<llvm::AllocaInst>(&DefInst))
              continue;
            writeInstruction(DefInst, Indent + 1);
          }
          emitIndent(Indent);
          OS << "} else {\n";
          writeInvertedFalseSkip(SelectHead, HeadElse, Indent + 1);
          emitIndent(Indent);
          OS << "}\n";
          InlinedFallthroughBlocks.insert(SelectHead);
          AllocaLastValues = SavedLast;
          AllocaImmediates = SavedImm;
          return;
        }
        AllocaLastValues = SavedLast;
        AllocaImmediates = SavedImm;
      }
      emitIndent(Indent);
      OS << "if (" << Cond << ") {\n";
      writeCondTrueEdge(From, Then, Indent + 1);
      emitIndent(Indent);
      OS << "}\n";
      return;
    }
    if (const llvm::BasicBlock *Body = elseBodyFallsIntoNext(From, Else)) {
      emitIndent(Indent);
      OS << "if (" << condStr(Br->getCondition()) << ") {\n";
      // The false body's statements will be moved before the layout's next
      // block. The true edge must jump past them even when its join was that
      // next block in the original layout.
      writeCondTrueEdge(From, Then, Indent + 1, false);
      emitIndent(Indent);
      OS << "}\n";
      for (llvm::Instruction &BodyInst :
           *const_cast<llvm::BasicBlock *>(Body)) {
        if (BodyInst.isTerminator() || llvm::isa<llvm::AllocaInst>(&BodyInst))
          continue;
        writeInstruction(BodyInst, Indent);
      }
      // Moving the false arm next to its join removes only the printed jump.
      // Its outgoing edge must still publish PHI values, including loop seeds.
      const auto *Go = llvm::cast<llvm::UncondBrInst>(Body->getTerminator());
      writeGoto(Body, Go->getSuccessor(0), Indent, false);
      InlinedFallthroughBlocks.insert(Body);
      return;
    }
    // A conditional block laid out after the shared exit can still be the
    // false edge: one arm is that exit, and the other is the next statement,
    // which itself falls into the exit. Print it here, with that statement
    // inside the arm that reaches it.
    if (Else && Else->getSinglePredecessor() == From &&
        !edgePrintsPhiCopy(From, Else)) {
      const auto *LateBr =
          llvm::dyn_cast<llvm::CondBrInst>(Else->getTerminator());
      if (LateBr && !edgePrintsPhiCopy(Else, LateBr->getSuccessor(0)) &&
          !edgePrintsPhiCopy(Else, LateBr->getSuccessor(1))) {
        const llvm::BasicBlock *TrueT =
            printBranchTarget(LateBr->getSuccessor(0));
        const llvm::BasicBlock *FalseT =
            printBranchTarget(LateBr->getSuccessor(1));
        const llvm::BasicBlock *WorkBB = nullptr;
        bool WorkOnFalse = false;
        auto IsWork = [&](const llvm::BasicBlock *Edge, bool OnFalse) {
          if (!Edge || Edge->getSinglePredecessor() != Else ||
              !joinPrintsNext(From, Edge))
            return false;
          const auto *WorkBr =
              llvm::dyn_cast<llvm::UncondBrInst>(Edge->getTerminator());
          if (!WorkBr || edgePrintsPhiCopy(Else, Edge) ||
              edgePrintsPhiCopy(Edge, WorkBr->getSuccessor(0)))
            return false;
          const llvm::BasicBlock *Tail =
              printBranchTarget(WorkBr->getSuccessor(0));
          const llvm::BasicBlock *Other = OnFalse ? TrueT : FalseT;
          return Tail && Other == Tail;
        };
        if (IsWork(FalseT, true)) {
          WorkBB = LateBr->getSuccessor(1);
          WorkOnFalse = true;
        } else if (IsWork(TrueT, false)) {
          WorkBB = LateBr->getSuccessor(0);
        }
        if (WorkBB) {
          auto InvertCond = [](std::string Cond) {
            if (Cond.size() >= 3 && Cond[0] == '!' && Cond[1] == '(' &&
                Cond.back() == ')')
              return Cond.substr(2, Cond.size() - 3);
            return "!(" + Cond + ")";
          };
          std::string LateCond = condStr(LateBr->getCondition());
          if (WorkOnFalse)
            LateCond = InvertCond(std::move(LateCond));
          if (!LateCond.empty()) {
            emitIndent(Indent);
            OS << "if (" << condStr(Br->getCondition()) << ") {\n";
            writeCondTrueEdge(From, Then, Indent + 1);
            emitIndent(Indent);
            OS << "}\n";
            for (llvm::Instruction &LateInst :
                 *const_cast<llvm::BasicBlock *>(Else)) {
              if (LateInst.isTerminator() ||
                  llvm::isa<llvm::AllocaInst>(&LateInst))
                continue;
              writeInstruction(LateInst, Indent);
            }
            emitIndent(Indent);
            OS << "if (" << LateCond << ") {\n";
            for (llvm::Instruction &WorkInst :
                 *const_cast<llvm::BasicBlock *>(WorkBB)) {
              if (WorkInst.isTerminator() ||
                  llvm::isa<llvm::AllocaInst>(&WorkInst))
                continue;
              writeInstruction(WorkInst, Indent + 1);
            }
            emitIndent(Indent);
            OS << "}\n";
            InlinedFallthroughBlocks.insert(Else);
            InlinedFallthroughBlocks.insert(WorkBB);
            return;
          }
        }
        // One arm is the next printed block. Print this block, branch to
        // the other arm, and let that next block follow.
        const llvm::BasicBlock *FallEdge = nullptr;
        const llvm::BasicBlock *OtherEdge = nullptr;
        bool FallIsTrue = false;
        auto IsFall = [&](const llvm::BasicBlock *Edge) {
          if (!Edge || Edge == Else || Edge->getSinglePredecessor() != Else ||
              edgePrintsPhiCopy(Else, Edge))
            return false;
          return joinPrintsNext(From, Edge);
        };
        if (IsFall(LateBr->getSuccessor(1)) &&
            !IsFall(LateBr->getSuccessor(0))) {
          FallEdge = LateBr->getSuccessor(1);
          OtherEdge = LateBr->getSuccessor(0);
        } else if (IsFall(LateBr->getSuccessor(0)) &&
                   !IsFall(LateBr->getSuccessor(1))) {
          FallEdge = LateBr->getSuccessor(0);
          OtherEdge = LateBr->getSuccessor(1);
          FallIsTrue = true;
        }
        bool TrueExits = true;
        if (const llvm::BasicBlock *Arm =
                straightLineTrueArm(From, Then, true)) {
          const llvm::Instruction *Term = Arm->getTerminator();
          if (const auto *ArmBr = llvm::dyn_cast<llvm::UncondBrInst>(Term))
            TrueExits = !joinPrintsNext(From, ArmBr->getSuccessor(0));
          else
            TrueExits = llvm::isa<llvm::ReturnInst>(Term) ||
                        llvm::isa<llvm::UnreachableInst>(Term);
        }
        if (FallEdge && OtherEdge && TrueExits) {
          auto InvertCond = [](std::string Text) {
            if (Text.size() >= 3 && Text[0] == '!' && Text[1] == '(' &&
                Text.back() == ')')
              return Text.substr(2, Text.size() - 3);
            return "!(" + Text + ")";
          };
          const auto SavedLast = AllocaLastValues;
          AllocaLastValues.clear();
          for (const llvm::Instruction &Local : *Else) {
            const auto *SI = llvm::dyn_cast<llvm::StoreInst>(&Local);
            if (!SI)
              continue;
            if (const llvm::AllocaInst *Slot =
                    asAllocaPointer(SI->getPointerOperand()))
              AllocaLastValues[Slot] = SI->getValueOperand();
          }
          std::string LateCond = condStr(LateBr->getCondition());
          AllocaLastValues = SavedLast;
          if (FallIsTrue)
            LateCond = InvertCond(std::move(LateCond));
          if (!LateCond.empty()) {
            emitIndent(Indent);
            OS << "if (" << condStr(Br->getCondition()) << ") {\n";
            writeCondTrueEdge(From, Then, Indent + 1);
            emitIndent(Indent);
            OS << "}\n";
            for (llvm::Instruction &LateInst :
                 *const_cast<llvm::BasicBlock *>(Else)) {
              if (LateInst.isTerminator() ||
                  llvm::isa<llvm::AllocaInst>(&LateInst))
                continue;
              writeInstruction(LateInst, Indent);
            }
            const llvm::BasicBlock *LoopLatch = nullptr;
            const llvm::BasicBlock *LoopStep = nullptr;
            const llvm::AllocaInst *LoopSlot = nullptr;
            const llvm::BasicBlock *LoopHead = printBranchTarget(FallEdge);
            auto EntersLoop = [&](const llvm::BasicBlock *BB) {
              if (!BB)
                return false;
              if (splitPhiCursorLoop(BB, LoopLatch, LoopStep, LoopSlot))
                return true;
              const auto *Hop =
                  llvm::dyn_cast<llvm::UncondBrInst>(BB->getTerminator());
              return Hop && Hop->getSuccessor(0) &&
                     splitPhiCursorLoop(printBranchTarget(Hop->getSuccessor(0)),
                                        LoopLatch, LoopStep, LoopSlot);
            };
            const bool LoopOwnsNull =
                EntersLoop(LoopHead) || EntersLoop(FallEdge);
            if (!LoopOwnsNull) {
              emitIndent(Indent);
              OS << "if (" << LateCond << ") {\n";
              writeGoto(Else, OtherEdge, Indent + 1);
              emitIndent(Indent);
              OS << "}\n";
            } else {
              ReferencedBlocks.erase(FallEdge);
            }
            InlinedFallthroughBlocks.insert(Else);
            return;
          }
        }
      }
    }
    emitIndent(Indent);
    OS << "if (" << condStr(Br->getCondition()) << ") {\n";
    writeCondTrueEdge(From, Then, Indent + 1);
    if (redundantLoopElseContinue(From, Else)) {
      emitIndent(Indent);
      OS << "}\n";
      return;
    }
    emitIndent(Indent);
    OS << "} else {\n";
    writeGoto(From, Else, Indent + 1);
    emitIndent(Indent);
    OS << "}\n";
    return;
  }

  if (auto *Ret = llvm::dyn_cast<llvm::ReturnInst>(&Inst)) {
    writeReturn(*Ret, Indent);
    return;
  }

  if (auto *SW = llvm::dyn_cast<llvm::SwitchInst>(&Inst)) {
    const llvm::BasicBlock *From = Inst.getParent();
    auto *SelectorType = SW->getCondition()->getType();
    // LLVM switch compares bit patterns. Normalize the condition through the
    // existing unsigned width owner before C's integer promotions, and keep
    // labels in that same domain: sign-extending i2 2 to -2 cannot match a
    // masked uint8_t selector whose values are 0..3.
    const std::string Selector =
        castStr(llvm::Instruction::Trunc, valueStr(SW->getCondition()),
                SelectorType, SelectorType);
    emitIndent(Indent);
    OS << "switch (" << Selector << ") {\n";
    for (auto &C : SW->cases()) {
      const auto *Value = C.getCaseValue();
      // Wide constants already use an unsigned 128-bit carrier. Narrow
      // labels must stay numeric even if their bits resemble an image address.
      const std::string Label =
          Value->getBitWidth() > 64
              ? constStr(Value)
              : std::to_string(Value->getZExtValue()) + "ULL";
      emitIndent(Indent);
      OS << "case " << Label << ": {\n";
      writeGoto(From, C.getCaseSuccessor(), Indent + 1);
      emitIndent(Indent);
      OS << "}\n";
    }
    emitIndent(Indent);
    OS << "default: {\n";
    writeGoto(From, SW->getDefaultDest(), Indent + 1);
    emitIndent(Indent);
    OS << "}\n";
    emitIndent(Indent);
    OS << "}\n";
    return;
  }

  if (auto *Extract = llvm::dyn_cast<llvm::ExtractElementInst>(&Inst)) {
    emitIndent(Indent);
    OS << Name << " = (" << valueStr(Extract->getVectorOperand()) << ")["
       << valueStr(Extract->getIndexOperand()) << "];\n";
    return;
  }

  if (auto *Insert = llvm::dyn_cast<llvm::InsertElementInst>(&Inst)) {
    emitIndent(Indent);
    OS << Name << " = " << valueStr(Insert->getOperand(0)) << ";\n";
    emitIndent(Indent);
    OS << Name << "[" << valueStr(Insert->getOperand(2))
       << "] = " << valueStr(Insert->getOperand(1)) << ";\n";
    return;
  }

  if (auto *EV = llvm::dyn_cast<llvm::ExtractValueInst>(&Inst)) {
    auto *Agg = EV->getAggregateOperand();
    auto It = Analysis.IntrinsicStructNames.find(Agg);
    if (It != Analysis.IntrinsicStructNames.end())
      return;
    emitIndent(Indent);
    OS << Name << " = (" << valueStr(Agg) << ")"
       << aggregateMemberPath(Agg->getType(), EV->getIndices()) << ";\n";
    return;
  }

  if (auto *Insert = llvm::dyn_cast<llvm::InsertValueInst>(&Inst)) {
    emitIndent(Indent);
    OS << Name << " = " << valueStr(Insert->getAggregateOperand()) << ";\n";
    emitIndent(Indent);
    OS << Name << aggregateMemberPath(Insert->getType(), Insert->getIndices())
       << " = " << valueStr(Insert->getInsertedValueOperand()) << ";\n";
    return;
  }

  if (llvm::isa<llvm::PHINode>(&Inst))
    return;

  if (auto *Sel = llvm::dyn_cast<llvm::SelectInst>(&Inst)) {
    emitIndent(Indent);
    if (auto Condition = foldImmediate(Sel->getCondition());
        Condition && (*Condition == "0" || *Condition == "1")) {
      OS << Name << " = "
         << valueStr(*Condition == "1" ? Sel->getTrueValue()
                                       : Sel->getFalseValue())
         << ";\n";
      return;
    }
    OS << Name << " = " << valueStr(Sel->getCondition()) << " ? "
       << valueStr(Sel->getTrueValue()) << " : "
       << valueStr(Sel->getFalseValue()) << ";\n";
    return;
  }

  if (auto *FI = llvm::dyn_cast<llvm::FenceInst>(&Inst)) {
    emitIndent(Indent);
    OS << renderFence(Opts.TheArch, FI->getOrdering());
    HasCIntrinsics = true;
    return;
  }

  if (auto *RMW = llvm::dyn_cast<llvm::AtomicRMWInst>(&Inst)) {
    emitIndent(Indent);
    if (!RMW->getType()->isVoidTy())
      OS << Name << " = ";
    OS << atomicRMWText(*RMW) << ";\n";
    return;
  }

  if (llvm::isa<llvm::UnreachableInst>(&Inst)) {
    emitIndent(Indent);
    OS << "__builtin_unreachable();\n";
    return;
  }

  emitIndent(Indent);
  OS << "/* unhandled: ";
  std::string OpName;
  llvm::raw_string_ostream RSO(OpName);
  Inst.print(RSO);
  OS << OpName << " */\n";
}

bool LLVMCWriter::writeRawMemoryCopy(llvm::Instruction &Inst, int Indent) {
  auto *Load = llvm::dyn_cast<llvm::LoadInst>(&Inst);
  auto *Store = llvm::dyn_cast<llvm::StoreInst>(&Inst);
  if ((!Load || !Load->isSimple() || Load->getPointerAddressSpace() != 0) &&
      (!Store || !Store->isSimple() || Store->getPointerAddressSpace() != 0))
    return false;
  auto *Type = Load ? Load->getType() : Store->getValueOperand()->getType();
  // These carriers have exact C object representations. Partial-width
  // integers and x87 have their separate exact-byte paths.
  const bool Integer = Type->isIntegerTy(8) || Type->isIntegerTy(16) ||
                       Type->isIntegerTy(32) || Type->isIntegerTy(64) ||
                       Type->isIntegerTy(128) || Type->isIntegerTy(256) ||
                       Type->isIntegerTy(512);
  if (!Integer && !Type->isFloatTy() && !Type->isDoubleTy() &&
      !Type->isHalfTy() && !Type->isBFloatTy() && !Type->isPointerTy())
    return false;
  const auto &Layout = CurMod->getDataLayout();
  const auto Size = Layout.getTypeStoreSize(Type);
  const auto *Address =
      Load ? Load->getPointerOperand() : Store->getPointerOperand();
  // Proven source objects are projected before reaching this fallback.
  // LLVM alignment alone does not establish a C effective type, even for an
  // aligned inttoptr address or overlapping views of a byte array.
  if (Size.isScalable())
    return false;
  // Immutable image values have their own projection below. A folded image
  // address need not have a runtime object in the recompiled program.
  if (Load && Integer)
    if (auto VA = imageDataVA(Address))
      if (foldReadonlyScalar(*VA, Size.getFixedValue()))
        return false;
  std::string Pointer = valueStr(Address);
  if (std::string Image = imageDataCName(Address); !Image.empty())
    Pointer = "&" + Image;
  else if (const auto *Global = llvm::dyn_cast<llvm::GlobalVariable>(Address)) {
    // Named globals denote C objects, not addresses. An exact type match
    // permits direct access; differently typed views still copy object bytes.
    if (Global->getValueType() == Type &&
        !ExternalDataIdentifiers.count(Global)) {
      emitIndent(Indent);
      if (Load)
        OS << getName(Load) << " = " << Pointer << ";\n";
      else
        OS << Pointer << " = "
           << (Integer ? integerPointerOperandStr(Store->getValueOperand())
                       : valueStr(Store->getValueOperand()))
           << ";\n";
      return true;
    }
    Pointer = "&" + Pointer;
  }
  emitIndent(Indent);
  if (UnalignedTypesWritten) {
    if (auto Alias = c_memory::alias(typeToCLLVM(Type), Opts.ScalarPointers);
        !Alias.empty()) {
      if (Load)
        OS << getName(Load) << " = " << c_memory::access(Alias, Pointer)
           << ";\n";
      else
        OS << c_memory::access(Alias, Pointer) << " = "
           << (Integer ? integerPointerOperandStr(Store->getValueOperand())
                       : valueStr(Store->getValueOperand()))
           << ";\n";
      return true;
    }
  }
  if (Load) {
    OS << c_memory::loadCopy(getName(Load), Pointer,
                             std::to_string(Size.getFixedValue()))
       << ";\n";
  } else {
    const std::string Value = freshVar("memory_value");
    OS << "{ " << typeToCLLVM(Type) << " " << Value << " = "
       << (Integer ? integerPointerOperandStr(Store->getValueOperand())
                   : valueStr(Store->getValueOperand()))
       << ";\n";
    emitIndent(Indent + 1);
    OS << c_memory::storeCopy(Pointer, Value,
                              std::to_string(Size.getFixedValue()))
       << ";\n";
    emitIndent(Indent);
    OS << "}\n";
  }
  return true;
}

void LLVMCWriter::writeGEP(llvm::GetElementPtrInst &GEP,
                           const std::string &Name, int Indent) {
  if (SyntheticFrame && GEP.getFunction() &&
      functionNeedsAnalysisOnlyEHWrap(*GEP.getFunction())) {
    bool OnlyState = !GEP.use_empty();
    for (const llvm::User *U : GEP.users()) {
      const auto *SI = llvm::dyn_cast<llvm::StoreInst>(U);
      const auto *Init =
          SI ? llvm::dyn_cast<llvm::ConstantInt>(SI->getValueOperand())
             : nullptr;
      if (!SI || SI->getPointerOperand() != &GEP || !Init ||
          Init->getBitWidth() > 64 || Init->getSExtValue() != -2)
        OnlyState = false;
    }
    if (OnlyState) {
      if (const auto Peeled = peelPointerOffset(&GEP);
          Peeled && Peeled->first == SyntheticFrame)
        return;
    }
  }
  emitIndent(Indent);
  OS << Name << " = " << gepExpr(*llvm::cast<llvm::GEPOperator>(&GEP)) << ";\n";
}

bool LLVMCWriter::writeIntrinsicCall(llvm::CallBase &Call, int Indent) {
  const llvm::Function *Callee = Call.getCalledFunction();
  if (!Callee || !Callee->isIntrinsic())
    return false;

  auto IID = Callee->getIntrinsicID();
  if ((IID == llvm::Intrinsic::ctpop || IID == llvm::Intrinsic::ctlz ||
       IID == llvm::Intrinsic::cttz) &&
      Call.getType()->isIntegerTy() &&
      Call.getType()->getIntegerBitWidth() <= 64) {
    const unsigned Bits = Call.getType()->getIntegerBitWidth();
    const std::string Source = freshVar("count_bits");
    emitIndent(Indent);
    OS << "{ uint64_t " << Source << " = (uint64_t)("
       << valueStr(Call.getArgOperand(0)) << ")";
    if (Bits < 64)
      OS << " & UINT64_C(" << ((uint64_t(1) << Bits) - 1) << ")";
    OS << ";\n";
    emitIndent(Indent + 1);
    OS << getName(&Call) << " = ";
    if (IID != llvm::Intrinsic::ctpop)
      OS << Source << " == 0 ? " << Bits << " : ";
    OS << (IID == llvm::Intrinsic::ctpop  ? "__builtin_popcountll("
           : IID == llvm::Intrinsic::ctlz ? "__builtin_clzll("
                                          : "__builtin_ctzll(")
       << Source << ")";
    if (IID == llvm::Intrinsic::ctlz && Bits < 64)
      OS << " - " << 64 - Bits;
    OS << ";\n";
    emitIndent(Indent);
    OS << "}\n";
    return true;
  }
  if (IID == llvm::Intrinsic::aarch64_neon_bfmmla) {
    if (!isNativeVectorIntrinsic(Call, Opts.TheArch))
      throw std::runtime_error("BFMMLA requires an AArch64 C projection");
    emitIndent(Indent);
    OS << getName(&Call) << " = vbfmmlaq_f32("
       << valueStr(Call.getArgOperand(0)) << ", "
       << valueStr(Call.getArgOperand(1)) << ", "
       << valueStr(Call.getArgOperand(2)) << ");\n";
    return true;
  }
  if (IID == llvm::Intrinsic::experimental_constrained_sitofp &&
      Call.getType()->isX86_FP80Ty()) {
    if (Call.arg_size() != 3)
      throw std::runtime_error("invalid constrained x87 integer conversion");
    auto MetadataText = [&](unsigned Index) -> llvm::StringRef {
      const auto *Value =
          llvm::dyn_cast<llvm::MetadataAsValue>(Call.getArgOperand(Index));
      const auto *String =
          Value ? llvm::dyn_cast<llvm::MDString>(Value->getMetadata())
                : nullptr;
      return String ? String->getString() : llvm::StringRef();
    };
    const auto *Input = Call.getArgOperand(0);
    if ((Opts.TheArch != Arch::X86 && Opts.TheArch != Arch::X64) ||
        (!Input->getType()->isIntegerTy(32) &&
         !Input->getType()->isIntegerTy(64)) ||
        MetadataText(1) != "round.dynamic" ||
        (MetadataText(2) != "fpexcept.strict" &&
         MetadataText(2) != "fpexcept.ignore"))
      throw std::runtime_error(
          "unsupported constrained x87 integer conversion");
    const unsigned Bits = Input->getType()->getIntegerBitWidth();
    const std::string IntegerType = "int" + std::to_string(Bits) + "_t";
    const std::string Source = freshVar("x87_integer");
    emitIndent(Indent);
    OS << "{ " << IntegerType << " " << Source << " = (" << IntegerType << ")("
       << valueStr(Input) << ");\n";
    emitIndent(Indent + 1);
    OS << "__asm__ volatile(\"" << (Bits == 32 ? "fildl" : "fildq")
       << " %1\" : \"=t\"(" << getName(&Call) << ") : \"m\"(" << Source
       << "));\n";
    emitIndent(Indent);
    OS << "}\n";
    return true;
  }
  if (IID == llvm::Intrinsic::sqrt && Call.getType()->isX86_FP80Ty()) {
    if (Opts.TheArch != Arch::X86 && Opts.TheArch != Arch::X64)
      throw std::runtime_error("x87 square root requires an x86 C projection");
    emitIndent(Indent);
    OS << "__asm__ volatile(\"fsqrt\" : \"=t\"(" << getName(&Call)
       << ") : \"0\"(" << valueStr(Call.getArgOperand(0)) << "));\n";
    return true;
  }
  if (IID == llvm::Intrinsic::fptosi_sat &&
      Call.getArgOperand(0)->getType()->isX86_FP80Ty()) {
    if (!Call.getType()->isIntegerTy(32) && !Call.getType()->isIntegerTy(64))
      throw std::runtime_error("unsupported saturating x87 integer width");
    const unsigned Bits = Call.getType()->getIntegerBitWidth();
    const std::string Source = freshVar("fp_source");
    const std::string Bound = "0x1p" + std::to_string(Bits - 1) + "L";
    const std::string Suffix = Bits == 64 ? "ULL" : "U";
    const std::string Minimum =
        "0x" + llvm::toString(llvm::APInt::getSignedMinValue(Bits), 16, false) +
        Suffix;
    const std::string Maximum =
        "0x" + llvm::toString(llvm::APInt::getSignedMaxValue(Bits), 16, false) +
        Suffix;
    const std::string Name = getName(&Call);
    emitIndent(Indent);
    OS << "{ long double " << Source << " = " << valueStr(Call.getArgOperand(0))
       << ";\n";
    emitIndent(Indent + 1);
    OS << Name << " = __builtin_isnan(" << Source << ") ? 0 : " << Source
       << " >= " << Bound << " ? " << Maximum << " : " << Source << " < -"
       << Bound << " ? " << Minimum << " : (" << typeToCLLVM(Call.getType())
       << ")(int" << Bits << "_t)" << Source << ";\n";
    emitIndent(Indent);
    OS << "}\n";
    return true;
  }
  if (IID == llvm::Intrinsic::sideeffect || IID == llvm::Intrinsic::donothing ||
      IID == llvm::Intrinsic::seh_try_begin ||
      IID == llvm::Intrinsic::seh_try_end ||
      IID == llvm::Intrinsic::localescape ||
      IID == llvm::Intrinsic::localrecover)
    return true;
  if (IID == llvm::Intrinsic::returnaddress ||
      IID == llvm::Intrinsic::frameaddress) {
    if (Call.arg_size() != 1 ||
        !llvm::isa<llvm::ConstantInt>(Call.getArgOperand(0)) ||
        !llvm::cast<llvm::ConstantInt>(Call.getArgOperand(0))->isZero())
      throw std::runtime_error("unsupported frame/return-address depth");
    emitIndent(Indent);
    OS << getName(&Call) << " = "
       << (IID == llvm::Intrinsic::returnaddress ? "__builtin_return_address"
                                                 : "__builtin_frame_address")
       << "(0);\n";
    return true;
  }
  if (IID == llvm::Intrinsic::localaddress) {
    // Keep the target-specific LLVM intrinsic: it can select SP, FP, or a base
    // pointer depending on stack realignment. __builtin_frame_address differs.
    const std::string IntrinsicName = freshVar("llvm_localaddress");
    emitIndent(Indent);
    OS << "extern void *" << IntrinsicName
       << "(void) __asm__(\"llvm.localaddress\");\n";
    emitIndent(Indent);
    OS << getName(&Call) << " = " << IntrinsicName << "();\n";
    return true;
  }
  if (IID == llvm::Intrinsic::eh_exceptioncode) {
    if (!Call.getType()->isVoidTy()) {
      emitIndent(Indent);
      OS << getName(&Call) << " = GetExceptionCode();\n";
    }
    return true;
  }
  if (IID == llvm::Intrinsic::memcpy || IID == llvm::Intrinsic::memmove) {
    emitIndent(Indent);
    OS << (IID == llvm::Intrinsic::memmove ? "memmove(" : "memcpy(")
       << valueStr(Call.getArgOperand(0)) << ", "
       << valueStr(Call.getArgOperand(1)) << ", "
       << valueStr(Call.getArgOperand(2)) << ");\n";
    return true;
  }
  if (IID == llvm::Intrinsic::memset) {
    emitIndent(Indent);
    OS << "memset(" << valueStr(Call.getArgOperand(0)) << ", "
       << valueStr(Call.getArgOperand(1)) << ", "
       << valueStr(Call.getArgOperand(2)) << ");\n";
    return true;
  }
  if (IID == llvm::Intrinsic::lifetime_start ||
      IID == llvm::Intrinsic::lifetime_end)
    return true;
  if (IID == llvm::Intrinsic::debugtrap) {
    if (AfterCxxThrow)
      return true;
    emitIndent(Indent);
    OS << renderDebugBreak(Opts.TheArch);
    AfterCxxThrow = false;
    return true;
  }
  if (IID == llvm::Intrinsic::trap) {
    if (AfterCxxThrow)
      return true;
    emitIndent(Indent);
    OS << "__builtin_trap();\n";
    AfterCxxThrow = false;
    return true;
  }
  if (IID == llvm::Intrinsic::prefetch) {
    emitIndent(Indent);
    OS << "__builtin_prefetch((void*)" << valueStr(Call.getArgOperand(0))
       << ");\n";
    return true;
  }
  if (IID == llvm::Intrinsic::aarch64_clrex) {
    emitIndent(Indent);
    OS << "__builtin_arm_clrex();\n";
    HasCIntrinsics = true;
    return true;
  }
  return false;
}

bool LLVMCWriter::writeInlineAsmCall(llvm::CallInst &Call,
                                     const std::string &Name, int Indent) {
  auto *IA = llvm::dyn_cast<llvm::InlineAsm>(Call.getCalledOperand());
  if (!IA)
    return false;

  if (auto Width = classifyX86ShadowStackReadAsm(Call)) {
    const unsigned Word = Call.getType()->getIntegerBitWidth() / 8;
    if ((Opts.TheArch != Arch::X86 && Opts.TheArch != Arch::X64) ||
        Word != (Opts.TheArch == Arch::X64 ? 8U : 4U))
      llvm::report_fatal_error("shadow stack read target/type mismatch");
    const std::string Temp = Call.use_empty() ? freshVar("nd_ssp_value") : Name;
    emitIndent(Indent);
    if (Call.use_empty())
      OS << "uint" << Word * 8 << "_t ";
    OS << Temp << " = " << integerPointerOperandStr(Call.getArgOperand(0))
       << ";\n";
    emitIndent(Indent);
    OS << "__asm__ volatile(\""
       << (*Width == 8 ? "rdsspq %0"
           : Word == 8 ? "rdsspd %k0"
                       : "rdsspd %0")
       << "\" : \"+r\"(" << Temp << ") :: \"memory\");\n";
    return true;
  }

  if (auto Shape = classifyX86FPStateAsm(Call)) {
    if (Opts.TheArch != Arch::X86 && Opts.TheArch != Arch::X64)
      llvm::report_fatal_error("x86 FP state C projection requires x86");
    const auto [Id, Bytes] = *Shape;
    emitIndent(Indent);
    if (!Bytes) {
      const std::string Pointer = valueStr(Call.getArgOperand(0));
      OS << "__asm__ volatile(\""
         << (Id == Intrinsic::X86ReadMXCSR ? "stmxcsr" : "ldmxcsr")
         << " (%0)\" :: \"r\"(" << Pointer << ") : \"memory\");\n";
      return true;
    }
    const auto BitcastInput = [&](unsigned Index) {
      return "__builtin_bit_cast(" +
             x86FPStateRawCType(x86FPStateSourceBytes(Bytes)) + ", " +
             valueStr(Call.getArgOperand(Index)) + ")";
    };
    const auto Helper = FPStateHelperNames.find(*Shape);
    if (Helper == FPStateHelperNames.end())
      llvm::report_fatal_error("uncollected x86 FP state C helper");
    if (isX86FPConversionStateIntrinsic(Id)) {
      const std::string Expression = Helper->second + "(" + BitcastInput(0) +
                                     ", (void*)" +
                                     valueStr(Call.getArgOperand(1)) + ")";
      if (!Call.use_empty())
        OS << Name << " = " << Expression << ";\n";
      else
        OS << "(void)" << Expression << ";\n";
      return true;
    }
    if (isX86FPRoundStateIntrinsic(Id)) {
      const std::string Source =
          Id == Intrinsic::X86FPRoundMemoryState
              ? "(void*)(uintptr_t)(" + valueStr(Call.getArgOperand(0)) + ")"
              : BitcastInput(0);
      const std::string Expression = Helper->second + "(" + Source +
                                     ", (void*)" +
                                     valueStr(Call.getArgOperand(1)) + ")";
      if (!Call.use_empty())
        OS << Name << " = __builtin_bit_cast(" << typeToCLLVM(Call.getType())
           << ", " << Expression << ");\n";
      else
        OS << "(void)" << Expression << ";\n";
      return true;
    }
    std::string Expression = Helper->second + "(" + BitcastInput(0) + ", " +
                             BitcastInput(1) + ", (void*)" +
                             valueStr(Call.getArgOperand(2)) + ")";
    if (!Call.use_empty())
      OS << Name << " = __builtin_bit_cast("
         << (Bytes == 4 ? "float" : "double") << ", " << Expression << ");\n";
    else
      OS << "(void)" << Expression << ";\n";
    return true;
  }

  std::string AsmStr = IA->getAsmString().str();
  if (llvm::StringRef(AsmStr).contains("rdssp"))
    llvm::report_fatal_error("unowned shadow stack read C projection");
  if (AsmStr.empty())
    return false;

  bool ResultLive = !Call.getType()->isVoidTy();
  if (ResultLive && InferredVoid)
    ResultLive = isCallResultLive(Analysis, &Call);

  // The native Linux x64 SYSCALL contract has seven scalar register inputs
  // and two register outputs.  Keep r10/r8/r9 and r11 fixed in C, since the
  // generic SysV call ABI would put the fourth argument in rcx instead.
  if (Opts.TheArch == Arch::X64 && AsmStr == "syscall" &&
      IA->getConstraintString() ==
          "={ax},={r11},0,{di},{si},{dx},{r10},{r8},{r9},~{rcx},~{memory},"
          "~{dirflag},~{fpsr},~{flags}") {
    if (!isLinuxX64SyscallInlineAsm(Call))
      llvm::report_fatal_error("invalid Linux x64 syscall C projection");
    if (Opts.Format != BinaryFormat::Unknown &&
        Opts.Format != BinaryFormat::ELF)
      llvm::report_fatal_error(
          "Linux x64 syscall requires an ELF C projection");
    const std::string Rax = freshVar("syscall_rax");
    const std::string Rdi = freshVar("syscall_rdi");
    const std::string Rsi = freshVar("syscall_rsi");
    const std::string Rdx = freshVar("syscall_rdx");
    const std::string R10 = freshVar("syscall_r10");
    const std::string R8 = freshVar("syscall_r8");
    const std::string R9 = freshVar("syscall_r9");
    const std::string R11 = freshVar("syscall_r11");
    OS << "#if !defined(__linux__) || !defined(__x86_64__)\n"
          "#error \"Linux x64 syscall C projection requires Linux x86-64\"\n"
          "#endif\n";
    emitIndent(Indent);
    OS << "{\n";
    const std::string ScalarNames[] = {Rax, Rdi, Rsi, Rdx};
    for (unsigned Index = 0; Index < 4; ++Index) {
      emitIndent(Indent + 1);
      OS << "uint64_t " << ScalarNames[Index] << " = "
         << valueStr(Call.getArgOperand(Index)) << ";\n";
    }
    const std::pair<llvm::StringRef, std::string> FixedInputs[] = {
        {"r10", R10}, {"r8", R8}, {"r9", R9}};
    for (unsigned Index = 0; Index < 3; ++Index) {
      emitIndent(Indent + 1);
      OS << "register uint64_t " << FixedInputs[Index].second << " __asm__(\""
         << FixedInputs[Index].first
         << "\") = " << valueStr(Call.getArgOperand(Index + 4)) << ";\n";
    }
    emitIndent(Indent + 1);
    OS << "register uint64_t " << R11 << " __asm__(\"r11\");\n";
    emitIndent(Indent + 1);
    OS << "__asm__ volatile(\"syscall\" : \"+a\"(" << Rax << "), \"=r\"(" << R11
       << ") : \"D\"(" << Rdi << "), \"S\"(" << Rsi << "), \"d\"(" << Rdx
       << "), \"r\"(" << R10 << "), \"r\"(" << R8 << "), \"r\"(" << R9
       << ") : \"rcx\", \"memory\", \"cc\");\n";
    if (ResultLive) {
      emitIndent(Indent + 1);
      OS << Name << ".field_0 = " << Rax << ";\n";
      emitIndent(Indent + 1);
      OS << Name << ".field_1 = " << R11 << ";\n";
    }
    emitIndent(Indent);
    OS << "}\n";
    return true;
  }

  // The Windows NT x64 SYSCALL contract takes the service number in RAX and
  // the first four arguments in R10, RDX, R8 and R9, and hands back RAX, R11
  // and the volatile RDX, R8, R9 and R10.
  if (Opts.TheArch == Arch::X64 && AsmStr == "syscall" &&
      IA->getConstraintString() ==
          "={ax},={r11},={dx},={r8},={r9},={r10},0,2,3,4,5,~{rcx},~{memory},"
          "~{dirflag},~{fpsr},~{flags}") {
    if (!isWindowsX64SyscallInlineAsm(Call))
      llvm::report_fatal_error("invalid Windows x64 syscall C projection");
    if (Opts.Format != BinaryFormat::Unknown &&
        Opts.Format != BinaryFormat::COFF)
      llvm::report_fatal_error(
          "Windows x64 syscall requires a COFF C projection");
    const std::string Rax = freshVar("syscall_rax");
    const std::string Rdx = freshVar("syscall_rdx");
    const std::string R8 = freshVar("syscall_r8");
    const std::string R9 = freshVar("syscall_r9");
    const std::string R10 = freshVar("syscall_r10");
    const std::string R11 = freshVar("syscall_r11");
    OS << "#if !defined(_WIN64) || !defined(__x86_64__)\n"
          "#error \"Windows x64 syscall C projection requires Windows "
          "x86-64\"\n"
          "#endif\n";
    emitIndent(Indent);
    OS << "{\n";
    emitIndent(Indent + 1);
    OS << "uint64_t " << Rax << " = " << valueStr(Call.getArgOperand(0))
       << ";\n";
    emitIndent(Indent + 1);
    OS << "uint64_t " << Rdx << " = " << valueStr(Call.getArgOperand(1))
       << ";\n";
    const std::pair<llvm::StringRef, std::string> FixedInputs[] = {
        {"r8", R8}, {"r9", R9}, {"r10", R10}};
    for (unsigned Index = 0; Index < 3; ++Index) {
      emitIndent(Indent + 1);
      OS << "register uint64_t " << FixedInputs[Index].second << " __asm__(\""
         << FixedInputs[Index].first
         << "\") = " << valueStr(Call.getArgOperand(Index + 2)) << ";\n";
    }
    emitIndent(Indent + 1);
    OS << "register uint64_t " << R11 << " __asm__(\"r11\");\n";
    emitIndent(Indent + 1);
    OS << "__asm__ volatile(\"syscall\" : \"+a\"(" << Rax << "), \"=r\"(" << R11
       << "), \"+d\"(" << Rdx << "), \"+r\"(" << R8 << "), \"+r\"(" << R9
       << "), \"+r\"(" << R10 << ") : : \"rcx\", \"memory\", \"cc\");\n";
    if (ResultLive) {
      const std::string *Fields[] = {&Rax, &R11, &Rdx, &R8, &R9, &R10};
      for (unsigned Index = 0; Index < std::size(Fields); ++Index) {
        emitIndent(Indent + 1);
        OS << Name << ".field_" << Index << " = " << *Fields[Index] << ";\n";
      }
    }
    emitIndent(Indent);
    OS << "}\n";
    return true;
  }

  if (Opts.TheArch == Arch::X86 || Opts.TheArch == Arch::X64) {
    const llvm::StringRef Constraints = IA->getConstraintString();
    if (AsmStr == "frndint" && Call.getType()->isX86_FP80Ty() &&
        Call.arg_size() == 1 &&
        Call.getArgOperand(0)->getType()->isX86_FP80Ty() &&
        Constraints == "=&{st},0,~{dirflag},~{fpsr},~{flags}") {
      const std::string Result =
          ResultLive ? Name : freshVar("x87_unused_integer");
      if (!ResultLive) {
        emitIndent(Indent);
        OS << "long double " << Result << ";\n";
      }
      emitIndent(Indent);
      OS << "__asm__ volatile(\"frndint\" : \"=&t\"(" << Result << ") : \"0\"("
         << valueStr(Call.getArgOperand(0)) << ") : \"cc\");\n";
      return true;
    }
    auto X87Operand = [&](unsigned Index) -> std::string {
      const llvm::Value *Arg = Call.getArgOperand(Index);
      if (const auto *Cast = llvm::dyn_cast<llvm::BitCastInst>(Arg);
          Cast && Cast->getSrcTy()->isIntegerTy(80) &&
          Cast->getDestTy()->isX86_FP80Ty()) {
        const std::string Temp = freshVar("x87_operand");
        const std::string Bits = freshVar("x87_source");
        emitIndent(Indent);
        OS << "__uint128_t " << Bits << " = " << valueStr(Cast->getOperand(0))
           << ";\n";
        emitIndent(Indent);
        OS << "long double " << Temp << " = 0;\n";
        emitIndent(Indent);
        OS << "__builtin_memcpy(&" << Temp << ", &" << Bits << ", 10);\n";
        return Temp;
      }
      return valueStr(Arg);
    };
    const auto *PairType = llvm::dyn_cast<llvm::StructType>(Call.getType());
    if ((AsmStr == "fprem\n\tfnstsw $1" || AsmStr == "fprem1\n\tfnstsw $1") &&
        PairType && PairType->getNumElements() == 2 &&
        PairType->getElementType(0)->isX86_FP80Ty() &&
        PairType->getElementType(1)->isIntegerTy(16) && Call.arg_size() == 2 &&
        Call.getArgOperand(0)->getType()->isX86_FP80Ty() &&
        Call.getArgOperand(1)->getType()->isX86_FP80Ty() &&
        Constraints == "=&{st},={ax},0,{st(1)},~{dirflag},~{fpsr},~{flags}") {
      const std::string Dividend = X87Operand(0);
      const std::string Divisor = X87Operand(1);
      const std::string Result =
          ResultLive ? Name : freshVar("x87_unused_pair");
      if (!ResultLive) {
        emitIndent(Indent);
        OS << typeToCLLVM(Call.getType()) << " " << Result << ";\n";
      }
      const llvm::StringRef Mnemonic = llvm::StringRef(AsmStr).take_front(
          llvm::StringRef(AsmStr).find('\n'));
      emitIndent(Indent);
      OS << "__asm__ volatile(\"" << Mnemonic
         << "\\n\\tfnstsw %%ax\" : \"=&t\"(" << Result << ".field_0), \"=a\"("
         << Result << ".field_1) : \"0\"(" << Dividend << "), \"u\"(" << Divisor
         << ") : \"cc\");\n";
      return true;
    }
    if ((AsmStr == "fprem" || AsmStr == "fprem1") &&
        Call.getType()->isX86_FP80Ty() && Call.arg_size() == 2 &&
        Call.getArgOperand(0)->getType()->isX86_FP80Ty() &&
        Call.getArgOperand(1)->getType()->isX86_FP80Ty() &&
        Constraints == "=&{st},0,{st(1)},~{dirflag},~{fpsr},~{flags}") {
      const std::string Dividend = X87Operand(0);
      const std::string Divisor = X87Operand(1);
      const std::string Result =
          ResultLive ? Name : freshVar("x87_unused_remainder");
      if (!ResultLive) {
        emitIndent(Indent);
        OS << "long double " << Result << ";\n";
      }
      const llvm::CallInst *Status = nullptr;
      const llvm::Instruction *Next = Call.getNextNode();
      while (Next && (llvm::isa<llvm::BitCastInst>(Next) ||
                      llvm::isa<llvm::DbgInfoIntrinsic>(Next)))
        Next = Next->getNextNode();
      if (const auto *Candidate =
              llvm::dyn_cast_or_null<llvm::CallInst>(Next)) {
        const auto *StatusAsm =
            llvm::dyn_cast<llvm::InlineAsm>(Candidate->getCalledOperand());
        if (StatusAsm && !Candidate->use_empty() &&
            Candidate->getType()->isIntegerTy(16) &&
            Candidate->arg_size() == 0 &&
            StatusAsm->getAsmString() == "fnstsw $0" &&
            StatusAsm->getConstraintString() ==
                "={ax},~{dirflag},~{fpsr},~{flags}")
          Status = Candidate;
      }
      emitIndent(Indent);
      OS << "__asm__ volatile(\"" << AsmStr;
      if (Status)
        OS << "\\n\\tfnstsw %%ax";
      OS << "\" : \"=&t\"(" << Result << ")";
      if (Status)
        OS << ", \"=a\"(" << getName(Status) << ")";
      OS << " : \"0\"(" << Dividend << "), \"u\"(" << Divisor
         << ") : \"cc\");\n";
      if (Status)
        CapturedX87StatusCalls.insert(Status);
      return true;
    }
    bool KnownBinaryValue = false, PopsSecondInput = false;
#define NEVERD_X87_VALUE_HELPER(ID, CName, Asm, Operands, PopsST1)             \
  if (Operands == 2 && AsmStr == Asm) {                                        \
    KnownBinaryValue = true;                                                   \
    PopsSecondInput = PopsST1;                                                 \
  }
#include "neverd/ir/intrinsics/X87ValueInstructions.def"
    const std::string BinaryConstraints =
        PopsSecondInput
            ? "=&{st},0,{st(1)},~{st(1)},~{dirflag},~{fpsr},~{flags}"
            : "=&{st},0,{st(1)},~{dirflag},~{fpsr},~{flags}";
    if (KnownBinaryValue && Call.getType()->isX86_FP80Ty() &&
        Call.arg_size() == 2 &&
        Call.getArgOperand(0)->getType()->isX86_FP80Ty() &&
        Call.getArgOperand(1)->getType()->isX86_FP80Ty() &&
        Constraints == BinaryConstraints) {
      const std::string Left = X87Operand(0), Right = X87Operand(1);
      const std::string Result =
          ResultLive ? Name : freshVar("x87_unused_value");
      emitIndent(Indent);
      if (!ResultLive)
        OS << "long double " << Result << ";\n";
      OS << "__asm__ volatile(\"" << AsmStr << "\" : \"=&t\"(" << Result
         << ") : \"0\"(" << Left << "), \"u\"(" << Right
         << ") : " << (PopsSecondInput ? "\"st(1)\", " : "") << "\"cc\");\n";
      return true;
    }
    if (AsmStr == "fnstsw $0" && Call.getType()->isIntegerTy(16) &&
        Call.arg_size() == 0 &&
        Constraints == "={ax},~{dirflag},~{fpsr},~{flags}") {
      if (CapturedX87StatusCalls.count(&Call))
        return true;
      emitIndent(Indent);
      if (ResultLive)
        OS << "__asm__ volatile(\"fnstsw %%ax\" : \"=a\"(" << Name
           << ") : : \"cc\");\n";
      else
        OS << "__asm__ volatile(\"fnstsw %%ax\" : : : \"ax\", "
              "\"cc\");\n";
      return true;
    }
    if (Call.getType()->isVoidTy() && Call.arg_size() == 0) {
      // x87 state instructions keep their register-stack clobbers in C, so
      // the C compiler holds no x87 value in a register across them either.
      const bool IsFfree = AsmStr.size() == 12 &&
                           llvm::StringRef(AsmStr).starts_with("ffree %st(") &&
                           AsmStr[10] >= '0' && AsmStr[10] <= '7' &&
                           AsmStr[11] == ')';
      std::optional<X87StateEffect> Effect;
      if (AsmStr == intrinsicAsmMnemonic(Intrinsic::X87Fninit))
        Effect = X87StateEffect::Reset;
      else if (IsFfree || AsmStr == intrinsicAsmMnemonic(Intrinsic::X87Fincstp))
        Effect = X87StateEffect::Stack;
      else if (AsmStr == intrinsicAsmMnemonic(Intrinsic::X87Wait) ||
               AsmStr == intrinsicAsmMnemonic(Intrinsic::X87Fnclex))
        Effect = X87StateEffect::Status;
      if (Effect && x87StateEffectOf(Constraints) == Effect) {
        std::string Template = AsmStr;
        if (IsFfree)
          Template.insert(6, "%");
        emitIndent(Indent);
        OS << "__asm__ volatile(\"" << Template
           << "\" ::: " << x87StateCClobbers(*Effect) << ");\n";
        return true;
      }
    }
  }

  if (auto Stos = classifyX86RepStos(Opts.TheArch, Call)) {
    // Unsigned address arithmetic also models the final (possibly wrapping)
    // register update without creating an out-of-bounds C pointer. Byte stores
    // preserve little-endian values and avoid alignment/aliasing assumptions.
    const std::string Dst = freshVar("stos_dst");
    const std::string Count = freshVar("stos_count");
    const std::string Value = freshVar("stos_value");
    const std::string Backward = freshVar("stos_backward");
    const std::string AddrTy =
        "uint" + std::to_string(Stos->AddressBits) + "_t";
    emitIndent(Indent);
    OS << "{\n";
    emitIndent(Indent + 1);
    OS << AddrTy << " " << Dst << " = (" << AddrTy << ")(uintptr_t)("
       << valueStr(Call.getArgOperand(0)) << ");\n";
    emitIndent(Indent + 1);
    OS << AddrTy << " " << Count << " = " << valueStr(Call.getArgOperand(1))
       << ";\n";
    emitIndent(Indent + 1);
    OS << "uint" << Stos->ElementBytes * 8 << "_t " << Value << " = "
       << valueStr(Call.getArgOperand(2)) << ";\n";
    emitIndent(Indent + 1);
    OS << "int " << Backward << " = "
       << (Stos->Dir == X86RepStos::Dynamic
               ? "(" + valueStr(Call.getArgOperand(3)) + ") != 0"
           : Stos->Dir == X86RepStos::Backward ? "1"
                                               : "0")
       << ";\n";
    emitIndent(Indent + 1);
    OS << "while (" << Count << " != 0) {\n";
    for (unsigned Byte = 0; Byte < Stos->ElementBytes; ++Byte) {
      emitIndent(Indent + 2);
      OS << "((unsigned char*)(uintptr_t)" << Dst << ")[" << Byte
         << "] = (unsigned char)(" << Value << " >> " << Byte * 8 << ");\n";
    }
    emitIndent(Indent + 2);
    OS << Dst << " = " << Backward << " ? " << Dst << " - "
       << Stos->ElementBytes << " : " << Dst << " + " << Stos->ElementBytes
       << ";\n";
    emitIndent(Indent + 2);
    OS << "--" << Count << ";\n";
    emitIndent(Indent + 1);
    OS << "}\n";
    emitIndent(Indent);
    OS << "}\n";
    return true;
  }
  if ((Opts.TheArch == Arch::X86 || Opts.TheArch == Arch::X64) &&
      llvm::StringRef(AsmStr).contains("rep stos"))
    llvm::report_fatal_error("unsupported REP STOS inline assembly contract");

  std::vector<std::string> ArgStrs;
  for (unsigned I = 0; I < Call.arg_size(); ++I)
    ArgStrs.push_back(valueStr(Call.getArgOperand(I)));

  auto Render =
      renderInlineAsm(Opts.TheArch, AsmStr, Call.getType()->isStructTy(), Name,
                      ResultLive, ArgStrs);

  emitIndent(Indent);
  OS << Render.Code;
  if (Render.SetIntrinsics)
    HasCIntrinsics = true;
  return true;
}

void LLVMCWriter::writeCallLike(llvm::CallBase &Call, const std::string &Name,
                                int Indent) {
  if (writeIntrinsicCall(Call, Indent))
    return;
  if (auto *CI = llvm::dyn_cast<llvm::CallInst>(&Call))
    if (writeInlineAsmCall(*CI, Name, Indent))
      return;

  std::string CalleeName;
  if (auto *Callee = Call.getCalledFunction()) {
    const std::string RawCalleeName = Callee->getName().str();
    const char *CN = llvmIntrinsicToCName(RawCalleeName.c_str());
    if (CN) {
      CalleeName = CN;
    } else {
      CalleeName = functionIdentifier(*Callee);
    }
  } else {
    CalleeName = resolveImportCalleeName(Call.getCalledOperand());
    if (CalleeName.empty()) {
      if (std::string Slot = indirectCalleeStr(Call.getCalledOperand());
          !Slot.empty())
        CalleeName = Slot;
      else
        CalleeName = "(" + valueStr(Call.getCalledOperand()) + ")";
    }
  }

  if (isMsvcCxxThrowCallName(CalleeName) ||
      (Call.getCalledFunction() &&
       isMsvcCxxThrowCallName(Call.getCalledFunction()->getName()))) {
    emitIndent(Indent);
    OS << "throw";
    if (Call.arg_size() > 0) {
      const llvm::Value *Object = Call.getArgOperand(0);
      const auto *Integer = llvm::dyn_cast<llvm::ConstantInt>(Object);
      if (!llvm::isa<llvm::ConstantPointerNull>(Object) &&
          !(Integer && Integer->isZero()))
        OS << " " << valueStr(Object);
    }
    OS << ";\n";
    AfterCxxThrow = true;
    return;
  }

  const bool NoReturn = callDoesNotReturn(Call);

  emitIndent(Indent);
  bool ResultLive = !Call.getType()->isVoidTy();
  if (ResultLive && NoReturn)
    ResultLive = false;
  else if (ResultLive && llvm::isa<llvm::InvokeInst>(&Call)) {
    // The invoke and its unwind edge remain observable even when only dead
    // register copies consume its result. Do not invent a C assignment then.
    ResultLive = valueFeedsPrintedUse(&Call);
  } else if (ResultLive && InferredVoid) {
    if (auto *CI = llvm::dyn_cast<llvm::CallInst>(&Call))
      ResultLive = isCallResultLive(Analysis, CI);
  }
  if (ResultLive && !ctorThisAddress(Call).empty())
    ResultLive = false;
  if (ResultLive && unreadMsvcThisReturn(Call))
    ResultLive = false;
  if (ResultLive)
    OS << Name << " = ";
  OS << callExpr(Call) << ";\n";
  AfterCxxThrow = NoReturn;
}

std::string LLVMCWriter::indirectCalleeType(const llvm::CallBase &Call,
                                            bool Strict) {
  const auto *Type = Call.getFunctionType();
  std::string Convention;
  switch (Call.getCallingConv()) {
  case llvm::CallingConv::C:
    break;
  case llvm::CallingConv::Win64:
    Convention = "__attribute__((ms_abi)) ";
    break;
  case llvm::CallingConv::X86_64_SysV:
    Convention = "__attribute__((sysv_abi)) ";
    break;
  case llvm::CallingConv::X86_StdCall:
    Convention = "__attribute__((stdcall)) ";
    break;
  case llvm::CallingConv::X86_FastCall:
    Convention = "__attribute__((fastcall)) ";
    break;
  default:
    if (Strict)
      throw std::runtime_error(
          "unsupported preserved indirect-call convention");
    break;
  }
  std::string Parameters;
  for (auto *Parameter : Type->params()) {
    if (!Parameters.empty())
      Parameters += ", ";
    Parameters += typeToCLLVM(Parameter);
  }
  if (Type->isVarArg()) {
    // C before C23 names no variadic type without a fixed parameter; shown
    // C leaves such a callee unprototyped.
    if (!Parameters.empty())
      Parameters += ", ...";
    else if (Strict)
      throw std::runtime_error(
          "C variadic indirect call lacks a fixed parameter");
  } else if (Parameters.empty()) {
    Parameters = "void";
  }
  return typeToCLLVM(Type->getReturnType()) + " (" + Convention + "*)(" +
         Parameters + ")";
}

std::string
LLVMCWriter::preservedIndirectCalleeStr(const llvm::CallBase &Call) {
  // Opaque LLVM pointers do not carry a C function-pointer type. The call
  // instruction owns the exact result, parameter widths and convention.
  return "((" + indirectCalleeType(Call, /*Strict=*/true) + ")(" +
         valueStr(Call.getCalledOperand()) + "))";
}

std::string LLVMCWriter::callExpr(const llvm::CallBase &Call) {
  for (const llvm::CallBase *Pending : RenderingCalls)
    if (Pending == &Call)
      return getName(&Call);
  RenderingCalls.push_back(&Call);

  if (scalarUnary(Call)) {
    const std::string Expr = functionIdentifier(*Call.getCalledFunction()) +
                             "(" + callArgStr(Call.getArgOperand(0), Call, 0) +
                             ")";
    RenderingCalls.pop_back();
    return Expr;
  }

  if (scalarIntegerMinMax(Call)) {
    // An actual function captures each argument once, including when the
    // intrinsic itself is inlined into another rendered expression.
    const std::string Expr = functionIdentifier(*Call.getCalledFunction()) +
                             "(" + callArgStr(Call.getArgOperand(0), Call, 0) +
                             ", " + callArgStr(Call.getArgOperand(1), Call, 1) +
                             ")";
    RenderingCalls.pop_back();
    return Expr;
  }

  if (const auto *Callee = Call.getCalledFunction()) {
    const auto IID = Callee->getIntrinsicID();
    if (IID == llvm::Intrinsic::assume) {
      if (!Callee->isDeclaration() || !llvm::isa<llvm::CallInst>(Call) ||
          Call.arg_size() != 1 ||
          !Call.getArgOperand(0)->getType()->isIntegerTy(1) ||
          !Call.getType()->isVoidTy() || Call.hasOperandBundles())
        throw std::runtime_error("unsupported LLVM assume contract");
      // The conditional evaluates its predicate once. A compiler builtin
      // assume can discard side effects when the predicate was inlined here.
      const std::string Expr = "(" + valueStr(Call.getArgOperand(0)) +
                               " ? (void)0 : __builtin_unreachable())";
      RenderingCalls.pop_back();
      return Expr;
    }
    if (IID == llvm::Intrinsic::fshl || IID == llvm::Intrinsic::fshr) {
      const auto *Ty = llvm::dyn_cast<llvm::IntegerType>(Call.getType());
      const unsigned Width = Ty ? Ty->getBitWidth() : 0;
      if (Call.arg_size() != 3 || (Width != 8 && Width != 16 && Width != 32 &&
                                   Width != 64 && Width != 128))
        throw std::runtime_error("unsupported LLVM funnel shift width");
      std::string Expr = functionIdentifier(*Callee) + "(";
      for (unsigned I = 0; I < 3; ++I) {
        if (I)
          Expr += ", ";
        Expr += callArgStr(Call.getArgOperand(I), Call, I);
      }
      RenderingCalls.pop_back();
      return Expr + ")";
    }
  }

  std::string CalleeName;
  if (auto *Callee = Call.getCalledFunction()) {
    const std::string RawCalleeName = Callee->getName().str();
    const char *CN = llvmIntrinsicToCName(RawCalleeName.c_str());
    if (CN)
      CalleeName = CN;
    else
      CalleeName = functionIdentifier(*Callee);
  } else if (Opts.PreserveLLVMFunctionTypes) {
    CalleeName = preservedIndirectCalleeStr(Call);
  } else {
    CalleeName = resolveImportCalleeName(Call.getCalledOperand());
    if (CalleeName.empty()) {
      // C calls no data pointer: the call's own type names the function a
      // pointer or an address held as an integer reaches.
      std::string Target = indirectCalleeStr(Call.getCalledOperand());
      if (Target.empty()) {
        const llvm::Value *Callee = Call.getCalledOperand();
        if (const auto *Cast = llvm::dyn_cast<llvm::Operator>(Callee);
            Cast && Cast->getOpcode() == llvm::Instruction::IntToPtr)
          Callee = Cast->getOperand(0);
        Target = "(" + valueStr(Callee) + ")";
      }
      CalleeName = "((" + indirectCalleeType(Call, /*Strict=*/false) + ")" +
                   Target + ")";
    }
  }
  if (Call.getCalledFunction() &&
      isX86FastFailName(Call.getCalledFunction()->getName())) {
    CalleeName = "__fastfail";
    HasCIntrinsics = true;
  }

  std::string Expr = CalleeName + "(";
  const unsigned Limit = printedCallArgLimit(Call, CalleeName);
  for (unsigned ArgIdx = 0; ArgIdx < Limit; ++ArgIdx) {
    if (ArgIdx > 0)
      Expr += ", ";
    std::string Argument = callArgStr(Call.getArgOperand(ArgIdx), Call, ArgIdx);
    const auto *Callee = Call.getCalledFunction();
    if (!Opts.PreserveLLVMFunctionTypes && Callee && Callee->isDeclaration() &&
        libc::headerFor(CalleeName) &&
        libc::isObjectPointerParameter(CalleeName, ArgIdx) &&
        Call.getArgOperand(ArgIdx)->getType()->isIntegerTy())
      Argument = "(void *)(uintptr_t)(" + Argument + ")";
    Expr += Argument;
  }
  Expr += ")";
  RenderingCalls.pop_back();
  return Expr;
}

namespace {
const llvm::Value *peelCastFreeze(const llvm::Value *V) {
  std::set<const llvm::Value *> Seen;
  while (V && Seen.insert(V).second) {
    if (const auto *Cast = llvm::dyn_cast<llvm::CastInst>(V)) {
      V = Cast->getOperand(0);
      continue;
    }
    if (const auto *Fr = llvm::dyn_cast<llvm::FreezeInst>(V)) {
      V = Fr->getOperand(0);
      continue;
    }
    break;
  }
  return V;
}
} // namespace

const llvm::PHINode *
LLVMCWriter::joinPhiForCallArg(const llvm::Value *Arg,
                               const llvm::BasicBlock *BB) {
  if (!Arg || !BB)
    return nullptr;
  Arg = peelCastFreeze(Arg);
  if (!Arg || llvm::isa<llvm::PHINode>(Arg))
    return nullptr;
  const std::string ArgText = valueStr(Arg);
  if (ArgText.find("->") == std::string::npos)
    return nullptr;
  const llvm::PHINode *Best = nullptr;
  for (const llvm::Instruction &Inst : *BB) {
    const auto *Phi = llvm::dyn_cast<llvm::PHINode>(&Inst);
    if (!Phi)
      break;
    if (Analysis.Inlinable.count(Phi) || Phi->getNumIncomingValues() < 2)
      continue;
    bool FieldLikeMatch = false;
    bool DistinctIncoming = false;
    for (const llvm::Value *Inc : Phi->incoming_values()) {
      const llvm::Value *Peeled = peelCastFreeze(Inc);
      const bool Matches =
          Peeled == Arg || Inc == Arg || valueStr(Inc) == ArgText ||
          (Peeled && Peeled != Inc && valueStr(Peeled) == ArgText);
      if (Matches)
        FieldLikeMatch = true;
      else
        DistinctIncoming = true;
    }
    if (FieldLikeMatch && DistinctIncoming)
      Best = Phi;
  }
  return Best;
}

bool LLVMCWriter::mixedPredLastStores(const llvm::AllocaInst *Slot,
                                      const llvm::BasicBlock *BB) const {
  if (!Slot || !BB)
    return false;
  std::optional<std::string> FirstImm;
  const llvm::Value *First = nullptr;
  bool Field = false;
  bool Imm = false;
  bool Distinct = false;
  unsigned Wrote = 0;
  for (const llvm::BasicBlock *Pred : llvm::predecessors(BB)) {
    const llvm::Value *Last = nullptr;
    for (const llvm::Instruction &Inst : *Pred) {
      const auto *SI = llvm::dyn_cast<llvm::StoreInst>(&Inst);
      if (!SI || asAllocaPointer(SI->getPointerOperand()) != Slot)
        continue;
      Last = SI->getValueOperand();
    }
    if (!Last)
      continue;
    ++Wrote;
    if (storedTypedMemberLoad(Last))
      Field = true;
    else if (const auto *CI =
                 llvm::dyn_cast<llvm::ConstantInt>(peelCastFreeze(Last))) {
      Imm = true;
      const std::string Folded = std::to_string(CI->getZExtValue());
      if (!FirstImm)
        FirstImm = Folded;
      else if (*FirstImm != Folded)
        Distinct = true;
    } else {
      Distinct = true;
    }
    if (!First)
      First = Last;
    else if (Last != First)
      Distinct = true;
  }
  return Wrote >= 2 && (Distinct || (Field && Imm));
}

const llvm::AllocaInst *
LLVMCWriter::joinAllocaForCallArg(const llvm::Value *Arg,
                                  const llvm::CallBase &Call) {
  const llvm::BasicBlock *BB = Call.getParent();
  if (!Arg || !BB || llvm::pred_empty(BB))
    return nullptr;
  Arg = peelCastFreeze(Arg);
  if (!Arg || llvm::isa<llvm::PHINode>(Arg) || llvm::isa<llvm::AllocaInst>(Arg))
    return nullptr;
  const llvm::AllocaInst *ArgHome = nullptr;
  if (const auto *LI = llvm::dyn_cast<llvm::LoadInst>(Arg))
    ArgHome = asAllocaPointer(LI->getPointerOperand());
  std::string ArgText;
  bool FieldArg = false;
  if (!ArgHome) {
    ArgText = valueStr(Arg);
    FieldArg = ArgText.find("->") != std::string::npos;
    if (!FieldArg)
      return nullptr;
  }

  if (FieldArg) {
    struct Home {
      bool Match = false;
      bool Distinct = false;
    };
    std::map<const llvm::AllocaInst *, Home> Homes;
    unsigned PredCount = 0;
    for (const llvm::BasicBlock *Pred : llvm::predecessors(BB)) {
      ++PredCount;
      std::map<const llvm::AllocaInst *, const llvm::Value *> Last;
      for (const llvm::Instruction &Inst : *Pred) {
        const auto *SI = llvm::dyn_cast<llvm::StoreInst>(&Inst);
        if (!SI)
          continue;
        const llvm::AllocaInst *Slot = asAllocaPointer(SI->getPointerOperand());
        if (!Slot || allocaAddressTaken(Slot) || isTypedRecordCursorSlot(Slot))
          continue;
        if (llvm::isa<llvm::ArrayType>(Slot->getAllocatedType()))
          continue;
        Last[Slot] = SI->getValueOperand();
      }
      for (const auto &[Slot, Stored] : Last) {
        const llvm::Value *Peeled = peelCastFreeze(Stored);
        const bool Matches =
            Peeled == Arg || Stored == Arg || valueStr(Stored) == ArgText ||
            (Peeled && Peeled != Stored && valueStr(Peeled) == ArgText);
        Home &Info = Homes[Slot];
        if (Matches)
          Info.Match = true;
        else
          Info.Distinct = true;
      }
    }
    if (PredCount < 2)
      return nullptr;
    const llvm::AllocaInst *Best = nullptr;
    for (const auto &[Slot, Info] : Homes) {
      if (Info.Match && Info.Distinct)
        Best = Slot;
    }
    if (Best)
      return Best;
  }
  if (ArgHome && mixedPredLastStores(ArgHome, BB))
    return ArgHome;
  if (ArgHome) {
    const llvm::Value *LastInBB = nullptr;
    for (const llvm::Instruction &Inst : *BB) {
      if (&Inst == &Call)
        break;
      const auto *SI = llvm::dyn_cast<llvm::StoreInst>(&Inst);
      if (!SI || asAllocaPointer(SI->getPointerOperand()) != ArgHome)
        continue;
      LastInBB = SI->getValueOperand();
    }
    const llvm::Value *Peeled = peelCastFreeze(LastInBB);
    if (const auto *LI = llvm::dyn_cast_or_null<llvm::LoadInst>(Peeled)) {
      if (const llvm::AllocaInst *Other =
              asAllocaPointer(LI->getPointerOperand());
          Other && Other != ArgHome && mixedPredLastStores(Other, BB))
        return Other;
    }
  }
  return nullptr;
}

namespace {
TypeRef debugCallArgType(const FunctionSym &FS, size_t Index,
                         const CEmitterOptions &Opts) {
  const bool Indirect =
      isMsvcIndirectReturn(FS.ReturnType, Opts.TheArch, Opts.Format);
  const bool Member =
      Indirect && !FS.Params.empty() && FS.Params[0].first == "this";
  if (Member) {
    if (Index == 0)
      return FS.Params.empty() ? TypeRef{} : FS.Params[0].second;
    if (Index >= 2 && Index - 1 < FS.Params.size())
      return FS.Params[Index - 1].second;
    return {};
  }
  if (Indirect) {
    if (Index >= 1 && Index - 1 < FS.Params.size())
      return FS.Params[Index - 1].second;
    return {};
  }
  if (Index < FS.Params.size())
    return FS.Params[Index].second;
  return {};
}

const llvm::Value *peelIntCast(const llvm::Value *V) {
  std::set<const llvm::Value *> Seen;
  while (V && Seen.insert(V).second) {
    const auto *Cast = llvm::dyn_cast<llvm::CastInst>(V);
    if (!Cast ||
        !llvm::isa<llvm::ZExtInst, llvm::SExtInst, llvm::TruncInst>(Cast) ||
        !Cast->getSrcTy()->isIntegerTy() || !Cast->getDestTy()->isIntegerTy())
      break;
    V = Cast->getOperand(0);
  }
  return V;
}
} // namespace

std::optional<std::string> LLVMCWriter::enumeratorDisplay(const TypeRef &Ty,
                                                          uint64_t Val) const {
  if (!Ty || Ty->Kind != NdTypeKind::Struct || !Ty->IsEnum)
    return std::nullopt;
  if (Dbg)
    Dbg->completeType(Ty);
  auto Name = Ty->displayFieldNameAt(Val);
  if (!Name)
    return std::nullopt;
  if (canonicalizeCProjectionIdentifier(*Name, "") != *Name)
    return std::nullopt;
  return Name;
}

TypeRef LLVMCWriter::enumTypeUsedAsCallArg(const llvm::AllocaInst *Slot) const {
  if (!Slot || !Dbg)
    return {};
  for (const llvm::User *SlotUser : Slot->users()) {
    const auto *LI = llvm::dyn_cast<llvm::LoadInst>(SlotUser);
    if (!LI)
      continue;
    std::vector<const llvm::User *> Work(LI->user_begin(), LI->user_end());
    std::set<const llvm::User *> Seen;
    while (!Work.empty()) {
      const llvm::User *User = Work.back();
      Work.pop_back();
      if (!Seen.insert(User).second)
        continue;
      if (const auto *Cast = llvm::dyn_cast<llvm::CastInst>(User)) {
        if (llvm::isa<llvm::ZExtInst, llvm::SExtInst, llvm::TruncInst>(Cast))
          Work.insert(Work.end(), Cast->user_begin(), Cast->user_end());
        continue;
      }
      const auto *CB = llvm::dyn_cast<llvm::CallBase>(User);
      if (!CB)
        continue;
      const auto FS = debugCallee(*CB);
      if (!FS)
        continue;
      for (unsigned I = 0; I < CB->arg_size(); ++I) {
        if (peelIntCast(CB->getArgOperand(I)) != LI)
          continue;
        TypeRef Ty = debugCallArgType(*FS, I, Opts);
        if (Ty && Ty->Kind == NdTypeKind::Struct && Ty->IsEnum)
          return Ty;
      }
    }
  }
  return {};
}

std::string LLVMCWriter::inplaceIntegerUpdate(const llvm::StoreInst &SI,
                                              llvm::StringRef Dest,
                                              const llvm::Value *Stored) {
  (void)SI;
  const llvm::Value *V = Stored;
  std::set<const llvm::Value *> Seen;
  while (V && Seen.insert(V).second) {
    const auto *Cast = llvm::dyn_cast<llvm::CastInst>(V);
    if (Cast &&
        llvm::isa<llvm::ZExtInst, llvm::SExtInst, llvm::TruncInst>(Cast)) {
      // A compound assignment evaluates in the destination's width. It cannot
      // replace arithmetic that deliberately narrows or extends its result.
      return {};
    }
    if (const auto *LI = llvm::dyn_cast<llvm::LoadInst>(V)) {
      if (const llvm::AllocaInst *Home =
              asAllocaPointer(LI->getPointerOperand())) {
        if (const llvm::Value *Prev = allocaStoredValueBefore(LI);
            Prev && Prev != V) {
          V = Prev;
          continue;
        }
      }
    }
    break;
  }
  const auto *BO = llvm::dyn_cast_or_null<llvm::BinaryOperator>(V);
  if (!BO)
    return {};
  const bool Sub = BO->getOpcode() == llvm::Instruction::Sub;
  if (BO->getOpcode() != llvm::Instruction::Add && !Sub)
    return {};
  const llvm::Value *Base = BO->getOperand(0);
  const auto *CI =
      llvm::dyn_cast<llvm::ConstantInt>(peelIntegerView(BO->getOperand(1)));
  if (!CI && !Sub) {
    CI = llvm::dyn_cast<llvm::ConstantInt>(peelIntegerView(BO->getOperand(0)));
    Base = BO->getOperand(1);
  }
  if (!CI || CI->getBitWidth() > 64 || CI->isZero())
    return {};
  int64_t Delta = CI->getSExtValue();
  if (Sub) {
    if (CI->getValue().isMinSignedValue())
      return {};
    Delta = -Delta;
  }
  if (Delta == 0)
    return {};
  if (BO->getType() != Stored->getType() || valueStr(Base) != Dest)
    return {};
  const uint64_t Abs =
      Delta > 0 ? static_cast<uint64_t>(Delta) : static_cast<uint64_t>(-Delta);
  return Dest.str() + (Delta > 0 ? " += " : " -= ") + std::to_string(Abs);
}

std::string LLVMCWriter::integerCallStoredText(const llvm::Value *Stored) {
  const llvm::Value *V = Stored;
  std::set<const llvm::Value *> Seen;
  while (V && Seen.insert(V).second) {
    if (const auto *Cast = llvm::dyn_cast<llvm::CastInst>(V)) {
      if (llvm::isa<llvm::ZExtInst, llvm::SExtInst, llvm::TruncInst>(Cast)) {
        return valueStr(Stored);
      }
    }
    if (const auto *CB = llvm::dyn_cast<llvm::CallBase>(V)) {
      llvm::Type *Ty = CB->getType();
      if (Ty && Ty->isIntegerTy() && !Ty->isIntegerTy(1))
        return valueStr(CB);
      break;
    }
    if (const auto *LI = llvm::dyn_cast<llvm::LoadInst>(V)) {
      if (const llvm::AllocaInst *Home =
              asAllocaPointer(LI->getPointerOperand())) {
        if (const llvm::Value *Prev = allocaStoredValueBefore(LI);
            Prev && Prev != V) {
          V = Prev;
          continue;
        }
      }
    }
    break;
  }
  return valueStr(Stored);
}

std::string LLVMCWriter::enumStoredText(const llvm::AllocaInst *Slot,
                                        const llvm::Value *Stored) {
  const std::string RHS = valueStr(Stored);
  if (!Slot)
    return RHS;
  if (SyntheticFrame &&
      (!Analysis.RawFrameLocations.empty() ||
       Analysis.RawFrameAllocas.count(SyntheticFrame)) &&
      Slot->getAllocatedType()->isIntegerTy())
    if (auto Frame = peelPointerOffset(Stored);
        Frame && Frame->first == SyntheticFrame)
      return "(uintptr_t)" + RHS;
  const auto *CI =
      llvm::dyn_cast_or_null<llvm::ConstantInt>(peelIntegerView(Stored));
  if (!CI)
    return RHS;
  if (TypeRef Ty = enumTypeUsedAsCallArg(Slot))
    if (auto Name = enumeratorDisplay(Ty, CI->getZExtValue()))
      return *Name;
  return RHS;
}

std::string LLVMCWriter::comparedOperandText(const llvm::Value *V,
                                             const llvm::Value *Other) {
  const std::string Text = valueStr(peelIntegerView(V));
  const auto *CI = llvm::dyn_cast_or_null<llvm::ConstantInt>(
      peelIntCast(peelIntegerView(V)));
  if (!CI || !Other)
    return Text;
  const llvm::Value *Typed = peelIntCast(peelIntegerView(Other));
  const auto *LI = llvm::dyn_cast_or_null<llvm::LoadInst>(Typed);
  if (!LI)
    return Text;
  auto Acc =
      typedRecordAccess(LI->getPointerOperand(), llvmAccessSize(LI->getType()));
  if (!Acc)
    return Text;
  if (auto Name = enumeratorDisplay(Acc->Type, CI->getZExtValue()))
    return *Name;
  return Text;
}

std::string LLVMCWriter::callArgStr(const llvm::Value *Arg,
                                    const llvm::CallBase &Call,
                                    unsigned ArgIdx) {
  const llvm::BasicBlock *BB = Call.getParent();
  if (const llvm::PHINode *Phi = joinPhiForCallArg(Arg, BB))
    return getName(Phi);
  if (const llvm::LoadInst *ArgLoad =
          llvm::dyn_cast<llvm::LoadInst>(peelCastFreeze(Arg))) {
    if (auto It = SelectUseText.find(ArgLoad); It != SelectUseText.end())
      return It->second;
  }
  if (const llvm::AllocaInst *Slot = joinAllocaForCallArg(Arg, Call)) {
    if (auto Fwd = ForwardedFieldArgs.find(Slot);
        Fwd != ForwardedFieldArgs.end())
      return Fwd->second;
    if (Slot != PhiTailSlot)
      return getName(Slot);
  }
  if (const MsvcCallee *Msvc =
          msvcCallee(printedCalleeName(Call), Opts.Format)) {
    const TypeRef Expected = msvcExpectedCallArgType(*Msvc, ArgIdx);
    if (Expected && Expected->Kind == NdTypeKind::Int) {
      const llvm::Value *Inner = peelIntegerView(Arg);
      if (Inner && Inner != Arg)
        return valueStr(Inner);
    }
  }
  if (const auto *CI =
          llvm::dyn_cast<llvm::ConstantInt>(peelIntegerView(Arg))) {
    if (const auto FS = debugCallee(Call))
      if (auto Name = enumeratorDisplay(debugCallArgType(*FS, ArgIdx, Opts),
                                        CI->getZExtValue()))
        return *Name;
  }
  if (auto Imm = foldImmediate(Arg)) {
    uint64_t Val = 0;
    if (!llvm::StringRef(*Imm).getAsInteger(0, Val))
      if (const auto FS = debugCallee(Call))
        if (auto Name =
                enumeratorDisplay(debugCallArgType(*FS, ArgIdx, Opts), Val))
          return *Name;
  }
  return valueStr(Arg);
}

bool LLVMCWriter::writeExclusiveSkip(
    llvm::ArrayRef<const llvm::BasicBlock *> Headers,
    llvm::ArrayRef<const llvm::BasicBlock *> Arms,
    llvm::ArrayRef<char> ArmOnTrue, const llvm::BasicBlock *Tail,
    llvm::ArrayRef<const llvm::BasicBlock *> Owned,
    llvm::ArrayRef<const llvm::BasicBlock *> FlatConds,
    llvm::ArrayRef<char> FlatTaken, llvm::ArrayRef<unsigned> FlatCounts,
    llvm::ArrayRef<const llvm::BasicBlock *> Elided, int Indent) {
  if (Headers.size() < 2 || !Tail || Headers.size() != Arms.size() ||
      Headers.size() != ArmOnTrue.size() || FlatCounts.size() != Headers.size())
    return false;
  unsigned CondTotal = 0;
  for (unsigned Count : FlatCounts)
    CondTotal += Count;
  if (CondTotal != FlatConds.size() || CondTotal != FlatTaken.size())
    return false;
  (void)ArmOnTrue;
  auto Invert = [&](const llvm::Value *V, std::string Cond) {
    if (std::optional<std::string> Swapped = invertedRelationalText(V))
      return std::move(*Swapped);
    if (std::optional<std::string> Peeled = peelWrappedNot(Cond))
      return std::move(*Peeled);
    return "!(" + Cond + ")";
  };
  auto CondText = [&](const llvm::BasicBlock *Header, bool Live) {
    const auto *Br = llvm::cast<llvm::CondBrInst>(Header->getTerminator());
    if (Live)
      return condStr(Br->getCondition());
    const auto SavedLast = AllocaLastValues;
    const auto SavedImm = AllocaImmediates;
    AllocaLastValues.clear();
    AllocaImmediates.clear();
    for (const llvm::Instruction &Local : *Header) {
      const auto *SI = llvm::dyn_cast<llvm::StoreInst>(&Local);
      if (!SI)
        continue;
      if (const llvm::AllocaInst *Slot =
              asAllocaPointer(SI->getPointerOperand()))
        AllocaLastValues[Slot] = SI->getValueOperand();
    }
    llvm::SmallVector<const llvm::CallInst *, 4> Added;
    if (!Live) {
      for (const llvm::Instruction &Local : *Header) {
        const auto *Call = llvm::dyn_cast<llvm::CallInst>(&Local);
        if (!Call || Call->getType()->isVoidTy() ||
            Analysis.Inlinable.count(Call))
          continue;
        Analysis.Inlinable.insert(Call);
        Added.push_back(Call);
      }
    }
    std::string Text = condStr(Br->getCondition());
    for (const llvm::CallInst *Call : Added) {
      Analysis.Inlinable.erase(Call);
      InlineCache.erase(Call);
    }
    AllocaLastValues = SavedLast;
    AllocaImmediates = SavedImm;
    return Text;
  };
  auto Group = [](std::string Text) {
    if (Text.find("&&") != std::string::npos ||
        Text.find("||") != std::string::npos)
      return "(" + Text + ")";
    return Text;
  };
  llvm::SmallVector<std::string, 4> Conds;
  size_t Base = 0;
  for (size_t I = 0; I < Headers.size(); ++I) {
    std::string Cond;
    for (unsigned J = 0; J < FlatCounts[I]; ++J) {
      const llvm::BasicBlock *Header = FlatConds[Base + J];
      std::string Part = CondText(Header, I == 0 && J == 0);
      if (!FlatTaken[Base + J])
        Part = Invert(llvm::cast<llvm::CondBrInst>(Header->getTerminator())
                          ->getCondition(),
                      std::move(Part));
      if (Part.empty())
        return false;
      if (!Cond.empty())
        Cond += " && ";
      Cond += Group(std::move(Part));
    }
    Base += FlatCounts[I];
    Conds.push_back(std::move(Cond));
  }
  llvm::SmallPtrSet<const llvm::BasicBlock *, 32> OwnedSet(Owned.begin(),
                                                           Owned.end());
  for (const llvm::BasicBlock *BB : Owned) {
    InlinedFallthroughBlocks.insert(BB);
    ReferencedBlocks.erase(BB);
  }
  for (const llvm::BasicBlock *BB : FlatConds) {
    if (BB == Headers.front())
      continue;
    InlinedFallthroughBlocks.insert(BB);
    ReferencedBlocks.erase(BB);
  }
  for (const llvm::BasicBlock *BB : Arms) {
    InlinedFallthroughBlocks.insert(BB);
    ReferencedBlocks.erase(BB);
  }
  for (const llvm::BasicBlock *BB : Elided) {
    InlinedFallthroughBlocks.insert(BB);
    ReferencedBlocks.erase(BB);
  }
  ReferencedBlocks.erase(Tail);
  llvm::SmallPtrSet<const llvm::BasicBlock *, 32> Seen;
  auto HeaderPrints = [&](const llvm::BasicBlock *Header) {
    for (const llvm::Instruction &Inst : *Header) {
      if (Inst.isTerminator() || llvm::isa<llvm::AllocaInst>(&Inst))
        continue;
      if (instructionIsPrinted(Inst))
        return true;
    }
    return false;
  };
  std::function<void(size_t, int, bool)> WriteChain;
  WriteChain = [&](size_t Index, int Depth, bool SameLine) {
    if (!SameLine)
      emitIndent(Depth);
    OS << "if (" << Conds[Index] << ") {\n";
    writeExclusiveArm(Arms[Index], Tail, OwnedSet, nullptr, Seen, Depth + 1);
    if (Index + 1 == Headers.size()) {
      emitIndent(Depth);
      OS << "}\n";
      return;
    }
    if (!HeaderPrints(Headers[Index + 1])) {
      emitIndent(Depth);
      OS << "} else ";
      WriteChain(Index + 1, Depth, true);
      return;
    }
    emitIndent(Depth);
    OS << "} else {\n";
    {
      decltype(KnownImmediates) Keep;
      KnownImmediates.swap(Keep);
      AllocaImmediates.clear();
      AllocaLastValues.clear();
    }
    for (llvm::Instruction &Inst :
         *const_cast<llvm::BasicBlock *>(Headers[Index + 1])) {
      if (Inst.isTerminator() || llvm::isa<llvm::AllocaInst>(&Inst))
        continue;
      writeInstruction(Inst, Depth + 1);
    }
    WriteChain(Index + 1, Depth + 1, false);
    emitIndent(Depth);
    OS << "}\n";
  };
  WriteChain(0, Indent, false);
  ReferencedBlocks.erase(Tail);
  return true;
}

void LLVMCWriter::writeExclusiveArm(
    const llvm::BasicBlock *BB, const llvm::BasicBlock *Tail,
    const llvm::SmallPtrSetImpl<const llvm::BasicBlock *> &Owned,
    const llvm::BasicBlock *Stop,
    llvm::SmallPtrSetImpl<const llvm::BasicBlock *> &Seen, int Indent) {
  if (!BB || BB == Tail || BB == Stop || !Owned.count(BB))
    return;
  if (!Seen.insert(BB).second)
    return;
  AfterCxxThrow = false;
  {
    decltype(KnownImmediates) Keep;
    for (const llvm::Instruction &Inst : *BB) {
      const auto *Phi = llvm::dyn_cast<llvm::PHINode>(&Inst);
      if (!Phi)
        break;
      if (auto It = KnownImmediates.find(Phi); It != KnownImmediates.end())
        Keep[Phi] = It->second;
    }
    KnownImmediates.swap(Keep);
    AllocaImmediates.clear();
    AllocaLastValues.clear();
  }
  auto RawRegisterDtor = [&](llvm::Instruction &Inst) {
    const auto *Call = llvm::dyn_cast<llvm::CallBase>(&Inst);
    if (!Call || Call->arg_empty())
      return false;
    const std::string Callee = printedCalleeName(*Call);
    if (Callee.size() < 5 || Callee.compare(Callee.size() - 5, 5, "_dtor") != 0)
      return false;
    const std::string Arg = callArgStr(Call->getArgOperand(0), *Call, 0);
    const auto Under = Arg.rfind('_');
    if (Arg.empty() || Under == std::string::npos || Under == 0 ||
        Under + 1 >= Arg.size() ||
        !std::isupper(static_cast<unsigned char>(Arg[0])))
      return false;
    for (size_t C = 0; C < Arg.size(); ++C) {
      const unsigned char Ch = static_cast<unsigned char>(Arg[C]);
      if (C < Under) {
        if (!std::isupper(Ch) && !std::isdigit(Ch))
          return false;
      } else if (C > Under && !std::isdigit(Ch))
        return false;
    }
    return true;
  };
  for (llvm::Instruction &Inst : *const_cast<llvm::BasicBlock *>(BB)) {
    if (Inst.isTerminator() || llvm::isa<llvm::AllocaInst>(&Inst))
      continue;
    if (RawRegisterDtor(Inst))
      continue;
    writeInstruction(Inst, Indent);
  }
  const llvm::Instruction *Term = BB->getTerminator();
  if (!Term)
    return;
  if (llvm::isa<llvm::ReturnInst>(Term) ||
      llvm::isa<llvm::UnreachableInst>(Term)) {
    writeInstruction(*const_cast<llvm::Instruction *>(Term), Indent);
    return;
  }
  auto Resolve = [&](const llvm::BasicBlock *Raw) -> const llvm::BasicBlock * {
    const llvm::BasicBlock *Pred = BB;
    const llvm::BasicBlock *Cur = Raw;
    llvm::SmallPtrSet<const llvm::BasicBlock *, 8> Hop;
    while (Cur && isPrintPassthrough(Cur) &&
           Cur->getSinglePredecessor() == Pred && Hop.insert(Cur).second) {
      Pred = Cur;
      const auto *Br = llvm::dyn_cast<llvm::UncondBrInst>(Cur->getTerminator());
      if (!Br)
        break;
      Cur = Br->getSuccessor(0);
    }
    return Cur;
  };
  auto Common = [&](const llvm::BasicBlock *Left,
                    const llvm::BasicBlock *Right) -> const llvm::BasicBlock * {
    if (!Left || !Right || !Owned.count(Left) || !Owned.count(Right) ||
        Left == Tail || Right == Tail || Left == Stop || Right == Stop)
      return nullptr;
    llvm::SmallPtrSet<const llvm::BasicBlock *, 32> FromLeft;
    llvm::SmallVector<const llvm::BasicBlock *, 16> Work{Left};
    while (!Work.empty()) {
      const llvm::BasicBlock *Cur = Work.pop_back_val();
      if (!Cur || Cur == Tail || Cur == Stop || !Owned.count(Cur) ||
          !FromLeft.insert(Cur).second)
        continue;
      if (FromLeft.size() > 64)
        break;
      for (const llvm::BasicBlock *Succ : llvm::successors(Cur))
        if (Owned.count(Succ))
          Work.push_back(Succ);
    }
    llvm::SmallVector<const llvm::BasicBlock *, 16> Queue{Right};
    llvm::SmallPtrSet<const llvm::BasicBlock *, 32> SeenRight;
    for (size_t Q = 0; Q < Queue.size(); ++Q) {
      const llvm::BasicBlock *Cur = Queue[Q];
      if (!Cur || Cur == Tail || Cur == Stop || !Owned.count(Cur) ||
          !SeenRight.insert(Cur).second)
        continue;
      if (FromLeft.count(Cur))
        return Cur;
      for (const llvm::BasicBlock *Succ : llvm::successors(Cur))
        if (Owned.count(Succ))
          Queue.push_back(Succ);
    }
    return nullptr;
  };
  auto PrintReturnChain = [&](const llvm::BasicBlock *Origin,
                              const llvm::BasicBlock *Start, int Depth) {
    llvm::SmallVector<const llvm::BasicBlock *, 4> Chain;
    const llvm::BasicBlock *Cur = Start;
    llvm::SmallPtrSet<const llvm::BasicBlock *, 8> Local;
    while (Cur && Chain.size() < 4 && Local.insert(Cur).second) {
      if (Cur == Tail || Cur == Stop || Owned.count(Cur))
        return false;
      Chain.push_back(Cur);
      const llvm::Instruction *ChainTerm = Cur->getTerminator();
      if (!ChainTerm)
        return false;
      if (llvm::isa<llvm::ReturnInst>(ChainTerm) ||
          llvm::isa<llvm::UnreachableInst>(ChainTerm)) {
        // Copy stores along this edge name a constructor's returned
        // address. Print every block; the last one is the epilogue return.
        const auto SavedLast = AllocaLastValues;
        auto NoteStores = [&](const llvm::BasicBlock *Block) {
          if (!Block)
            return;
          for (const llvm::Instruction &Inst : *Block) {
            const auto *SI = llvm::dyn_cast<llvm::StoreInst>(&Inst);
            if (!SI)
              continue;
            if (const llvm::AllocaInst *Slot =
                    asAllocaPointer(SI->getPointerOperand()))
              AllocaLastValues[Slot] = SI->getValueOperand();
          }
        };
        llvm::SmallVector<const llvm::BasicBlock *, 8> Back;
        llvm::SmallPtrSet<const llvm::BasicBlock *, 8> BackSeen;
        const llvm::BasicBlock *Hop = Origin;
        while (Hop && Back.size() < 8 && BackSeen.insert(Hop).second) {
          Back.push_back(Hop);
          Hop = Hop->getSinglePredecessor();
        }
        for (auto It = Back.rbegin(); It != Back.rend(); ++It)
          NoteStores(*It);
        for (size_t I = 0; I < Chain.size(); ++I) {
          NoteStores(Chain[I]);
          const llvm::Instruction *BlockTerm = Chain[I]->getTerminator();
          for (llvm::Instruction &Inst :
               *const_cast<llvm::BasicBlock *>(Chain[I])) {
            if (&Inst == BlockTerm || llvm::isa<llvm::AllocaInst>(&Inst))
              continue;
            if (RawRegisterDtor(Inst))
              continue;
            writeInstruction(Inst, Depth);
          }
          if (I + 1 == Chain.size())
            writeInstruction(*const_cast<llvm::Instruction *>(BlockTerm),
                             Depth);
        }
        AllocaLastValues = SavedLast;
        return true;
      }
      const auto *Go = llvm::dyn_cast<llvm::UncondBrInst>(ChainTerm);
      if (!Go || !Go->getSuccessor(0) || Go->getSuccessor(0) == Cur)
        return false;
      Cur = Go->getSuccessor(0);
    }
    return false;
  };
  if (const auto *Br = llvm::dyn_cast<llvm::UncondBrInst>(Term)) {
    const llvm::BasicBlock *Raw = Br->getSuccessor(0);
    const llvm::BasicBlock *Dest = Resolve(Raw);
    if (!Dest || Dest == Tail || Dest == Stop)
      return;
    if (!Owned.count(Dest)) {
      if (!PrintReturnChain(BB, Dest, Indent))
        writeGoto(BB, Raw, Indent);
      return;
    }
    writeExclusiveArm(Dest, Tail, Owned, Stop, Seen, Indent);
    return;
  }
  const auto *Br = llvm::dyn_cast<llvm::CondBrInst>(Term);
  if (!Br)
    return;
  const llvm::BasicBlock *RawT = Br->getSuccessor(0);
  const llvm::BasicBlock *RawF = Br->getSuccessor(1);
  const llvm::BasicBlock *TrueT = Resolve(RawT);
  const llvm::BasicBlock *FalseT = Resolve(RawF);
  std::string Cond = condStr(Br->getCondition());
  auto Inside = [&](const llvm::BasicBlock *Dest) {
    return Dest && Dest != Tail && Dest != Stop && Owned.count(Dest);
  };
  const llvm::BasicBlock *Merge =
      Inside(TrueT) && Inside(FalseT) ? Common(TrueT, FalseT) : nullptr;
  if (Merge == BB)
    Merge = nullptr;
  if (Merge && TrueT == Merge && FalseT == Merge) {
    writeExclusiveArm(Merge, Tail, Owned, Stop, Seen, Indent);
    return;
  }
  auto Leave = [&](const llvm::BasicBlock *Dest, const llvm::BasicBlock *Raw,
                   int Depth) {
    if (!Dest || Dest == Tail || Dest == Stop || Dest == Merge)
      return;
    if (!Owned.count(Dest)) {
      if (!PrintReturnChain(BB, Dest, Depth))
        writeGoto(BB, Raw, Depth);
      return;
    }
    writeExclusiveArm(Dest, Tail, Owned, Merge ? Merge : Stop, Seen, Depth);
  };
  auto Inverted = [&](std::string Text) {
    if (std::optional<std::string> Swapped =
            invertedRelationalText(Br->getCondition()))
      return std::move(*Swapped);
    if (std::optional<std::string> Peeled = peelWrappedNot(Text))
      return std::move(*Peeled);
    return "!(" + Text + ")";
  };
  if (const llvm::BasicBlock *Taken = impliedPointerSuccessor(Br)) {
    const llvm::BasicBlock *Other = Taken == Br->getSuccessor(0)
                                        ? Br->getSuccessor(1)
                                        : Br->getSuccessor(0);
    auto Silent = [&](const llvm::BasicBlock *Edge) {
      const llvm::BasicBlock *Cur = Edge;
      llvm::SmallPtrSet<const llvm::BasicBlock *, 8> SeenEdge;
      while (Cur && SeenEdge.insert(Cur).second) {
        for (const llvm::Instruction &Inst : *Cur) {
          if (Inst.isTerminator() || llvm::isa<llvm::AllocaInst>(&Inst))
            continue;
          if (instructionIsPrinted(Inst))
            return false;
        }
        const auto *Go =
            llvm::dyn_cast<llvm::UncondBrInst>(Cur->getTerminator());
        if (!Go)
          return true;
        const llvm::BasicBlock *Next = Go->getSuccessor(0);
        if (!Next || Next->getSinglePredecessor() != Cur)
          return true;
        Cur = Next;
      }
      return true;
    };
    llvm::SmallVector<const llvm::BasicBlock *, 4> DeadNull;
    if (Silent(Other) || deadNullAssignBlocks(Br, Taken, DeadNull)) {
      for (const llvm::BasicBlock *BB : DeadNull) {
        Seen.insert(BB);
        InlinedFallthroughBlocks.insert(BB);
      }
      const llvm::BasicBlock *TakenR = Resolve(Taken);
      const llvm::BasicBlock *OtherR = Resolve(Other);
      const llvm::BasicBlock *Meet = Common(TakenR, OtherR);
      if (TakenR && Owned.count(TakenR) && TakenR != Tail && TakenR != Stop)
        writeExclusiveArm(TakenR, Tail, Owned, Meet ? Meet : Stop, Seen,
                          Indent);
      else
        Leave(TakenR, Taken, Indent);
      if (Meet && Meet != TakenR)
        writeExclusiveArm(Meet, Tail, Owned, Stop, Seen, Indent);
      return;
    }
  }
  if (Cond.empty()) {
    Leave(TrueT, RawT, Indent);
    Leave(FalseT, RawF, Indent);
    if (Merge)
      writeExclusiveArm(Merge, Tail, Owned, Stop, Seen, Indent);
    return;
  }
  const bool TrueIn = Inside(TrueT) && TrueT != Merge;
  const bool FalseIn = Inside(FalseT) && FalseT != Merge;
  if (Merge && !TrueIn && FalseIn) {
    emitIndent(Indent);
    OS << "if (" << Inverted(Cond) << ") {\n";
    Leave(FalseT, RawF, Indent + 1);
    emitIndent(Indent);
    OS << "}\n";
    writeExclusiveArm(Merge, Tail, Owned, Stop, Seen, Indent);
    return;
  }
  if (Merge && TrueIn && !FalseIn) {
    emitIndent(Indent);
    OS << "if (" << Cond << ") {\n";
    Leave(TrueT, RawT, Indent + 1);
    emitIndent(Indent);
    OS << "}\n";
    writeExclusiveArm(Merge, Tail, Owned, Stop, Seen, Indent);
    return;
  }
  if (TrueIn && FalseIn) {
    emitIndent(Indent);
    OS << "if (" << Cond << ") {\n";
    Leave(TrueT, RawT, Indent + 1);
    emitIndent(Indent);
    OS << "} else {\n";
    Leave(FalseT, RawF, Indent + 1);
    emitIndent(Indent);
    OS << "}\n";
    if (Merge)
      writeExclusiveArm(Merge, Tail, Owned, Stop, Seen, Indent);
    return;
  }
  if (TrueIn && !FalseIn) {
    emitIndent(Indent);
    OS << "if (" << Cond << ") {\n";
    Leave(TrueT, RawT, Indent + 1);
    if (FalseT && FalseT != Tail && FalseT != Stop && FalseT != Merge) {
      emitIndent(Indent);
      OS << "} else {\n";
      Leave(FalseT, RawF, Indent + 1);
    }
    emitIndent(Indent);
    OS << "}\n";
    return;
  }
  if (FalseIn && !TrueIn) {
    emitIndent(Indent);
    OS << "if (" << Inverted(Cond) << ") {\n";
    Leave(FalseT, RawF, Indent + 1);
    if (TrueT && TrueT != Tail && TrueT != Stop && TrueT != Merge) {
      emitIndent(Indent);
      OS << "} else {\n";
      Leave(TrueT, RawT, Indent + 1);
    }
    emitIndent(Indent);
    OS << "}\n";
    return;
  }
  if ((!TrueT || TrueT == Tail || TrueT == Stop) &&
      (!FalseT || FalseT == Tail || FalseT == Stop))
    return;
  if (!TrueT || TrueT == Tail || TrueT == Stop) {
    emitIndent(Indent);
    OS << "if (" << Inverted(Cond) << ") {\n";
    Leave(FalseT, RawF, Indent + 1);
    emitIndent(Indent);
    OS << "}\n";
    return;
  }
  if (!FalseT || FalseT == Tail || FalseT == Stop) {
    emitIndent(Indent);
    OS << "if (" << Cond << ") {\n";
    Leave(TrueT, RawT, Indent + 1);
    emitIndent(Indent);
    OS << "}\n";
    return;
  }
  emitIndent(Indent);
  OS << "if (" << Cond << ") {\n";
  Leave(TrueT, RawT, Indent + 1);
  emitIndent(Indent);
  OS << "} else {\n";
  Leave(FalseT, RawF, Indent + 1);
  emitIndent(Indent);
  OS << "}\n";
}

void LLVMCWriter::writeInvertedFalseSkip(const llvm::BasicBlock *From,
                                         const llvm::BasicBlock *Else,
                                         int Indent) {
  const llvm::BasicBlock *Body = straightLineFalseSkip(From, Else);
  if (!Body)
    return;
  const auto *FromBr = llvm::cast<llvm::CondBrInst>(From->getTerminator());
  const llvm::BasicBlock *Then = FromBr->getSuccessor(0);
  auto Inverted = [](std::string Cond) {
    auto Wrapped = [](const std::string &S) {
      if (S.size() < 2 || S.front() != '(' || S.back() != ')')
        return false;
      int Depth = 0;
      for (size_t I = 0; I < S.size(); ++I) {
        if (S[I] == '(')
          ++Depth;
        else if (S[I] == ')') {
          --Depth;
          if (Depth == 0 && I + 1 != S.size())
            return false;
        }
      }
      return Depth == 0;
    };
    if (Cond.size() >= 2 && Cond[0] == '!' && Wrapped(Cond.substr(1)))
      return Cond.substr(2, Cond.size() - 3);
    return "!(" + Cond + ")";
  };
  std::string Shown = condStr(FromBr->getCondition());
  if (std::optional<std::string> Swapped =
          invertedRelationalText(FromBr->getCondition()))
    Shown = *Swapped;
  else
    Shown = Inverted(std::move(Shown));
  llvm::SmallVector<const llvm::BasicBlock *, 4> SelConds;
  llvm::SmallVector<const llvm::BasicBlock *, 4> SelArms;
  const llvm::BasicBlock *SelDef = nullptr;
  const llvm::BasicBlock *SelJoin = nullptr;
  if (assignSelectElseIf(From, Body, SelConds, SelArms, SelDef, SelJoin)) {
    auto WriteArm = [&](const llvm::BasicBlock *Arm, int ArmIndent) {
      const auto SavedLast = AllocaLastValues;
      const auto SavedImm = AllocaImmediates;
      AllocaLastValues.clear();
      AllocaImmediates.clear();
      for (llvm::Instruction &ArmInst : *const_cast<llvm::BasicBlock *>(Arm)) {
        if (ArmInst.isTerminator() || llvm::isa<llvm::AllocaInst>(&ArmInst))
          continue;
        writeInstruction(ArmInst, ArmIndent);
      }
      AllocaLastValues = SavedLast;
      AllocaImmediates = SavedImm;
    };
    auto CondText = [&](const llvm::BasicBlock *Cond) {
      const auto *Br = llvm::cast<llvm::CondBrInst>(Cond->getTerminator());
      const auto SavedLast = AllocaLastValues;
      const auto SavedImm = AllocaImmediates;
      AllocaLastValues.clear();
      AllocaImmediates.clear();
      for (const llvm::Instruction &Local : *Cond) {
        const auto *SI = llvm::dyn_cast<llvm::StoreInst>(&Local);
        if (!SI)
          continue;
        if (const llvm::AllocaInst *Slot =
                asAllocaPointer(SI->getPointerOperand()))
          AllocaLastValues[Slot] = SI->getValueOperand();
      }
      llvm::SmallVector<const llvm::CallInst *, 4> Added;
      for (const llvm::Instruction &Local : *Cond) {
        const auto *Call = llvm::dyn_cast<llvm::CallInst>(&Local);
        if (!Call || Call->getType()->isVoidTy() ||
            Analysis.Inlinable.count(Call))
          continue;
        Analysis.Inlinable.insert(Call);
        Added.push_back(Call);
      }
      std::string Text = condStr(Br->getCondition());
      for (const llvm::CallInst *Call : Added) {
        Analysis.Inlinable.erase(Call);
        InlineCache.erase(Call);
      }
      AllocaLastValues = SavedLast;
      AllocaImmediates = SavedImm;
      return Text;
    };
    for (size_t I = 1; I < SelConds.size(); ++I)
      InlinedFallthroughBlocks.insert(SelConds[I]);
    for (const llvm::BasicBlock *Arm : SelArms)
      InlinedFallthroughBlocks.insert(Arm);
    InlinedFallthroughBlocks.insert(SelDef);
    emitIndent(Indent);
    OS << "if (" << Shown << ") {\n";
    WriteArm(SelArms[0], Indent + 1);
    for (size_t I = 1; I < SelConds.size(); ++I) {
      emitIndent(Indent);
      OS << "} else if (" << CondText(SelConds[I]) << ") {\n";
      WriteArm(SelArms[I], Indent + 1);
    }
    emitIndent(Indent);
    OS << "} else {\n";
    if (ReferencedBlocks.count(SelDef))
      OS << blockLabel(SelDef) << ":\n";
    WriteArm(SelDef, Indent + 1);
    emitIndent(Indent);
    OS << "}\n";
    if (joinPrintsNext(From, SelJoin))
      ReferencedBlocks.erase(SelJoin);
    else
      writeGoto(From, SelJoin, Indent);
    return;
  }
  emitIndent(Indent);
  OS << "if (" << Shown << ") {\n";
  for (llvm::Instruction &Inst : *const_cast<llvm::BasicBlock *>(Body)) {
    if (Inst.isTerminator() || llvm::isa<llvm::AllocaInst>(&Inst))
      continue;
    writeInstruction(Inst, Indent + 1);
  }
  if (const auto *Br =
          llvm::dyn_cast<llvm::UncondBrInst>(Body->getTerminator())) {
    const llvm::BasicBlock *Check = Br->getSuccessor(0);
    const auto *CheckBr =
        Check ? llvm::dyn_cast<llvm::CondBrInst>(Check->getTerminator())
              : nullptr;
    if (CheckBr && Check->getSinglePredecessor() == Body &&
        straightLineFalseSkip(Check, CheckBr->getSuccessor(1)) &&
        printBranchTarget(CheckBr->getSuccessor(0)) ==
            printBranchTarget(Then)) {
      writeInstruction(*const_cast<llvm::CondBrInst *>(CheckBr), Indent + 1);
      InlinedFallthroughBlocks.insert(Check);
    } else {
      const bool FallsThrough =
          printBranchTarget(Br->getSuccessor(0)) == printBranchTarget(Then);
      writeGoto(Body, Br->getSuccessor(0), Indent + 1, !FallsThrough);
    }
  } else if (auto *Ret =
                 llvm::dyn_cast<llvm::ReturnInst>(Body->getTerminator())) {
    writeInstruction(*const_cast<llvm::ReturnInst *>(Ret), Indent + 1);
  } else if (auto *Inner =
                 llvm::dyn_cast<llvm::CondBrInst>(Body->getTerminator())) {
    const llvm::BasicBlock *Alt = nullptr;
    const llvm::BasicBlock *Def = nullptr;
    const llvm::BasicBlock *Join = nullptr;
    std::string InnerShown = condStr(Inner->getCondition());
    if (std::optional<std::string> Swapped =
            invertedRelationalText(Inner->getCondition()))
      InnerShown = *Swapped;
    else
      InnerShown = Inverted(std::move(InnerShown));
    if (!InnerShown.empty() && sharedAssignElse(From, Body, Alt, Def, Join)) {
      auto WriteArm = [&](const llvm::BasicBlock *Arm, int ArmIndent) {
        const auto SavedLast = AllocaLastValues;
        AllocaLastValues.clear();
        for (llvm::Instruction &ArmInst :
             *const_cast<llvm::BasicBlock *>(Arm)) {
          if (ArmInst.isTerminator() || llvm::isa<llvm::AllocaInst>(&ArmInst))
            continue;
          writeInstruction(ArmInst, ArmIndent);
        }
        AllocaLastValues = SavedLast;
      };
      DuplicatedAssignBlocks.insert(Alt);
      DuplicatedAssignBlocks.insert(Def);
      bool SeenFrom = false;
      bool Falls = false;
      for (const llvm::BasicBlock &Next : *From->getParent()) {
        if (!SeenFrom) {
          if (&Next == From)
            SeenFrom = true;
          continue;
        }
        if (&Next == Body || &Next == Alt || &Next == Def ||
            FoldedJoinArms.count(&Next) ||
            InlinedFallthroughBlocks.count(&Next) ||
            ConditionChainBodies.count(&Next) ||
            DuplicatedAssignBlocks.count(&Next))
          continue;
        if (&Next != &From->getParent()->getEntryBlock() &&
            llvm::pred_empty(&Next))
          continue;
        if (isPrintPassthrough(&Next) ||
            llvm::isa<llvm::CatchSwitchInst>(Next.getTerminator()))
          continue;
        Falls = &Next == Join || printBranchTarget(&Next) == Join;
        break;
      }
      emitIndent(Indent + 1);
      OS << "if (" << InnerShown << ") {\n";
      WriteArm(Alt, Indent + 2);
      emitIndent(Indent + 1);
      OS << "} else {\n";
      WriteArm(Def, Indent + 2);
      emitIndent(Indent + 1);
      OS << "}\n";
      if (!Falls)
        writeGoto(Body, Join, Indent + 1);
      emitIndent(Indent);
      OS << "} else {\n";
      WriteArm(Def, Indent + 1);
      if (!Falls)
        writeGoto(From, Join, Indent + 1);
      emitIndent(Indent);
      OS << "}\n";
      InlinedFallthroughBlocks.insert(Body);
      return;
    }
    writeInstruction(*const_cast<llvm::CondBrInst *>(Inner), Indent + 1);
  }
  emitIndent(Indent);
  OS << "}\n";
  InlinedFallthroughBlocks.insert(Body);
  // The true edge's call would otherwise fall into a later skip goto.
  if (duplicatedCallTail(Then))
    writeGoto(From, Then, Indent);
}

void LLVMCWriter::writeCondTrueEdge(const llvm::BasicBlock *From,
                                    const llvm::BasicBlock *Then, int Indent,
                                    bool AllowFallthrough) {
  auto FallsIntoNext = [&](const llvm::BasicBlock *Succ) {
    return AllowFallthrough && joinPrintsNext(From, Succ) &&
           !cursorForExitsAt(Succ);
  };
  const llvm::BasicBlock *Body = straightLineTrueArm(From, Then, true);
  if (!Body) {
    const llvm::BasicBlock *Region = nullptr;
    llvm::SmallVector<const llvm::BasicBlock *, 4> TrueChain;
    llvm::SmallVector<const llvm::BasicBlock *, 4> FalseChain;
    if (singleEntryCondRegion(From, Then, Region, TrueChain, FalseChain)) {
      auto WriteChain = [&](llvm::ArrayRef<const llvm::BasicBlock *> Chain) {
        for (const llvm::BasicBlock *Tail : Chain) {
          const auto SavedKnown = KnownImmediates;
          const auto SavedLast = AllocaLastValues;
          const auto SavedImm = AllocaImmediates;
          KnownImmediates.clear();
          AllocaLastValues.clear();
          AllocaImmediates.clear();
          for (llvm::Instruction &TailInst :
               *const_cast<llvm::BasicBlock *>(Tail)) {
            if (TailInst.isTerminator() ||
                llvm::isa<llvm::AllocaInst>(&TailInst))
              continue;
            writeInstruction(TailInst, Indent + 1);
          }
          KnownImmediates = SavedKnown;
          AllocaLastValues = SavedLast;
          AllocaImmediates = SavedImm;
        }
        if (const auto *TailBr = llvm::dyn_cast<llvm::UncondBrInst>(
                Chain.back()->getTerminator())) {
          const bool Falls = FallsIntoNext(TailBr->getSuccessor(0));
          writeGoto(Chain.back(), TailBr->getSuccessor(0), Indent + 1, !Falls);
        }
      };
      const auto SavedKnown = KnownImmediates;
      const auto SavedImm = AllocaImmediates;
      const auto SavedLast = AllocaLastValues;
      KnownImmediates.clear();
      AllocaImmediates.clear();
      AllocaLastValues.clear();
      for (llvm::Instruction &Inst : *const_cast<llvm::BasicBlock *>(Region)) {
        if (Inst.isTerminator() || llvm::isa<llvm::AllocaInst>(&Inst))
          continue;
        writeInstruction(Inst, Indent);
      }
      const auto *RegionBr =
          llvm::cast<llvm::CondBrInst>(Region->getTerminator());
      DuplicatedAssignBlocks.insert(Region);
      for (const llvm::BasicBlock *Tail : TrueChain)
        DuplicatedAssignBlocks.insert(Tail);
      for (const llvm::BasicBlock *Tail : FalseChain)
        DuplicatedAssignBlocks.insert(Tail);
      emitIndent(Indent);
      OS << "if (" << condStr(RegionBr->getCondition()) << ") {\n";
      WriteChain(TrueChain);
      emitIndent(Indent);
      OS << "} else {\n";
      WriteChain(FalseChain);
      emitIndent(Indent);
      OS << "}\n";
      KnownImmediates = SavedKnown;
      AllocaImmediates = SavedImm;
      AllocaLastValues = SavedLast;
      return;
    }
    writeGoto(From, Then, Indent);
    return;
  }
  for (llvm::Instruction &Inst : *const_cast<llvm::BasicBlock *>(Body)) {
    if (Inst.isTerminator() || llvm::isa<llvm::AllocaInst>(&Inst))
      continue;
    writeInstruction(Inst, Indent);
  }
  if (const auto *Br =
          llvm::dyn_cast<llvm::UncondBrInst>(Body->getTerminator())) {
    llvm::SmallVector<const llvm::BasicBlock *, 4> Chain;
    if (singleEntryUncondTail(From, Body, Br->getSuccessor(0), Chain)) {
      for (const llvm::BasicBlock *Tail : Chain)
        InlinedFallthroughBlocks.insert(Tail);
      for (const llvm::BasicBlock *Tail : Chain) {
        const auto SavedLast = AllocaLastValues;
        AllocaLastValues.clear();
        for (llvm::Instruction &TailInst :
             *const_cast<llvm::BasicBlock *>(Tail)) {
          if (TailInst.isTerminator() || llvm::isa<llvm::AllocaInst>(&TailInst))
            continue;
          writeInstruction(TailInst, Indent);
        }
        AllocaLastValues = SavedLast;
      }
      if (const auto *TailBr = llvm::dyn_cast<llvm::UncondBrInst>(
              Chain.back()->getTerminator())) {
        const bool FallsThrough = FallsIntoNext(TailBr->getSuccessor(0));
        writeGoto(Chain.back(), TailBr->getSuccessor(0), Indent, !FallsThrough);
      }
    } else {
      const bool FallsThrough = FallsIntoNext(Br->getSuccessor(0));
      writeGoto(Body, Br->getSuccessor(0), Indent, !FallsThrough);
    }
  } else if (auto *Ret =
                 llvm::dyn_cast<llvm::ReturnInst>(Body->getTerminator())) {
    writeInstruction(*const_cast<llvm::ReturnInst *>(Ret), Indent);
  } else if (llvm::isa<llvm::UnreachableInst>(Body->getTerminator())) {
    writeInstruction(*const_cast<llvm::Instruction *>(Body->getTerminator()),
                     Indent);
    ReferencedBlocks.erase(Body);
  }
  InlinedFallthroughBlocks.insert(Body);
}

bool LLVMCWriter::phiPrintedAsJoinCallArg(const llvm::PHINode *Phi) {
  if (!Phi)
    return false;
  const llvm::BasicBlock *BB = Phi->getParent();
  if (!BB)
    return false;
  for (const llvm::Instruction &Inst : *BB) {
    const auto *CB = llvm::dyn_cast<llvm::CallBase>(&Inst);
    if (!CB)
      continue;
    for (unsigned I = 0, N = CB->arg_size(); I < N; ++I)
      if (joinPhiForCallArg(CB->getArgOperand(I), BB) == Phi)
        return true;
  }
  return false;
}

bool LLVMCWriter::phiIncomingIsPrinted(const llvm::PHINode *Phi,
                                       llvm::Value *Incoming,
                                       bool ForceMaterialized) {
  if (!Phi || !Incoming || Analysis.Inlinable.count(Phi))
    return false;
  if ((OmittedUnknowns.count(Phi) || OmittedInlined.count(Phi) ||
       usersAreDeadCopies(Phi)) &&
      !phiPrintedAsJoinCallArg(Phi))
    return false;
  if (!ForceMaterialized && Phi->getParent()->getSinglePredecessor() &&
      foldImmediate(Incoming) && usersOnlySeeImmediate(Phi) &&
      !phiPrintedAsJoinCallArg(Phi))
    return false;
  if ((isUnknownPlaceholder(Incoming) || isCallClobberValue(Incoming)) &&
      usersOnlySeeImmediate(Phi))
    return false;
  return true;
}

bool LLVMCWriter::phiEdgeNeedsMaterialization(
    const llvm::BasicBlock *From, const llvm::BasicBlock *To) const {
  if (!From || !To)
    return false;
  if (EHMovedContinuationBlocks.count(To))
    return true;
  const auto *Invoke = llvm::dyn_cast<llvm::InvokeInst>(From->getTerminator());
  if (Invoke && Invoke->getNormalDest() == To)
    return true;
  // A reserved invoke goto may skip nonprinting blocks whose outgoing PHI
  // copies still belong to the invoke's normal path.  writeInvoke clears
  // path-local immediates after each edge, so every such copy needs a C
  // assignment, including constant incoming values.
  for (const auto &[InvokeBB, PrintedTarget] : EHInvokeNormalGotos) {
    const auto *Reserved =
        llvm::dyn_cast<llvm::InvokeInst>(InvokeBB->getTerminator());
    if (!Reserved)
      continue;
    const llvm::BasicBlock *EdgeFrom = InvokeBB;
    const llvm::BasicBlock *EdgeTo = Reserved->getNormalDest();
    llvm::SmallPtrSet<const llvm::BasicBlock *, 8> Seen;
    bool Matches = false;
    while (EdgeTo && Seen.insert(EdgeTo).second) {
      Matches |= EdgeFrom == From && EdgeTo == To;
      if (EdgeTo == PrintedTarget) {
        if (Matches)
          return true;
        break;
      }
      const auto *Bridge =
          llvm::dyn_cast<llvm::UncondBrInst>(EdgeTo->getTerminator());
      if (!Bridge)
        break;
      EdgeFrom = EdgeTo;
      EdgeTo = Bridge->getSuccessor(0);
    }
  }
  return false;
}

bool LLVMCWriter::edgePrintsPhiCopy(const llvm::BasicBlock *From,
                                    const llvm::BasicBlock *To) {
  std::set<const llvm::BasicBlock *> Seen;
  const llvm::BasicBlock *CurFrom = From;
  const llvm::BasicBlock *CurTo = To;
  auto Copies = [&](const llvm::BasicBlock *Pred,
                    const llvm::BasicBlock *Dest) {
    if (!Pred || !Dest)
      return false;
    for (const llvm::Instruction &Inst : *Dest) {
      const auto *Phi = llvm::dyn_cast<llvm::PHINode>(&Inst);
      if (!Phi)
        break;
      if (Phi->getBasicBlockIndex(Pred) < 0)
        continue;
      if (phiIncomingIsPrinted(Phi, Phi->getIncomingValueForBlock(Pred),
                               phiEdgeNeedsMaterialization(Pred, Dest)))
        return true;
    }
    return false;
  };
  while (CurTo && Seen.insert(CurTo).second && isPrintPassthrough(CurTo)) {
    if (Copies(CurFrom, CurTo))
      return true;
    CurFrom = CurTo;
    CurTo = CurTo->getTerminator()->getSuccessor(0);
  }
  return Copies(CurFrom, CurTo);
}

void LLVMCWriter::writePhiCopies(const llvm::BasicBlock *From,
                                 const llvm::BasicBlock *To, int Indent,
                                 bool ForceMaterialized,
                                 const llvm::PHINode *Deferred,
                                 const llvm::PHINode *Declare) {
  if (!From || !To)
    return;
  struct PhiCopy {
    const llvm::PHINode *Phi;
    const llvm::Value *Incoming;
    std::string RHS;
    std::string Destination;
    std::string Temp;
  };
  std::vector<PhiCopy> Copies;
  std::map<const llvm::Value *, std::string> EdgeImmediates;
  const bool SinglePredecessor = To->getSinglePredecessor() != nullptr;
  const bool MaterializeEdge =
      ForceMaterialized || phiEdgeNeedsMaterialization(From, To);
  for (const llvm::Instruction &Inst : *To) {
    const auto *Phi = llvm::dyn_cast<llvm::PHINode>(&Inst);
    if (!Phi)
      break;
    if (Phi == Deferred ||
        (Phi->use_empty() && !phiPrintedAsJoinCallArg(Phi)) ||
        Analysis.Inlinable.count(Phi) || Analysis.DeadFrameStores.count(Phi))
      continue;
    const int IncomingIndex = Phi->getBasicBlockIndex(From);
    if (IncomingIndex < 0)
      continue;
    llvm::Value *Incoming = Phi->getIncomingValue(IncomingIndex);
    if (SinglePredecessor)
      if (auto Imm = foldImmediate(Incoming))
        EdgeImmediates.emplace(Phi, *Imm);
    if (phiIncomingIsPrinted(Phi, Incoming, MaterializeEdge)) {
      const std::string RHS = integerPointerOperandStr(Incoming);
      const std::string Destination = getName(Phi);
      if (!ForceMaterialized || RHS != Destination)
        Copies.push_back({Phi, Incoming, RHS, Destination, {}});
    }
  }
  // Evaluate the whole edge against its predecessor state before publishing
  // any new PHI facts. Loop PHIs may exchange values, including cached ones.
  for (const llvm::PHINode &Phi : To->phis())
    KnownImmediates.erase(&Phi);
  for (const auto &[Phi, Immediate] : EdgeImmediates)
    KnownImmediates[Phi] = Immediate;
  if (Copies.empty())
    return;

  // Inspect rendered operands: inlining and path-local substitutions can make
  // their variable reads differ from the immediate LLVM operands. Retain RHS
  // evaluation order and delay a destination only if a later RHS reads its old
  // value. Tokenize once per RHS, rather than searching every destination in
  // every expression. Mentions in literals may conservatively keep a temp.
  std::map<std::string, size_t, std::less<>> LastRead;
  for (const auto &Copy : Copies)
    LastRead.emplace(Copy.Destination, 0);
  for (size_t I = 0; I < Copies.size(); ++I) {
    llvm::StringRef RHS = Copies[I].RHS;
    for (size_t Begin = 0; Begin < RHS.size();) {
      if (!std::isalpha(static_cast<unsigned char>(RHS[Begin])) &&
          RHS[Begin] != '_') {
        ++Begin;
        continue;
      }
      size_t End = Begin + 1;
      while (End < RHS.size() &&
             (std::isalnum(static_cast<unsigned char>(RHS[End])) ||
              RHS[End] == '_'))
        ++End;
      auto Found =
          LastRead.find(std::string_view(RHS.data() + Begin, End - Begin));
      if (Found != LastRead.end())
        Found->second = I + 1;
      Begin = End;
    }
  }
  bool HasTemps = false;
  for (size_t I = 0; I < Copies.size(); ++I) {
    auto &Copy = Copies[I];
    if (!isCIdentifier(Copy.Destination) ||
        LastRead.at(Copy.Destination) > I + 1) {
      Copy.Temp = freshVar("phi_edge");
      HasTemps = true;
    }
  }
  if (HasTemps) {
    emitIndent(Indent);
    OS << "{\n";
  }
  const int CopyIndent = Indent + (HasTemps ? 1 : 0);
  for (const auto &Copy : Copies) {
    emitIndent(CopyIndent);
    if (Copy.Phi == Declare && !HasTemps && Indent == 1) {
      ScopedScalarNames.insert(Copy.Destination);
      OS << typeToCLLVM(Copy.Phi->getType()) << " " << Copy.Destination << " = "
         << Copy.RHS << ";\n";
      continue;
    }
    if (Copy.Temp.empty())
      if (auto Update = scalarConditionalUpdate(Copy.Phi, Copy.Incoming)) {
        if (!Update->Initial.empty()) {
          OS << Update->Initial << ";\n";
          emitIndent(CopyIndent);
        }
        OS << "if (" << Update->Condition << ") " << Update->Update << ";\n";
        continue;
      }
    if (Copy.Temp.empty())
      if (auto Update = scalarUpdateText(Copy.Phi, Copy.Incoming)) {
        OS << *Update << ";\n";
        continue;
      }
    if (!Copy.Temp.empty())
      OS << typeToCLLVM(Copy.Phi->getType()) << " " << Copy.Temp;
    else
      OS << Copy.Destination;
    OS << " = " << Copy.RHS << ";\n";
  }
  for (const auto &Copy : Copies) {
    if (Copy.Temp.empty())
      continue;
    emitIndent(CopyIndent);
    OS << Copy.Destination << " = " << Copy.Temp << ";\n";
  }
  if (HasTemps) {
    emitIndent(Indent);
    OS << "}\n";
  }
}

std::string
LLVMCWriter::resolveImportCalleeName(const llvm::Value *Callee) const {
  if (!Callee)
    return {};
  const llvm::Value *Op = Callee->stripPointerCasts();
  // A call to a symbol the module declares as data, such as an import its
  // GOT mirror names, calls that import.
  if (const auto *GV = llvm::dyn_cast<llvm::GlobalVariable>(Op);
      GV && GV->isDeclaration() && !GV->getName().empty())
    return canonicalizeCProjectionIdentifier(cNameOfGlobal(GV->getName(), true),
                                             "nd_import");
  for (unsigned Depth = 0; Op && Depth < 6; ++Depth) {
    if (const auto *Fn = llvm::dyn_cast<llvm::Function>(Op))
      return functionIdentifier(*Fn);
    if (const auto *CI = llvm::dyn_cast<llvm::CastInst>(Op)) {
      if (CI->getOpcode() == llvm::Instruction::IntToPtr ||
          CI->getOpcode() == llvm::Instruction::PtrToInt ||
          CI->getOpcode() == llvm::Instruction::BitCast) {
        Op = CI->getOperand(0)->stripPointerCasts();
        continue;
      }
    }
    if (const auto *CE = llvm::dyn_cast<llvm::ConstantExpr>(Op)) {
      if (CE->isCast()) {
        Op = CE->getOperand(0)->stripPointerCasts();
        continue;
      }
    }
    if (const auto *GEP = llvm::dyn_cast<llvm::GEPOperator>(Op)) {
      Op = GEP->getPointerOperand()->stripPointerCasts();
      continue;
    }
    break;
  }
  const llvm::Value *Ptr = Op;
  if (const auto *LI = llvm::dyn_cast<llvm::LoadInst>(Op))
    Ptr = LI->getPointerOperand()->stripPointerCasts();

  auto nameFromSlot = [&](va_t Slot) -> std::string {
    if (Dbg) {
      if (auto Data = Dbg->resolveDataObject(Slot); Data && !Data->Name.empty())
        return canonicalizeCProjectionIdentifier(Data->Name, "nd_import");
      if (auto FS = Dbg->resolveFunction(Slot); FS && !FS->Name.empty())
        return canonicalizeCProjectionIdentifier(FS->Name, "nd_import");
    }
    if (!Img)
      return {};
    if (const Import *Imp = Img->findImportAt(Slot); Imp && !Imp->Name.empty())
      return canonicalizeCProjectionIdentifier(Imp->Name, "nd_import");
    // A slot the loader binds to one import, such as an ELF GOT entry, names
    // it while the call still loads the slot.  An identifier spelled from
    // the symbol could merge two symbols, such as C++ overloads, so such an
    // import stays unnamed here.
    if (const auto Bound = Img->ImportStorageSlots.find(Slot);
        Bound != Img->ImportStorageSlots.end() &&
        !Img->ConflictingImportStorageSlots.count(Slot) &&
        Bound->second.Addend == 0 && !Bound->second.Name.empty()) {
      const llvm::StringRef Name =
          cNameOfSymbol(Bound->second.Name, Opts.Format, Opts.TheArch);
      if (!identifierSpelledFromSymbol(Name) && !hasMsvcStem(Name))
        return canonicalizeCProjectionIdentifier(Name, "nd_import");
    }
    return {};
  };

  if (auto Slot = imageDataVA(Ptr)) {
    if (std::string Name = nameFromSlot(*Slot); !Name.empty())
      return Name;
  }

  const auto *GV = llvm::dyn_cast<llvm::GlobalValue>(Ptr);
  if (!GV)
    return {};
  const llvm::StringRef Raw = GV->getName();
  if (Raw.starts_with("__imp_") || Raw.starts_with("_imp_") ||
      Raw.starts_with("??") || Raw.starts_with("ord_")) {
    const std::string Canon =
        canonicalizeCProjectionIdentifier(Raw, "nd_import");
    if (!Canon.empty())
      return Canon;
  }
  if (auto Slot = parseNdCodePtrSymbol(Raw)) {
    if (std::string Name = nameFromSlot(*Slot); !Name.empty())
      return Name;
  }
  if (auto Slot = parseNdDataSymbol(Raw)) {
    if (std::string Name = nameFromSlot(*Slot); !Name.empty())
      return Name;
  }
  return {};
}

bool LLVMCWriter::isImportCalleeOnlyLoad(const llvm::LoadInst *LI) const {
  if (!LI || LI->use_empty())
    return false;
  for (const llvm::User *User : LI->users()) {
    const llvm::Value *Callee = User;
    if (const auto *Cast = llvm::dyn_cast<llvm::CastInst>(User)) {
      if (Cast->user_empty())
        return false;
      for (const llvm::User *CastUser : Cast->users()) {
        const auto *Call = llvm::dyn_cast<llvm::CallBase>(CastUser);
        if (!Call || Call->getCalledOperand() != Cast ||
            resolveImportCalleeName(Call->getCalledOperand()).empty())
          return false;
      }
      continue;
    }
    const auto *Call = llvm::dyn_cast<llvm::CallBase>(User);
    if (!Call || Call->getCalledOperand() != LI ||
        resolveImportCalleeName(Call->getCalledOperand()).empty())
      return false;
  }
  return true;
}

bool LLVMCWriter::callDoesNotReturn(const llvm::CallBase &Call) const {
  if (Call.doesNotReturn())
    return true;
  const auto *Fn = Call.getCalledFunction();
  if (!Fn)
    return false;
  llvm::StringRef Name = Fn->getName();
  if (isX86FastFailName(Name))
    return true;
  return libc::isNoReturnFunction(Name);
}

void LLVMCWriter::writeCall(llvm::CallInst &Call, const std::string &Name,
                            int Indent) {
  writeCallLike(Call, Name, Indent);
}

void LLVMCWriter::writeInvoke(llvm::InvokeInst &Invoke, const std::string &Name,
                              int Indent) {
  writeCallLike(Invoke, Name, Indent);
  // An invoke's normal successor can consume a PHI even when the invoke is
  // only an EH boundary intrinsic.  Its unwind edge must not assign the
  // normal incoming value, so emit the copy only after the call returns.
  if (!AfterCxxThrow) {
    auto WriteNormalCopy = [&](const llvm::BasicBlock *From,
                               const llvm::BasicBlock *To) {
      writePhiCopies(From, To, Indent);
      // An invoke's normal edge is path-local.  A shared normal/handler
      // continuation cannot inherit its immediate on the later block walk.
      for (const llvm::Instruction &Inst : *To) {
        const auto *Phi = llvm::dyn_cast<llvm::PHINode>(&Inst);
        if (!Phi)
          break;
        KnownImmediates.erase(Phi);
      }
    };
    if (auto It = EHInvokeNormalGotos.find(Invoke.getParent());
        It != EHInvokeNormalGotos.end()) {
      const llvm::BasicBlock *From = Invoke.getParent();
      const llvm::BasicBlock *To = Invoke.getNormalDest();
      llvm::SmallPtrSet<const llvm::BasicBlock *, 8> Seen;
      while (To && To != It->second && isPrintPassthrough(To) &&
             Seen.insert(To).second) {
        WriteNormalCopy(From, To);
        From = To;
        To = To->getTerminator()->getSuccessor(0);
      }
      if (To != It->second)
        llvm::report_fatal_error(
            "LLVMC EH invoke normal path changed before rendering");
      WriteNormalCopy(From, To);
      emitIndent(Indent);
      OS << "goto " << blockLabel(It->second) << ";\n";
    } else {
      WriteNormalCopy(Invoke.getParent(), Invoke.getNormalDest());
    }
  }
}

std::string LLVMCWriter::windowsEHFilterExpr(const llvm::CatchSwitchInst &CS) {
  if (CS.getNumHandlers() == 0)
    return "EXCEPTION_EXECUTE_HANDLER";
  const llvm::BasicBlock *PadBB = *CS.handler_begin();
  const llvm::Instruction *First = nullptr;
  if (PadBB)
    for (const llvm::Instruction &Inst : *PadBB)
      if (!llvm::isa<llvm::PHINode>(&Inst)) {
        First = &Inst;
        break;
      }
  const auto *Pad = llvm::dyn_cast_or_null<llvm::CatchPadInst>(First);
  if (!Pad || Pad->arg_size() == 0)
    return "EXCEPTION_EXECUTE_HANDLER";
  llvm::Value *Filter = Pad->getArgOperand(0)->stripPointerCasts();
  if (llvm::isa<llvm::ConstantPointerNull>(Filter))
    return "EXCEPTION_EXECUTE_HANDLER";
  if (const auto *Fn = llvm::dyn_cast<llvm::Function>(Filter)) {
    std::string Args = "GetExceptionInformation()";
    if (Fn->arg_size() == 2 && Fn->getArg(0)->getType()->isPointerTy() &&
        Fn->getArg(1)->getType()->isPointerTy()) {
      if (SEHFilterFrameName.empty())
        SEHFilterFrameName = freshVar("seh_parent_frame");
      Args += ", " + SEHFilterFrameName;
    } else if (Fn->arg_size() != 1 ||
               !Fn->getArg(0)->getType()->isPointerTy()) {
      throw std::runtime_error("unsupported Windows SEH filter source ABI");
    }
    return functionIdentifier(*Fn) + "(" + Args + ")";
  }
  return "nd_seh_filter_0x0(GetExceptionInformation())";
}

std::string LLVMCWriter::windowsCxxCatchType(const llvm::CatchPadInst &Pad) {
  uint32_t Adjectives = 0;
  if (Pad.arg_size() >= 2)
    if (const auto *CI =
            llvm::dyn_cast<llvm::ConstantInt>(Pad.getArgOperand(1)))
      Adjectives = static_cast<uint32_t>(CI->getZExtValue());
  if ((Adjectives & kCxxCatchAll) != 0 || Pad.arg_size() == 0)
    return "...";
  std::string Type = "...";
  if (Pad.arg_size() != 0) {
    llvm::Value *Desc = Pad.getArgOperand(0)->stripPointerCasts();
    if (const auto *GV = llvm::dyn_cast<llvm::GlobalValue>(Desc)) {
      if (auto VA = parseNdDataSymbol(GV->getName())) {
        std::string Recovered = cxxTypeFromDescriptor(Img, *VA);
        if (!Recovered.empty() && isCIdentifier(Recovered))
          Type = Recovered;
        else
          Type = "/* type @ 0x" + llvm::utohexstr(*VA) + " */";
      }
    }
  }
  std::string Result;
  if (Adjectives & kCxxCatchConst)
    Result += "const ";
  if (Adjectives & kCxxCatchVolatile)
    Result += "volatile ";
  Result += Type;
  if (Adjectives & kCxxCatchReference)
    Result += " &";
  return Result;
}

void LLVMCWriter::writeCatchSwitch(llvm::CatchSwitchInst &CS, int Indent) {
  emitIndent(Indent);
  const llvm::BasicBlock *Target = nullptr;
  if (CS.getNumHandlers() != 0) {
    const llvm::BasicBlock *PadBB = *CS.handler_begin();
    if (PadBB)
      for (const llvm::Instruction &Inst : *PadBB)
        if (const auto *Ret = llvm::dyn_cast<llvm::CatchReturnInst>(&Inst)) {
          Target = Ret->getSuccessor();
          break;
        }
    if (!Target)
      Target = PadBB;
  }
  const llvm::Instruction *First = nullptr;
  if (CS.getNumHandlers())
    for (const llvm::Instruction &Inst : **CS.handler_begin())
      if (!llvm::isa<llvm::PHINode>(&Inst)) {
        First = &Inst;
        break;
      }
  const auto *Pad = llvm::dyn_cast_or_null<llvm::CatchPadInst>(First);
  const bool Cxx = Pad && Pad->arg_size() >= 2;
  if (Cxx)
    OS << "/* catch (" << (Pad ? windowsCxxCatchType(*Pad) : "...") << ") */ ";
  else
    OS << "/* __except (" << windowsEHFilterExpr(CS) << ") */ ";
  if (Target)
    OS << "goto " << blockLabel(Target) << ";\n";
  else
    OS << ";\n";
}

void LLVMCWriter::writeCleanupRet(llvm::CleanupReturnInst &CR, int Indent) {
  llvm::BasicBlock *Dest = CR.getUnwindDest();
  if (OmitCleanupRetTo.count(Dest))
    return;
  emitIndent(Indent);
  if (!Dest)
    OS << "return; /* __finally */\n";
  else if (llvm::isa<llvm::CatchSwitchInst>(Dest->getTerminator()))
    OS << "/* __finally */\n";
  else
    OS << "goto " << blockLabel(Dest) << "; /* __finally */\n";
}

void LLVMCWriter::writeReturn(llvm::ReturnInst &Ret, int Indent) {
  emitIndent(Indent);
  if (InferredVoid) {
    OS << "return;\n";
    return;
  }
  if (auto *RV = Ret.getReturnValue()) {
    if (auto *Collapsed = tryCollapseHiLo(Analysis, RV)) {
      OS << "return " << valueStr(Collapsed) << ";\n";
    } else {
      auto *FnRetTy = Ret.getFunction()->getReturnType();
      std::string RetExpr = valueStr(RV);
      if (RV->getType() != FnRetTy && FnRetTy->isIntegerTy() &&
          RV->getType()->isIntegerTy() &&
          FnRetTy->getIntegerBitWidth() < RV->getType()->getIntegerBitWidth())
        RetExpr = "(" + typeToCLLVM(FnRetTy) + ")" + RetExpr;
      OS << "return " << RetExpr << ";\n";
    }
  } else {
    OS << "return;\n";
  }
}

} // namespace neverd
