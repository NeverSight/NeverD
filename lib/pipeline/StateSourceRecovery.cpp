//===- StateSourceRecovery.cpp - Project explicit state ABI contracts -----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Keep unknown boundaries intact while exposing proven scalar source calls.

#include "neverd/pipeline/StateSourceRecovery.h"

#include "StateSourceBank.h"

#include "neverd/backend/llvm/LLVMSourceTypeHints.h"
#include "neverd/pipeline/Limits.h"
#include "neverd/pipeline/Pipeline.h"

#include "llvm/ADT/BitVector.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/ADT/StringSwitch.h"
#include "llvm/Analysis/ConstantFolding.h"
#include "llvm/IR/Dominators.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/InstIterator.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/MDBuilder.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Verifier.h"
#include "llvm/Support/Errc.h"
#include "llvm/Transforms/Utils/BasicBlockUtils.h"
#include "llvm/Transforms/Utils/Cloning.h"

#include <algorithm>
#include <map>
#include <set>

namespace neverd {
namespace {
using state_source::bankOffsets;
using state_source::displacement;
using state_source::validateBank;
constexpr llvm::StringLiteral ProbePrefix = "__neverd_state_probe_";
llvm::Error invalid(llvm::StringRef Message) {
  return llvm::createStringError(llvm::errc::invalid_argument,
                                 "state source contract: " + Message);
}
llvm::Type *type(llvm::LLVMContext &C, StateSourceType T) {
  switch (T) {
  case StateSourceType::Void:
    return llvm::Type::getVoidTy(C);
  case StateSourceType::U8:
  case StateSourceType::I8:
    return llvm::Type::getInt8Ty(C);
  case StateSourceType::U16:
  case StateSourceType::I16:
    return llvm::Type::getInt16Ty(C);
  case StateSourceType::U32:
  case StateSourceType::I32:
    return llvm::Type::getInt32Ty(C);
  case StateSourceType::U64:
  case StateSourceType::I64:
    return llvm::Type::getInt64Ty(C);
  case StateSourceType::Pointer:
    return llvm::PointerType::getUnqual(C);
  }
  llvm_unreachable("invalid source type");
}
bool signedType(StateSourceType T) {
  return T == StateSourceType::I8 || T == StateSourceType::I16 ||
         T == StateSourceType::I32 || T == StateSourceType::I64;
}
bool identifier(llvm::StringRef Name) {
  return !Name.empty() &&
         Name.size() <= ::neverd::limits::pipeline::
                            kStateSourceRecoveryIdentifierNameSizeLimit &&
         (llvm::isAlpha(Name.front()) || Name.front() == '_') &&
         llvm::all_of(Name,
                      [](char C) { return llvm::isAlnum(C) || C == '_'; }) &&
         !llvm::StringSwitch<bool>(Name)
              .Cases({"auto",         "break",     "case",
                      "char",         "const",     "continue",
                      "default",      "do",        "double",
                      "else",         "enum",      "extern",
                      "float",        "for",       "goto",
                      "if",           "inline",    "int",
                      "long",         "register",  "restrict",
                      "return",       "short",     "signed",
                      "sizeof",       "static",    "struct",
                      "switch",       "typedef",   "union",
                      "unsigned",     "void",      "volatile",
                      "while",        "alignas",   "alignof",
                      "bool",         "constexpr", "nullptr",
                      "false",        "true",      "static_assert",
                      "thread_local", "typeof",    "typeof_unqual",
                      "_Alignas",     "_Alignof",  "_Atomic",
                      "_Bool",        "_Complex",  "_Generic",
                      "_Imaginary",   "_Noreturn", "_Static_assert",
                      "_Thread_local"},
                     true)
              .Default(false);
}
std::vector<StateSourceType> nativeParameters(const StateSourceImport &I,
                                              bool FixedOnly = false) {
  std::vector<StateSourceType> Parameters;
  if (I.PassContext)
    Parameters.push_back(StateSourceType::Pointer);
  unsigned Count = FixedOnly
                       ? I.VariadicFixedParameters.value_or(I.Parameters.size())
                       : I.Parameters.size();
  for (unsigned P = 0; P < Count; ++P)
    if (I.TargetArgument != P)
      Parameters.push_back(I.Parameters[P]);
  return Parameters;
}
llvm::FunctionType *signature(llvm::LLVMContext &C,
                              const StateSourceImport &I) {
  std::vector<llvm::Type *> Parameters;
  for (auto T : nativeParameters(I, true))
    Parameters.push_back(type(C, T));
  return llvm::FunctionType::get(type(C, I.Result), Parameters,
                                 I.VariadicFixedParameters.has_value());
}
llvm::StringRef sourceScalarSpelling(StateSourceType T) {
  switch (T) {
  case StateSourceType::I8:
    return "int8_t";
  case StateSourceType::U8:
    return "uint8_t";
  case StateSourceType::I16:
    return "int16_t";
  case StateSourceType::U16:
    return "uint16_t";
  case StateSourceType::I32:
    return "int32_t";
  case StateSourceType::U32:
    return "uint32_t";
  case StateSourceType::I64:
    return "int64_t";
  case StateSourceType::U64:
    return "uint64_t";
  case StateSourceType::Pointer:
    return "void*";
  case StateSourceType::Void:
    llvm_unreachable("void variadic operand");
  }
  llvm_unreachable("invalid source type");
}

// Only the successful imported arm has zero gateway status. Fold its pure
// integer conditions without changing the original call or failure edges.
llvm::Constant *normalReturnConstant(llvm::Value *V, llvm::CallInst &Call,
                                     const llvm::DataLayout &DL,
                                     unsigned &Budget, unsigned Depth = 0) {
  if (!Budget || Depth >=
                     ::neverd::limits::pipeline::
                         kStateSourceRecoveryNormalReturnConstantDepthLimit)
    return nullptr;
  --Budget;
  if (V == &Call)
    return llvm::ConstantInt::get(Call.getType(), 0);
  if (auto *K = llvm::dyn_cast<llvm::Constant>(V))
    return K;
  auto *I = llvm::dyn_cast<llvm::Instruction>(V);
  if (!I || !I->getType()->isIntegerTy() ||
      !(llvm::isa<llvm::BinaryOperator, llvm::ICmpInst, llvm::CastInst,
                  llvm::SelectInst>(I)))
    return nullptr;
  llvm::SmallVector<llvm::Constant *, 4> Operands;
  for (auto &Op : I->operands()) {
    auto *K = normalReturnConstant(Op.get(), Call, DL, Budget, Depth + 1);
    if (!K)
      return nullptr;
    Operands.push_back(K);
  }
  return llvm::ConstantFoldInstOperands(I, Operands, DL, nullptr,
                                        /*AllowNonDeterministic=*/false);
}

// A void native call cannot reproduce unspecified physical result bits.
// Omit their bank write only if all bytes are killed on every normal-return
// path before a possible observer. Unknown accesses, calls, exits, cycles and
// exhausted budgets fail closed. The untouched gateway remains the fallback.
bool resultWriteIsDead(llvm::CallInst &Call, StateSourceSlot Result,
                       unsigned StateBytes, const llvm::DataLayout &DL) {
  using Key = std::pair<llvm::Instruction *, unsigned>;
  std::set<Key> Active, Proven;
  unsigned Budget =
      ::neverd::limits::pipeline::kStateSourceRecoveryResultWriteIsDeadBudget;
  auto *Bank = Call.getArgOperand(0);
  auto Check = [&](auto &&Self, llvm::Instruction *Start, unsigned Live,
                   unsigned Depth) -> bool {
    if (!Start || !Budget ||
        Depth >= ::neverd::limits::pipeline::
                     kStateSourceRecoveryResultWriteIsDeadCheckDepthLimit)
      return false;
    Key K{Start, Live};
    if (Proven.count(K))
      return true;
    if (!Active.insert(K).second)
      return false;
    auto Scan = [&]() -> bool {
      for (auto *I = Start; I; I = I->getNextNode()) {
        if (!Budget)
          return false;
        --Budget;
        llvm::Value *Address = nullptr;
        llvm::Type *AccessType = nullptr;
        bool Store = false;
        if (auto *L = llvm::dyn_cast<llvm::LoadInst>(I)) {
          if (!L->isSimple())
            return false;
          Address = L->getPointerOperand();
          AccessType = L->getType();
        } else if (auto *S = llvm::dyn_cast<llvm::StoreInst>(I)) {
          if (!S->isSimple())
            return false;
          Address = S->getPointerOperand();
          AccessType = S->getValueOperand()->getType();
          Store = true;
        }
        if (Address) {
          auto Offset = displacement(Address, Bank, DL);
          auto Size = DL.getTypeStoreSize(AccessType);
          if (!Offset || *Offset < 0 || uint64_t(*Offset) > StateBytes ||
              Size.isScalable() || Size.getFixedValue() > StateBytes - *Offset)
            return false;
          unsigned Touched = 0;
          for (unsigned B = 0; B < Result.Bytes; ++B) {
            uint64_t At = Result.Offset + B;
            if (At >= uint64_t(*Offset) && At - *Offset < Size.getFixedValue())
              Touched |= 1u << B;
          }
          if (!Store && (Live & Touched))
            return false;
          if (Store)
            Live &= ~Touched;
          if (!Live)
            return true;
          continue;
        }
        if (auto *Branch = llvm::dyn_cast<llvm::BranchInst>(I)) {
          if (Branch->isConditional())
            if (auto *C = llvm::dyn_cast_or_null<llvm::ConstantInt>(
                    normalReturnConstant(Branch->getCondition(), Call, DL,
                                         Budget)))
              return Self(Self,
                          &Branch->getSuccessor(C->isZero() ? 1 : 0)->front(),
                          Live, Depth + 1);
          for (auto *Next : Branch->successors())
            if (!Self(Self, &Next->front(), Live, Depth + 1))
              return false;
          return true;
        }
        if (auto *Switch = llvm::dyn_cast<llvm::SwitchInst>(I)) {
          if (auto *C = llvm::dyn_cast_or_null<llvm::ConstantInt>(
                  normalReturnConstant(Switch->getCondition(), Call, DL,
                                       Budget)))
            return Self(Self,
                        &Switch->findCaseValue(C)->getCaseSuccessor()->front(),
                        Live, Depth + 1);
          for (unsigned N = 0; N < Switch->getNumSuccessors(); ++N)
            if (!Self(Self, &Switch->getSuccessor(N)->front(), Live, Depth + 1))
              return false;
          return true;
        }
        if (I->isTerminator() || llvm::isa<llvm::CallBase>(I) ||
            I->mayReadOrWriteMemory() || I->mayHaveSideEffects())
          return false;
      }
      return false;
    };
    bool Success = Scan();
    Active.erase(K);
    if (Success)
      Proven.insert(K);
    return Success;
  };
  return Check(Check, Call.getNextNode(), (1u << Result.Bytes) - 1, 0);
}
// Only a complete, ordinary pointer-width load supplies slot provenance.
// This identifies a specialization candidate, not the loaded value. A later
// runtime address comparison remains mandatory, including after intervening
// guest writes or calls. Do not walk through narrow carriers or merged values.
llvm::LoadInst *pointerLoad(llvm::Value *V) {
  while (auto *Cast = llvm::dyn_cast<llvm::CastInst>(V)) {
    if (Cast->getOpcode() != llvm::Instruction::BitCast &&
        Cast->getOpcode() != llvm::Instruction::PtrToInt &&
        Cast->getOpcode() != llvm::Instruction::IntToPtr)
      return nullptr;
    auto Valid = [](llvm::Type *T) {
      return T->isIntegerTy(64) ||
             (T->isPointerTy() && T->getPointerAddressSpace() == 0);
    };
    if (!Valid(Cast->getType()) || !Valid(Cast->getOperand(0)->getType()))
      return nullptr;
    V = Cast->getOperand(0);
  }
  auto *Load = llvm::dyn_cast<llvm::LoadInst>(V);
  if (!Load || Load->isAtomic() || Load->isVolatile() ||
      (!Load->getType()->isIntegerTy(64) &&
       !(Load->getType()->isPointerTy() &&
         Load->getType()->getPointerAddressSpace() == 0)))
    return nullptr;
  return Load;
}
struct AdvisoryAddress {
  llvm::Value *Root;
  std::optional<int64_t> LoadedBankSlot;
  uint64_t Offset = 0;
};
// Repeated loads of a bank-held base can nominate an old guest writer even if
// a call changed the base or guest bytes. This is lineage, never alias or value
// equality. The eventual comparison uses the original captured selector.
std::optional<AdvisoryAddress>
advisoryAddress(llvm::Value *V, llvm::Value *Bank, const llvm::DataLayout &DL,
                unsigned &Budget, unsigned Depth = 0) {
  if (!Budget || Depth >= ::neverd::limits::pipeline::
                              kStateSourceRecoveryAdvisoryAddressDepthLimit)
    return {};
  --Budget;
  auto FullWidth = [](llvm::Type *T) {
    return T->isIntegerTy(64) ||
           (T->isPointerTy() && T->getPointerAddressSpace() == 0);
  };
  if (!FullWidth(V->getType()))
    return {};
  auto Next = [&](llvm::Value *Base, uint64_t Offset = 0) {
    auto A = advisoryAddress(Base, Bank, DL, Budget, Depth + 1);
    if (A)
      A->Offset += Offset;
    return A;
  };
  if (auto *Cast = llvm::dyn_cast<llvm::CastInst>(V))
    if (FullWidth(Cast->getOperand(0)->getType()) &&
        (Cast->getOpcode() == llvm::Instruction::BitCast ||
         Cast->getOpcode() == llvm::Instruction::PtrToInt ||
         Cast->getOpcode() == llvm::Instruction::IntToPtr))
      return Next(Cast->getOperand(0));
  if (auto *G = llvm::dyn_cast<llvm::GetElementPtrInst>(V)) {
    llvm::APInt Offset(64, 0);
    if (G->accumulateConstantOffset(DL, Offset))
      return Next(G->getPointerOperand(), Offset.getZExtValue());
  }
  if (auto *B = llvm::dyn_cast<llvm::BinaryOperator>(V)) {
    if (B->getOpcode() == llvm::Instruction::Add ||
        B->getOpcode() == llvm::Instruction::Sub) {
      auto *K = llvm::dyn_cast<llvm::ConstantInt>(B->getOperand(1));
      auto *Base = B->getOperand(0);
      if (!K && B->getOpcode() == llvm::Instruction::Add) {
        K = llvm::dyn_cast<llvm::ConstantInt>(B->getOperand(0));
        Base = B->getOperand(1);
      }
      if (K)
        return Next(Base, B->getOpcode() == llvm::Instruction::Sub
                              ? uint64_t(0) - K->getZExtValue()
                              : K->getZExtValue());
    }
  }
  if (auto *Load = llvm::dyn_cast<llvm::LoadInst>(V))
    if (Load->isSimple())
      if (auto Slot = displacement(Load->getPointerOperand(), Bank, DL))
        return AdvisoryAddress{Bank, Slot};
  return AdvisoryAddress{V, std::nullopt};
}
struct AdvisoryStore {
  llvm::StoreInst *Store;
  unsigned ByteOffset;
};
// Follow bounded dominating writers only to nominate a guarded candidate.
// Pointer provenance stays bank-only; scalar selectors may additionally use
// guest writers sharing an advisory address lineage. Neither search adds reads.
std::optional<AdvisoryStore>
advisoryStore(llvm::LoadInst &Load, llvm::Value *Bank, llvm::DominatorTree &DT,
              const llvm::DataLayout &DL, unsigned &Budget,
              bool IncludeGuest = false) {
  auto Address =
      IncludeGuest ? advisoryAddress(Load.getPointerOperand(), Bank, DL, Budget)
                   : std::optional<AdvisoryAddress>();
  auto Offset = displacement(Load.getPointerOperand(), Bank, DL);
  if (!IncludeGuest && Offset)
    Address = AdvisoryAddress{Bank, std::nullopt, uint64_t(*Offset)};
  auto Size = DL.getTypeStoreSize(Load.getType());
  if (!Address || Size.isScalable() || Size.getFixedValue() > 8)
    return {};
  auto *Block = Load.getParent();
  auto Position = Load.getIterator();
  while (Block && Budget) {
    while (Position != Block->begin() && Budget) {
      --Position;
      --Budget;
      auto *Store = llvm::dyn_cast<llvm::StoreInst>(&*Position);
      if (!Store || !Store->isSimple())
        continue;
      auto StoredAddress =
          IncludeGuest
              ? advisoryAddress(Store->getPointerOperand(), Bank, DL, Budget)
              : std::optional<AdvisoryAddress>();
      if (!IncludeGuest)
        if (auto At = displacement(Store->getPointerOperand(), Bank, DL))
          StoredAddress = AdvisoryAddress{Bank, std::nullopt, uint64_t(*At)};
      auto StoredSize =
          DL.getTypeStoreSize(Store->getValueOperand()->getType());
      if (!StoredAddress || StoredAddress->Root != Address->Root ||
          StoredAddress->LoadedBankSlot != Address->LoadedBankSlot ||
          StoredSize.isScalable() || StoredSize.getFixedValue() > 8)
        continue;
      uint64_t Delta = Address->Offset - StoredAddress->Offset;
      if (Delta <= StoredSize.getFixedValue() &&
          Size.getFixedValue() <= StoredSize.getFixedValue() - Delta)
        return AdvisoryStore{Store, unsigned(Delta)};
    }
    auto *Node = DT.getNode(Block);
    Block = Node && Node->getIDom() ? Node->getIDom()->getBlock() : nullptr;
    if (Block)
      Position = Block->end();
  }
  return {};
}
llvm::LoadInst *pointerCandidateLoad(llvm::Value *V, llvm::Value *Bank,
                                     llvm::DominatorTree &DT,
                                     const llvm::DataLayout &DL) {
  unsigned Budget = ::neverd::limits::pipeline::
      kStateSourceRecoveryPointerCandidateLoadBudget;
  while (Budget) {
    --Budget;
    auto *Load = pointerLoad(V);
    if (!Load)
      return nullptr;
    auto Offset = displacement(Load->getPointerOperand(), Bank, DL);
    if (!Offset)
      return Load;
    auto Stored = advisoryStore(*Load, Bank, DT, DL, Budget);
    if (!Stored)
      return nullptr;
    V = Stored->Store->getValueOperand();
  }
  return nullptr;
}
// Recover a possible scalar selector from its current SSA expression and old
// bank/guest writers. Byte order determines slices, but intervening clobbers
// are NOT ruled out. No inferred value replaces an operand: the captured
// selector must equal the candidate at runtime before its declared import can
// run.
llvm::ConstantInt *selectorCandidate(llvm::Value *V, llvm::Value *Bank,
                                     llvm::DominatorTree &DT,
                                     const llvm::DataLayout &DL,
                                     unsigned &Budget, unsigned Depth = 0) {
  if (!Budget ||
      Depth >= ::neverd::limits::pipeline::
                   kStateSourceRecoverySelectorCandidateDepthLimit ||
      !V->getType()->isIntegerTy() || V->getType()->getIntegerBitWidth() > 64)
    return nullptr;
  --Budget;
  if (auto *K = llvm::dyn_cast<llvm::ConstantInt>(V))
    return K;
  auto Next = [&](llvm::Value *Op) {
    return selectorCandidate(Op, Bank, DT, DL, Budget, Depth + 1);
  };
  if (auto *Phi = llvm::dyn_cast<llvm::PHINode>(V)) {
    // A join needs only one advisory candidate, not agreement of its arms.
    // Prefer literal seeds before traversing backedges; the current value
    // still needs the same runtime equality check on every iteration.
    for (auto &Incoming : Phi->incoming_values()) {
      if (!Budget)
        return nullptr;
      --Budget;
      if (auto *K = llvm::dyn_cast<llvm::ConstantInt>(Incoming))
        return K;
    }
    for (auto &Incoming : Phi->incoming_values())
      if (auto *K = Next(Incoming))
        return K;
    return nullptr;
  }
  if (auto *Select = llvm::dyn_cast<llvm::SelectInst>(V)) {
    for (auto *Arm : {Select->getTrueValue(), Select->getFalseValue()})
      if (auto *K = Next(Arm))
        return K;
    return nullptr;
  }
  if (auto *Load = llvm::dyn_cast<llvm::LoadInst>(V)) {
    if (!Load->isSimple() || Load->getType()->getIntegerBitWidth() % 8)
      return nullptr;
    auto Store = advisoryStore(*Load, Bank, DT, DL, Budget, true);
    auto *K = Store ? Next(Store->Store->getValueOperand()) : nullptr;
    if (!K || K->getBitWidth() % 8)
      return nullptr;
    unsigned Width = Load->getType()->getIntegerBitWidth();
    unsigned Shift = 8 * Store->ByteOffset;
    if (DL.isBigEndian())
      Shift = K->getBitWidth() - Width - Shift;
    return llvm::ConstantInt::get(V->getContext(),
                                  K->getValue().lshr(Shift).trunc(Width));
  }
  auto *I = llvm::dyn_cast<llvm::Instruction>(V);
  if (!I || !llvm::isa<llvm::BinaryOperator, llvm::ICmpInst, llvm::CastInst>(I))
    return nullptr;
  llvm::SmallVector<llvm::Constant *, 4> Operands;
  for (auto &Op : I->operands()) {
    auto *K = Next(Op.get());
    if (!K)
      return nullptr;
    Operands.push_back(K);
  }
  return llvm::dyn_cast_or_null<llvm::ConstantInt>(
      llvm::ConstantFoldInstOperands(I, Operands, DL, nullptr,
                                     /*AllowNonDeterministic=*/false));
}
bool pointerCandidateMatches(llvm::LoadInst &Origin, llvm::Value *Bank,
                             const StateSourceImage &Image, unsigned Offset,
                             const llvm::DataLayout &DL) {
  unsigned Budget = ::neverd::limits::pipeline::
      kStateSourceRecoveryPointerCandidateMatchesBudget;
  for (auto &I : llvm::instructions(Origin.getFunction())) {
    if (!Budget--)
      return false;
    auto *Base = llvm::dyn_cast<llvm::LoadInst>(&I);
    if (!Base || pointerLoad(Base) != Base ||
        displacement(Base->getPointerOperand(), Bank, DL) != Image.Base.Offset)
      continue;
    // A past bank base is advisory too: an opaque call may have replaced it.
    // No extra guest load is generated, and only target equality permits the
    // native call. Candidate bytes and sampled addresses are never consulted.
    if (displacement(Origin.getPointerOperand(), Base, DL) == Offset)
      return true;
  }
  return false;
}
llvm::Value *slot(llvm::IRBuilder<> &B, llvm::Value *Bank, unsigned Offset) {
  return B.CreateConstGEP1_64(B.getInt8Ty(), Bank, Offset, "state_slot");
}
llvm::Value *read(llvm::IRBuilder<> &B, llvm::Value *Bank, StateSourceSlot S,
                  llvm::MDNode *Scope) {
  auto *V = B.CreateAlignedLoad(B.getIntNTy(S.Bytes * 8),
                                slot(B, Bank, S.Offset), llvm::Align(1));
  V->setMetadata(llvm::LLVMContext::MD_alias_scope, Scope);
  return B.CreateZExtOrTrunc(V, B.getInt64Ty());
}
void write(llvm::IRBuilder<> &B, llvm::Value *Bank, StateSourceSlot S,
           llvm::Value *V, llvm::MDNode *Scope) {
  V = B.CreateZExtOrTrunc(V, B.getIntNTy(S.Bytes * 8));
  B.CreateAlignedStore(V, slot(B, Bank, S.Offset), llvm::Align(1))
      ->setMetadata(llvm::LLVMContext::MD_alias_scope, Scope);
}
llvm::MDNode *bankScope(llvm::Function &F) {
  llvm::MDBuilder B(F.getContext());
  auto *Domain = B.createAnonymousAliasScopeDomain("source register bank");
  return llvm::MDNode::get(F.getContext(), B.createAnonymousAliasScope(Domain));
}

// A wide load may preserve unknown upper bytes for another state store, while
// an import selector uses only its low word. GVN cannot discard the wide use.
// Expose a separate narrow load at the ORIGINAL read, never at its later use:
// the ordinary memory optimizer can then forward an exact partial writer.
bool exposeBankLoadSlices(llvm::Module &M) {
  struct Slice {
    llvm::Instruction *Use;
    llvm::LoadInst *Load;
    unsigned Bits;
  };
  std::vector<Slice> Slices;
  for (auto &F : M) {
    if (F.isDeclaration() || F.arg_empty() ||
        !F.getArg(0)->getType()->isPointerTy())
      continue;
    for (auto &I : llvm::instructions(F)) {
      llvm::LoadInst *L = nullptr;
      unsigned Bits = 0;
      if (auto *T = llvm::dyn_cast<llvm::TruncInst>(&I)) {
        L = llvm::dyn_cast<llvm::LoadInst>(T->getOperand(0));
        if (T->getType()->isIntegerTy())
          Bits = T->getType()->getIntegerBitWidth();
      } else if (I.getOpcode() == llvm::Instruction::And) {
        for (unsigned Side : {0u, 1u}) {
          auto *Mask =
              llvm::dyn_cast<llvm::ConstantInt>(I.getOperand(1 - Side));
          if (Mask && Mask->getValue().isMask()) {
            L = llvm::dyn_cast<llvm::LoadInst>(I.getOperand(Side));
            Bits = Mask->getValue().getActiveBits();
            break;
          }
        }
      }
      if (!L || L->isVolatile() || L->isAtomic() ||
          !L->getType()->isIntegerTy() ||
          L->getType()->getIntegerBitWidth() > 64 || !Bits || Bits % 8 ||
          Bits >= L->getType()->getIntegerBitWidth() ||
          !displacement(L->getPointerOperand(), F.getArg(0), M.getDataLayout()))
        continue;
      if (Slices.size() < ::neverd::limits::pipeline::
                              kStateSourceRecoveryExposeBankLoadSlicesSizeLimit)
        Slices.push_back({&I, L, Bits});
    }
  }
  for (auto [Use, L, Bits] : Slices) {
    llvm::IRBuilder<> B(L);
    auto *Pointer = L->getPointerOperand();
    if (M.getDataLayout().isBigEndian())
      Pointer =
          slot(B, Pointer, (L->getType()->getIntegerBitWidth() - Bits) / 8);
    auto *Narrow = B.CreateAlignedLoad(B.getIntNTy(Bits), Pointer,
                                       llvm::Align(1), "state_value");
    Narrow->setMetadata(llvm::LLVMContext::MD_alias_scope,
                        L->getMetadata(llvm::LLVMContext::MD_alias_scope));
    Narrow->setMetadata(llvm::LLVMContext::MD_noalias,
                        L->getMetadata(llvm::LLVMContext::MD_noalias));
    Use->replaceAllUsesWith(B.CreateZExtOrTrunc(Narrow, Use->getType()));
    Use->eraseFromParent();
  }
  return !Slices.empty();
}

// A private, writable bank may be cached in an alloca, exposing overlapping
// byte/word accesses to the ordinary SROA/mem2reg owner. Publish before every
// call (including a possibly non-returning native import) and return. Calls
// receiving the real bank can change any byte, so reload after those calls.
// Keep function boundaries and the complete original state ABI. Never replace
// the bank operand of an opaque call with the cache's address.
bool localizeBank(llvm::Function &F, unsigned Bytes) {
  if (F.isDeclaration() || F.hasPersonalityFn() || F.hasLocalLinkage() ||
      F.hasComdat() || F.hasAvailableExternallyLinkage() ||
      Bytes > ::neverd::limits::pipeline::
                  kStateSourceRecoveryLocalizeBankBytesLimit ||
      F.getInstructionCount() >
          ::neverd::limits::pipeline::
              kStateSourceRecoveryLocalizeBankFGetInstructionCountLimit)
    return false;
  if (auto Error = validateBank(F, Bytes)) {
    llvm::consumeError(std::move(Error));
    return false;
  }
  for (const auto &BB : F)
    if (BB.hasAddressTaken())
      return false;
  auto *Bank = F.getArg(0);
  const auto &DL = F.getParent()->getDataLayout();
  struct Access {
    llvm::Instruction *Inst;
    unsigned Offset;
    unsigned Width;
  };
  std::vector<Access> Accesses;
  llvm::BitVector Readable(Bytes);
  std::map<const llvm::Instruction *, llvm::BitVector> Writes;
  llvm::MDNode *Scope = nullptr;
  std::vector<std::pair<llvm::CallInst *, bool>> Calls;
  std::vector<llvm::ReturnInst *> Returns;
  for (auto &I : llvm::instructions(F)) {
    if (auto *R = llvm::dyn_cast<llvm::ReturnInst>(&I))
      Returns.push_back(R);
    if (llvm::isa<llvm::InvokeInst, llvm::CallBrInst>(&I))
      return false;
    if (auto *C = llvm::dyn_cast<llvm::CallInst>(&I)) {
      if (C->canReturnTwice() || C->isMustTailCall() || C->isInlineAsm() ||
          C->isConvergent() || C->hasOperandBundles())
        return false;
      bool ReceivesBank =
          !C->arg_empty() && displacement(C->getArgOperand(0), Bank, DL) == 0;
      Calls.emplace_back(C, ReceivesBank);
    }
    llvm::Value *Pointer = nullptr;
    if (auto *L = llvm::dyn_cast<llvm::LoadInst>(&I))
      Pointer = L->getPointerOperand();
    if (auto *S = llvm::dyn_cast<llvm::StoreInst>(&I))
      Pointer = S->getPointerOperand();
    if (!Pointer)
      continue;
    auto Offsets = bankOffsets(Pointer, Bank, DL);
    if (!Offsets || Offsets->Bank.empty())
      continue;
    // Selected bank/guest addresses and multiple possible offsets retain the
    // original implementation. Complete preflight precedes all mutations.
    if (Offsets->Guest || Offsets->Bank.size() != 1)
      return false;
    const unsigned Offset = unsigned(Offsets->Bank.front());
    auto *Stored = llvm::dyn_cast<llvm::StoreInst>(&I);
    auto *T = Stored ? Stored->getValueOperand()->getType() : I.getType();
    const unsigned Width = DL.getTypeStoreSize(T).getFixedValue();
    Readable.set(Offset, Offset + Width);
    if (Stored) {
      llvm::BitVector Mask(Bytes);
      Mask.set(Offset, Offset + Width);
      Writes.emplace(&I, std::move(Mask));
    }
    if (auto *MD = I.getMetadata(llvm::LLVMContext::MD_alias_scope))
      Scope = MD;
    Accesses.push_back({&I, Offset, Width});
  }
  if (Accesses.empty() ||
      Calls.size() > ::neverd::limits::pipeline::
                         kStateSourceRecoveryLocalizeBankCallsSizeLimit ||
      uint64_t(Bytes) * (2 * Calls.size() + Returns.size() + 1) >
          ::neverd::limits::pipeline::
              kStateSourceRecoveryLocalizeBankUint64TBytes2CallsSizeReturnsSize1Limit)
    return false;
  // Publish only bytes possibly written since the last synchronization.
  // A whole-function write set would re-publish stale/unmodified carriers at
  // every call and multiply source size. Solve the original CFG first; a
  // refused construction budget leaves the complete function unchanged.
  std::map<const llvm::BasicBlock *, llvm::BitVector> DirtyOut;
  std::map<const llvm::Instruction *, llvm::BitVector> Publish;
  for (const auto &BB : F)
    DirtyOut.emplace(&BB, llvm::BitVector(Bytes));
  unsigned Work = 0;
  bool Changed;
  do {
    Changed = false;
    for (auto &BB : F) {
      if (++Work >
          ::neverd::limits::pipeline::kStateSourceRecoveryLocalizeBankWorkLimit)
        return false;
      llvm::BitVector Dirty(Bytes);
      for (const auto *Pred : llvm::predecessors(&BB))
        Dirty |= DirtyOut.at(Pred);
      for (auto &I : BB) {
        if (auto W = Writes.find(&I); W != Writes.end())
          Dirty |= W->second;
        if (llvm::isa<llvm::CallInst, llvm::ReturnInst>(I)) {
          Publish[&I] = Dirty;
          Dirty.reset();
        }
      }
      if (Dirty != DirtyOut.at(&BB)) {
        DirtyOut[&BB] = std::move(Dirty);
        Changed = true;
      }
    }
  } while (Changed);
  llvm::IRBuilder<> Entry(&*F.getEntryBlock().getFirstInsertionPt());
  auto *Cache = Entry.CreateAlloca(
      llvm::ArrayType::get(Entry.getInt8Ty(), Bytes), nullptr, "local_bank");
  auto Copy = [&](llvm::IRBuilder<> &B, bool Reload,
                  const llvm::BitVector &Mask) {
    for (unsigned Offset = 0; Offset < Bytes;) {
      if (!Mask[Offset]) {
        ++Offset;
        continue;
      }
      unsigned Width = 1;
      while (Width < 8 && Offset + Width * 2 <= Bytes &&
             Mask.find_first_unset_in(Offset + Width, Offset + Width * 2) == -1)
        Width *= 2;
      auto *Real = slot(B, Bank, Offset);
      auto *Local = slot(B, Cache, Offset);
      auto *Value =
          B.CreateAlignedLoad(B.getIntNTy(Width * 8), Reload ? Real : Local,
                              llvm::Align(1), "bank_value");
      auto *Store =
          B.CreateAlignedStore(Value, Reload ? Local : Real, llvm::Align(1));
      if (Scope)
        (Reload ? static_cast<llvm::Instruction *>(Value) : Store)
            ->setMetadata(llvm::LLVMContext::MD_alias_scope, Scope);
      Offset += Width;
    }
  };
  Copy(Entry, true, Readable);
  for (const auto &[I, Offset, Width] : Accesses) {
    llvm::IRBuilder<> B(I);
    auto *Address = slot(B, Cache, Offset);
    I->setOperand(llvm::isa<llvm::LoadInst>(I) ? 0 : 1, Address);
  }
  for (auto [Call, ReceivesBank] : Calls) {
    llvm::IRBuilder<> Before(Call);
    Copy(Before, false, Publish.at(Call));
    if (ReceivesBank) {
      llvm::IRBuilder<> After(Call->getNextNode());
      Copy(After, true, Readable);
    }
    // Synchronization follows ordinary tail calls; they are no longer tail.
    Call->setTailCallKind(llvm::CallInst::TCK_None);
  }
  for (auto *Return : Returns) {
    llvm::IRBuilder<> B(Return);
    Copy(B, false, Publish.at(Return));
  }
  return true;
}

// Evaluate through the shared optimizer before committing an ABI-local view.
// Partial-width banks can require more scalar merges than their memory form;
// never publish a more expensive body merely because promotion succeeded.
llvm::Error localizeProfitableBanks(llvm::Module &M, StateSourceRecovery &R) {
  std::vector<llvm::Function *> Functions;
  for (auto &F : M)
    if (!F.isDeclaration())
      Functions.push_back(&F);
  std::vector<std::pair<std::string, std::string>> Trials;
  for (auto *F : Functions) {
    llvm::ValueToValueMapTy Map;
    auto *Backup = llvm::CloneFunction(F, Map);
    if (localizeBank(*F, R.Contract.StateBytes))
      Trials.emplace_back(F->getName().str(), Backup->getName().str());
    else
      Backup->eraseFromParent();
  }
  if (Trials.empty())
    return llvm::Error::success();
  Pipeline::OptimizationOptions Options;
  Options.Strength = Pipeline::OptStrength::Deep;
  Options.LLVMLevel = llvm::OptimizationLevel::O2;
  auto Optimized = Pipeline::optimizeModule(M, Options);
  if (Optimized.Stop == OptimizationStopReason::InputInvalid ||
      Optimized.Stop == OptimizationStopReason::VerificationFailed)
    return invalid("local bank optimization failed: " +
                   optimizationStopMessage(Optimized));
  auto Cost = [](const llvm::Function &F) {
    unsigned Value = 0;
    for (const auto &I : llvm::instructions(F)) {
      ++Value;
      if (llvm::isa<llvm::LoadInst, llvm::StoreInst>(&I))
        Value += 4;
      if (const auto *P = llvm::dyn_cast<llvm::PHINode>(&I))
        Value += P->getNumIncomingValues();
      if (I.isTerminator())
        Value += 2 * I.getNumSuccessors();
    }
    return Value;
  };
  // The shared optimizer commits a new module body transactionally. Resolve
  // identities after it returns; pointers into the input body are invalid.
  for (const auto &[Name, BackupName] : Trials) {
    auto *F = M.getFunction(Name);
    auto *Backup = M.getFunction(BackupName);
    if (!F || !Backup)
      return invalid("local bank trial lost a function boundary");
    if (Cost(*F) < Cost(*Backup)) {
      ++R.LocalizedFunctions;
    } else {
      F->deleteBody();
      F->copyAttributesFrom(Backup);
      llvm::ValueToValueMapTy Map;
      for (auto &Arg : Backup->args())
        Map[&Arg] = F->getArg(Arg.getArgNo());
      llvm::SmallVector<llvm::ReturnInst *, 8> Returns;
      llvm::CloneFunctionInto(F, Backup, Map,
                              llvm::CloneFunctionChangeType::LocalChangesOnly,
                              Returns);
    }
    Backup->eraseFromParent();
  }
  return llvm::Error::success();
}

} // namespace

llvm::Expected<StateSourceRecovery>
prepareStateSourceRecovery(llvm::Module &M, const StateSourceContract &C) {
  if (!C.IsolatedBank || (!C.Gateways.empty() && !C.ZeroStatusGateways) ||
      !C.StateBytes ||
      C.StateBytes >
          ::neverd::limits::pipeline::
              kStateSourceRecoveryPrepareStateSourceRecoveryCStateBytesLimit ||
      C.Gateways.size() >
          ::neverd::limits::pipeline::
              kStateSourceRecoveryPrepareStateSourceRecoveryCGatewaysSizeLimit)
    return invalid(
        "requires a bounded isolated bank and explicit zero-status gateways");
  if (llvm::verifyModule(M, &llvm::errs()))
    return invalid("invalid input IR");
  const auto &DL = M.getDataLayout();
  if (DL.getPointerSizeInBits(0) != 64 || DL.getIndexSizeInBits(0) != 64 ||
      DL.isNonIntegralAddressSpace(0))
    return invalid("requires integral 64-bit pointers and indices");
  auto ValidSlot = [&](StateSourceSlot S) {
    return (S.Bytes == 1 || S.Bytes == 2 || S.Bytes == 4 || S.Bytes == 8) &&
           S.Offset <= C.StateBytes && S.Bytes <= C.StateBytes - S.Offset;
  };
  if (C.Images.size() > 16)
    return invalid("too many image candidates");
  for (const auto &Image : C.Images)
    if (!Image.Readable || !ValidSlot(Image.Base) || Image.Base.Bytes != 8 ||
        Image.Bytes.empty() ||
        Image.Bytes.size() >
            ::neverd::limits::pipeline::
                kStateSourceRecoveryPrepareStateSourceRecoveryImageBytesSizeLimit)
      return invalid("invalid readable image candidate");
  std::map<std::string, const StateSourceImport *> Names;
  std::map<std::string, size_t> Gateways;
  for (size_t Index = 0; Index < C.Gateways.size(); ++Index) {
    const auto &G = C.Gateways[Index];
    if (G.FallbackWrites && G.FallbackWrites->ResultBytes &&
        *G.FallbackWrites->ResultBytes != 1 &&
        *G.FallbackWrites->ResultBytes != 2 &&
        *G.FallbackWrites->ResultBytes != 4 &&
        *G.FallbackWrites->ResultBytes != 8)
      return invalid("invalid fixed fallback result width");
    if (!Gateways.emplace(G.Name, Index).second || !identifier(G.Name) ||
        G.Arguments.size() > 16 ||
        G.Imports.size() >
            ::neverd::limits::pipeline::
                kStateSourceRecoveryPrepareStateSourceRecoveryGImportsSizeLimit ||
        !ValidSlot(G.ResultOffset) || !ValidSlot(G.ResultBytes) ||
        (G.Selector && !ValidSlot(*G.Selector)) ||
        (G.FunctionPointer && (G.Selector || !ValidSlot(*G.FunctionPointer) ||
                               G.FunctionPointer->Bytes != 8)) ||
        (G.ArgumentCount && !ValidSlot(*G.ArgumentCount)))
      return invalid("invalid gateway layout");
    for (auto A : G.Arguments)
      if (!ValidSlot(A))
        return invalid("argument outside bank");
    std::set<unsigned> Indirect, Strings;
    for (unsigned A : G.IndirectArguments)
      if (A >= G.Arguments.size() || !Indirect.insert(A).second)
        return invalid("invalid indirect argument");
    if (G.Strings.size() > 16 ||
        G.ExcludedArguments.size() >
            ::neverd::limits::pipeline::
                kStateSourceRecoveryPrepareStateSourceRecoveryGExcludedArgumentsSizeLimit)
      return invalid("too many string arguments or exclusions");
    for (const auto &S : G.Strings)
      if (!S.ByValue || S.Argument >= G.Arguments.size() ||
          !Strings.insert(S.Argument).second || S.Image >= C.Images.size() ||
          bool(S.Mode) != bool(S.XorKey) ||
          (S.Mode && (!ValidSlot(*S.Mode) || !ValidSlot(*S.XorKey))))
        return invalid("invalid string normalization contract");
    for (auto E : G.ExcludedArguments)
      if (E.Argument >= G.Arguments.size())
        return invalid("invalid argument exclusion");
    if (G.Writes.size() >
        ::neverd::limits::pipeline::
            kStateSourceRecoveryPrepareStateSourceRecoveryGWritesSizeLimit)
      return invalid("too many fixed writes");
    for (auto W : G.Writes)
      if (!ValidSlot(W.Slot))
        return invalid("write outside bank");
    auto ValidType = [](StateSourceType T) {
      return unsigned(T) <= unsigned(StateSourceType::I16);
    };
    std::set<uint64_t> Selectors;
    std::set<std::pair<unsigned, unsigned>> PointerSources;
    for (const auto &I : G.Imports) {
      if (I.PassContext) {
        auto *Gateway = M.getFunction(G.Name);
        if (Gateway &&
            (Gateway->isVarArg() || Gateway->arg_size() != 2 ||
             !Gateway->getArg(0)->getType()->isPointerTy() ||
             Gateway->getArg(0)->getType()->getPointerAddressSpace() != 0 ||
             !Gateway->getArg(1)->getType()->isPointerTy() ||
             Gateway->getArg(1)->getType()->getPointerAddressSpace() != 0))
          return invalid(
              "context forwarding requires a two-argument pointer gateway");
      }
      if ((I.TargetArgument ? !I.Name.empty() : !identifier(I.Name)) ||
          Gateways.count(I.Name) ||
          (!G.FunctionPointer && (!Selectors.insert(I.Selector).second ||
                                  (!G.Selector && I.Selector))) ||
          bool(G.FunctionPointer) != bool(I.PointerSource) ||
          (G.FunctionPointer && I.Selector) ||
          (I.VariadicFixedParameters &&
           (!*I.VariadicFixedParameters ||
            *I.VariadicFixedParameters > I.Parameters.size() ||
            !I.Defaults.empty())) ||
          I.Defaults.size() > I.Parameters.size() ||
          (I.VoidResult != StateSourceVoidResult::Zero &&
           (I.VoidResult != StateSourceVoidResult::Unspecified ||
            I.Result != StateSourceType::Void)) ||
          I.Parameters.size() > 16 || !ValidType(I.Result) ||
          !llvm::all_of(I.Parameters, ValidType) ||
          llvm::is_contained(I.Parameters, StateSourceType::Void))
        return invalid("invalid import identity or parameters");
      unsigned Required = I.Parameters.size() - I.Defaults.size();
      if (Required > G.Arguments.size())
        return invalid("required argument has no bank slot");
      if (I.TargetArgument &&
          (G.FunctionPointer || I.PointerSource || I.VariadicFixedParameters ||
           *I.TargetArgument >= Required ||
           I.Parameters[*I.TargetArgument] != StateSourceType::Pointer ||
           G.Arguments[*I.TargetArgument].Bytes != 8))
        return invalid("invalid forwarded call target");
      for (unsigned P = 0; P < I.Parameters.size(); ++P) {
        auto *T = type(M.getContext(), I.Parameters[P]);
        if (I.VariadicFixedParameters && P >= *I.VariadicFixedParameters &&
            T->isIntegerTy() && T->getIntegerBitWidth() < 32)
          return invalid("variadic operands must use promoted integer types");
        if (P < Required)
          continue;
        uint64_t Value = I.Defaults[P - Required];
        if ((T->isPointerTy() && Value) ||
            (T->isIntegerTy() && T->getIntegerBitWidth() < 64 &&
             Value >> T->getIntegerBitWidth()))
          return invalid("default argument does not fit its source type");
      }
      if (I.PointerSource) {
        const auto &P = *I.PointerSource;
        if (P.Image >= C.Images.size() ||
            P.Offset > C.Images[P.Image].Bytes.size() ||
            8 > C.Images[P.Image].Bytes.size() - P.Offset ||
            !PointerSources.emplace(P.Image, P.Offset).second)
          return invalid("invalid or duplicate imported pointer source");
      }
      for (const auto &S : G.Strings)
        if (S.Argument >= I.Parameters.size() || S.Argument >= Required ||
            I.TargetArgument == S.Argument ||
            I.Parameters[S.Argument] != StateSourceType::Pointer)
          return invalid("string requires an imported pointer parameter");
      if (I.TargetArgument)
        continue;
      auto *T = signature(M.getContext(), I);
      auto [At, Added] = Names.emplace(I.Name, &I);
      if ((!Added && (signature(M.getContext(), *At->second) != T ||
                      At->second->Result != I.Result ||
                      nativeParameters(I, true) !=
                          nativeParameters(*At->second, true))) ||
          (M.getNamedValue(I.Name) &&
           (!M.getFunction(I.Name) ||
            M.getFunction(I.Name)->getFunctionType() != T ||
            !M.getFunction(I.Name)->isDeclaration())))
        return invalid("conflicting import declaration");
    }
  }
  for (auto &[Name, Index] : Gateways)
    if (Names.count(Name))
      return invalid("import is also a gateway");
  for (auto &F : M) {
    if (F.getName().starts_with(ProbePrefix))
      return invalid("reserved probe name is present");
    if (F.isDeclaration())
      continue;
    if (F.isVarArg() || F.arg_empty() ||
        !F.getArg(0)->getType()->isPointerTy() ||
        F.getArg(0)->getType()->getPointerAddressSpace() != 0 ||
        !F.getReturnType()->isIntegerTy(64))
      return invalid("incompatible state function ABI");
    for (auto &I : llvm::instructions(F))
      if (llvm::isa<llvm::InvokeInst, llvm::CallBrInst>(I))
        return invalid("exceptional state call is unsupported");
    if (auto Error = validateBank(F, C.StateBytes))
      return Error;
  }
  StateSourceRecovery R;
  R.Contract = C;
  R.Probes.resize(C.Gateways.size());
  std::vector<llvm::Function *> Functions;
  for (auto &F : M)
    if (!F.isDeclaration())
      Functions.push_back(&F);
  for (auto *F : Functions) {
    ++R.Functions;
    // Source reconstruction keeps the input's function boundaries. Module
    // inlining obscures the bank interface and can multiply source size.
    F->removeFnAttr(llvm::Attribute::AlwaysInline);
    F->addFnAttr(llvm::Attribute::NoInline);
    auto *Original = F->getArg(0);
    auto *Bank = Original;
    auto *Scope = bankScope(*F);
    std::vector<llvm::CallInst *> Calls;
    for (auto &I : llvm::instructions(F)) {
      if (auto *Call = llvm::dyn_cast<llvm::CallInst>(&I)) {
        if (!Call->arg_empty() &&
            displacement(Call->getArgOperand(0), Bank, M.getDataLayout()) == 0)
          Calls.push_back(Call);
        else
          Call->setMetadata(llvm::LLVMContext::MD_noalias, Scope);
      }
      llvm::Value *P = nullptr;
      llvm::Type *T = nullptr;
      if (auto *L = llvm::dyn_cast<llvm::LoadInst>(&I)) {
        P = L->getPointerOperand();
        T = L->getType();
      }
      if (auto *S = llvm::dyn_cast<llvm::StoreInst>(&I)) {
        P = S->getPointerOperand();
        T = S->getValueOperand()->getType();
      }
      if (P) {
        auto Addresses = bankOffsets(P, Bank, M.getDataLayout());
        if (!Addresses || Addresses->Bank.empty())
          I.setMetadata(llvm::LLVMContext::MD_noalias, Scope);
        else if (!Addresses->Guest)
          I.setMetadata(llvm::LLVMContext::MD_alias_scope, Scope);
      }
      auto Offset = P ? displacement(P, Bank, M.getDataLayout()) : std::nullopt;
      if (!Offset || *Offset < 0 || uint64_t(*Offset) > C.StateBytes ||
          M.getDataLayout().getTypeStoreSize(T).isScalable() ||
          M.getDataLayout().getTypeStoreSize(T).getFixedValue() >
              C.StateBytes - *Offset)
        continue;
      llvm::IRBuilder<> B(&I);
      auto *New = slot(B, Bank, *Offset);
      if (auto *L = llvm::dyn_cast<llvm::LoadInst>(&I))
        L->setOperand(0, New);
      else
        llvm::cast<llvm::StoreInst>(I).setOperand(1, New);
      I.setMetadata(llvm::LLVMContext::MD_alias_scope, Scope);
    }
    for (auto *Call : Calls) {
      llvm::IRBuilder<> B(Call);
      auto *Callee = Call->getCalledFunction();
      auto At =
          Callee ? Gateways.find(Callee->getName().str()) : Gateways.end();
      std::vector<llvm::Value *> Args;
      for (auto &A : Call->args())
        Args.push_back(A.get());
      Args[0] = Original;
      if (At != Gateways.end()) {
        const auto &G = C.Gateways[At->second];
        for (auto S : {G.FunctionPointer ? G.FunctionPointer : G.Selector,
                       G.ArgumentCount, std::optional(G.ResultOffset),
                       std::optional(G.ResultBytes)})
          Args.push_back(S ? read(B, Bank, *S, Scope) : B.getInt64(0));
        if (!G.ArgumentCount)
          Args[Call->arg_size() + 1] = B.getInt64(G.Arguments.size());
        for (auto S : G.Arguments)
          Args.push_back(read(B, Bank, S, Scope));
        for (const auto &S : G.Strings) {
          Args.push_back(read(B, Bank, C.Images[S.Image].Base, Scope));
          Args.push_back(S.Mode ? read(B, Bank, *S.Mode, Scope)
                                : B.getInt64(0));
          Args.push_back(S.XorKey ? read(B, Bank, *S.XorKey, Scope)
                                  : B.getInt64(0));
        }
        std::vector<llvm::Type *> Types;
        for (auto *V : Args)
          Types.push_back(V->getType());
        auto *FT = llvm::FunctionType::get(Call->getType(), Types, false);
        auto &Name = R.Probes[At->second];
        if (Name.empty())
          Name =
              llvm::Function::Create(FT, llvm::GlobalValue::ExternalLinkage,
                                     ProbePrefix + llvm::Twine(At->second), M)
                  ->getName()
                  .str();
        if (M.getFunction(Name)->getFunctionType() != FT)
          return invalid("inconsistent gateway ABI");
        auto *Replacement =
            B.CreateCall(M.getFunction(Name), Args, "state_call");
        Replacement->setMetadata("neverd.state.scope", Scope);
        Replacement->setMetadata(
            "neverd.state.original_argc",
            llvm::MDNode::get(
                M.getContext(),
                llvm::ConstantAsMetadata::get(B.getInt32(Call->arg_size()))));
        Call->replaceAllUsesWith(Replacement);
        Call->eraseFromParent();
        Call = Replacement;
      } else
        Call->setArgOperand(0, Original);
    }
  }
  return R;
}

llvm::Error finishStateSourceRecovery(llvm::Module &M, StateSourceRecovery &R) {
  state_source::CallWrites GatewayWrites;
  // Resolve indirect bank slots only after ordinary propagation has proved
  // their complete offsets. No unchecked pointer arithmetic or speculative
  // bank load is introduced for an unresolved layout.
  bool ResolvedSources = false;
  for (size_t Index = 0; Index < R.Probes.size(); ++Index) {
    auto *Probe = M.getFunction(R.Probes[Index]);
    if (!Probe)
      continue;
    const auto &G = R.Contract.Gateways[Index];
    for (auto *U : Probe->users()) {
      auto *Call = llvm::dyn_cast<llvm::CallInst>(U);
      if (!Call || Call->getCalledFunction() != Probe)
        return invalid("probe escaped");
      auto *MD = Call->getMetadata("neverd.state.original_argc");
      auto *Count =
          MD && MD->getNumOperands() == 1
              ? llvm::mdconst::dyn_extract<llvm::ConstantInt>(MD->getOperand(0))
              : nullptr;
      if (!Count)
        return invalid("lost call ABI metadata");
      unsigned N = Count->getZExtValue();
      unsigned Extra = N + 4 + G.Arguments.size();
      if (Call->arg_size() != Extra + 3 * G.Strings.size())
        return invalid("changed probe arity");
      std::vector<std::pair<unsigned, unsigned>> Offsets;
      for (unsigned A : G.IndirectArguments) {
        auto *Offset =
            llvm::dyn_cast<llvm::ConstantInt>(Call->getArgOperand(N + 4 + A));
        if (!Offset || Offset->getZExtValue() > R.Contract.StateBytes ||
            8 > R.Contract.StateBytes - Offset->getZExtValue())
          break;
        Offsets.emplace_back(A, unsigned(Offset->getZExtValue()));
      }
      if (Offsets.size() != G.IndirectArguments.size())
        continue;
      llvm::IRBuilder<> B(Call);
      for (auto [A, Offset] : Offsets)
        Call->setArgOperand(N + 4 + A,
                            read(B, Call->getArgOperand(0), {Offset, 8},
                                 Call->getMetadata("neverd.state.scope")));
      for (unsigned S = 0; S < G.Strings.size(); ++S)
        Call->setArgOperand(
            Extra + 3 * S,
            B.CreateSub(Call->getArgOperand(N + 4 + G.Strings[S].Argument),
                        Call->getArgOperand(Extra + 3 * S), "string_offset"));
      Call->setMetadata("neverd.state.sources_ready",
                        llvm::MDNode::get(M.getContext(), {}));
      ResolvedSources |= !Offsets.empty() || !G.Strings.empty();
    }
  }
  if (ResolvedSources) {
    Pipeline::OptimizationOptions Policy;
    Policy.Strength = Pipeline::OptStrength::Deep;
    Policy.LLVMLevel = llvm::OptimizationLevel::O2;
    auto Result = Pipeline::optimizeModule(M, Policy);
    if (Result.Stop == OptimizationStopReason::InputInvalid ||
        Result.Stop == OptimizationStopReason::VerificationFailed)
      return invalid("argument source optimization failed: " +
                     optimizationStopMessage(Result));
  }
  for (unsigned Round = 0;
       Round < ::neverd::limits::pipeline::
                   kStateSourceRecoveryFinishStateSourceRecoveryRoundLimit &&
       exposeBankLoadSlices(M);
       ++Round) {
    Pipeline::OptimizationOptions Policy;
    Policy.Strength = Pipeline::OptStrength::Deep;
    Policy.LLVMLevel = llvm::OptimizationLevel::O2;
    auto Result = Pipeline::optimizeModule(M, Policy);
    if (Result.Stop == OptimizationStopReason::InputInvalid ||
        Result.Stop == OptimizationStopReason::VerificationFailed)
      return invalid("bank slice optimization failed: " +
                     optimizationStopMessage(Result));
  }
  for (size_t Index = 0; Index < R.Probes.size(); ++Index) {
    if (R.Probes[Index].empty())
      continue;
    auto *Probe = M.getFunction(R.Probes[Index]);
    if (!Probe)
      continue; // The optimizer may delete every unreachable use and
                // declaration.
    const auto &G = R.Contract.Gateways[Index];
    std::vector<llvm::CallInst *> Calls;
    for (auto *U : Probe->users()) {
      auto *Call = llvm::dyn_cast<llvm::CallInst>(U);
      if (!Call || Call->getCalledFunction() != Probe)
        return invalid("probe escaped");
      Calls.push_back(Call);
    }
    for (auto *Call : Calls) {
      auto *MD = Call->getMetadata("neverd.state.original_argc");
      auto *Count =
          MD ? llvm::mdconst::dyn_extract<llvm::ConstantInt>(MD->getOperand(0))
             : nullptr;
      if (!Count)
        return invalid("lost call ABI metadata");
      unsigned N = Count->getZExtValue();
      if (Call->arg_size() != N + 4 + G.Arguments.size() + 3 * G.Strings.size())
        return invalid("changed probe arity");
      auto Constant = [&](unsigned Arg) -> std::optional<uint64_t> {
        if (auto *V =
                llvm::dyn_cast<llvm::ConstantInt>(Call->getArgOperand(N + Arg)))
          return V->getZExtValue();
        return {};
      };
      auto Selector = Constant(0), Arity = Constant(1), Offset = Constant(2),
           Bytes = Constant(3);
      auto CandidateSelector = Selector;
      llvm::LoadInst *PointerOrigin = nullptr;
      if (G.FunctionPointer) {
        llvm::DominatorTree DT(*Call->getFunction());
        PointerOrigin =
            pointerCandidateLoad(Call->getArgOperand(N), Call->getArgOperand(0),
                                 DT, M.getDataLayout());
      } else if (!Selector) {
        llvm::DominatorTree DT(*Call->getFunction());
        unsigned Budget = ::neverd::limits::pipeline::
            kStateSourceRecoveryFinishStateSourceRecoveryBudget;
        if (auto *K = selectorCandidate(Call->getArgOperand(N),
                                        Call->getArgOperand(0), DT,
                                        M.getDataLayout(), Budget))
          CandidateSelector = K->getZExtValue();
      }
      const StateSourceImport *Import = nullptr;
      if (Call->getMetadata("neverd.state.sources_ready") &&
          (G.FunctionPointer || CandidateSelector) && Offset && Bytes &&
          (*Bytes == 1 || *Bytes == 2 || *Bytes == 4 || *Bytes == 8) &&
          *Offset <= R.Contract.StateBytes &&
          *Bytes <= R.Contract.StateBytes - *Offset)
        for (const auto &I : G.Imports) {
          if (Arity &&
              (*Arity > std::min(I.Parameters.size(), G.Arguments.size()) ||
               *Arity < I.Parameters.size() - I.Defaults.size()))
            continue;
          if (G.FunctionPointer) {
            if (!PointerOrigin)
              continue;
            const auto &P = *I.PointerSource;
            if (!pointerCandidateMatches(*PointerOrigin, Call->getArgOperand(0),
                                         R.Contract.Images[P.Image], P.Offset,
                                         M.getDataLayout()))
              continue;
          } else if (I.Selector != *CandidateSelector)
            continue;
          Import = &I;
          break;
        }
      if (Import && Import->VoidResult == StateSourceVoidResult::Unspecified &&
          !resultWriteIsDead(*Call, {unsigned(*Offset), unsigned(*Bytes)},
                             R.Contract.StateBytes, M.getDataLayout()))
        Import = nullptr;
      llvm::IRBuilder<> B(Call);
      llvm::Value *ScalarMatches = B.getTrue();
      if (Import && !Arity) {
        auto *Count = Call->getArgOperand(N + 1);
        unsigned Total =
            std::min(Import->Parameters.size(), G.Arguments.size());
        if (Import->Defaults.empty())
          ScalarMatches = B.CreateICmpEQ(Count, B.getInt64(Total),
                                         "argument_count_matches");
        else
          ScalarMatches = B.CreateAnd(
              B.CreateICmpUGE(Count, B.getInt64(Import->Parameters.size() -
                                                Import->Defaults.size())),
              B.CreateICmpULE(Count, B.getInt64(Total)),
              "argument_count_matches");
      }
      if (Import && !G.FunctionPointer && !Selector)
        ScalarMatches =
            B.CreateAnd(ScalarMatches,
                        B.CreateICmpEQ(Call->getArgOperand(N),
                                       B.getInt64(Import->Selector)),
                        "selector_matches");
      if (Import && G.FunctionPointer) {
        auto Target = M.getOrInsertFunction(Import->Name,
                                            signature(M.getContext(), *Import));
        auto *Pointer = Call->getArgOperand(N);
        ScalarMatches = B.CreateAnd(
            ScalarMatches,
            B.CreateAnd(
                B.CreateICmpNE(Pointer, B.getInt64(0)),
                B.CreateICmpEQ(Pointer, B.CreatePtrToInt(Target.getCallee(),
                                                         B.getInt64Ty()))),
            "function_pointer_matches");
      }
      if (Import && Import->TargetArgument)
        ScalarMatches = B.CreateAnd(
            ScalarMatches,
            B.CreateICmpNE(Call->getArgOperand(N + 4 + *Import->TargetArgument),
                           B.getInt64(0)),
            "call_target_nonnull");
      for (auto E : G.ExcludedArguments)
        ScalarMatches =
            B.CreateAnd(ScalarMatches,
                        B.CreateICmpNE(Call->getArgOperand(N + 4 + E.Argument),
                                       B.getInt64(E.Value)),
                        "argument_allowed");
      struct StringCandidate {
        unsigned Argument;
        std::string Encoded;
        std::string Decoded;
      };
      std::vector<StringCandidate> Candidates;
      if (Import)
        for (unsigned I = 0; I < G.Strings.size(); ++I) {
          const auto &S = G.Strings[I];
          unsigned Extra = 4 + G.Arguments.size() + 3 * I;
          auto Start = Constant(Extra), Mode = Constant(Extra + 1),
               Key = Constant(Extra + 2);
          const auto &Image = R.Contract.Images[S.Image];
          if (!Start || *Start >= Image.Bytes.size() || !Key) {
            Import = nullptr;
            break;
          }
          uint8_t Xor = 0;
          if (S.Mode && *Key && (!Mode || *Mode == S.EncodedMode)) {
            Xor = uint8_t(*Key);
            ScalarMatches =
                B.CreateAnd(ScalarMatches,
                            B.CreateICmpEQ(Call->getArgOperand(N + Extra + 1),
                                           B.getInt64(S.EncodedMode)),
                            "string_mode_matches");
          }
          StringCandidate Candidate{S.Argument, {}, {}};
          bool Terminated = false;
          for (
              size_t P = *Start;
              P < Image.Bytes.size() &&
              Candidate.Encoded.size() <
                  ::neverd::limits::pipeline::
                      kStateSourceRecoveryFinishStateSourceRecoveryCandidateEncodedSizeLimit;
              ++P) {
            uint8_t Byte = Image.Bytes[P];
            Candidate.Encoded.push_back(char(Byte));
            if (uint8_t(Byte ^ Xor) == 0) {
              Terminated = true;
              break;
            }
            Candidate.Decoded.push_back(char(Byte ^ Xor));
          }
          if (!Terminated) {
            Import = nullptr;
            break;
          }
          Candidates.push_back(std::move(Candidate));
        }
      if (auto *Known = llvm::dyn_cast<llvm::ConstantInt>(ScalarMatches);
          Known && Known->isZero())
        Import = nullptr;
      auto OriginalCall = [&](llvm::IRBuilder<> &B) {
        std::vector<llvm::Value *> Args;
        std::vector<llvm::Type *> Types;
        for (unsigned I = 0; I < N; ++I)
          Args.push_back(Call->getArgOperand(I));
        for (auto *A : Args)
          Types.push_back(A->getType());
        // O1 can prune the now-unused original declaration. The probe retains
        // its exact original parameter types, including an optional context.
        auto Original = M.getOrInsertFunction(
            G.Name, llvm::FunctionType::get(Call->getType(), Types, false));
        auto *Fallback = B.CreateCall(Original, Args, "unresolved_state_call");
        // This stronger fallback assertion is optional and independent of
        // successful import reconstruction. Default unknown paths still
        // clobber the entire bank, including nonzero-status returns.
        auto Width =
            G.FallbackWrites && G.FallbackWrites->ResultBytes
                ? std::optional<uint64_t>(*G.FallbackWrites->ResultBytes)
                : Bytes;
        if (G.FallbackWrites && Offset && Width &&
            (*Width == 1 || *Width == 2 || *Width == 4 || *Width == 8) &&
            *Offset <= R.Contract.StateBytes &&
            *Width <= R.Contract.StateBytes - *Offset) {
          llvm::BitVector Mask(R.Contract.StateBytes);
          Mask.set(*Offset, *Offset + *Width);
          for (auto W : G.Writes)
            Mask.set(W.Slot.Offset, W.Slot.Offset + W.Slot.Bytes);
          GatewayWrites.emplace(Fallback, std::move(Mask));
        }
        return Fallback;
      };
      auto NativeCall = [&](llvm::IRBuilder<> &B) {
        std::vector<llvm::Value *> Args;
        for (size_t I = 0; I < Import->Parameters.size(); ++I) {
          size_t Required = Import->Parameters.size() - Import->Defaults.size();
          llvm::Value *V = I < G.Arguments.size()
                               ? Call->getArgOperand(N + 4 + I)
                               : B.getInt64(Import->Defaults[I - Required]);
          if (I >= Required && I < G.Arguments.size())
            V = B.CreateSelect(
                B.CreateICmpUGT(Call->getArgOperand(N + 1), B.getInt64(I)), V,
                B.getInt64(Import->Defaults[I - Required]), "default_argument");
          auto *T = type(M.getContext(), Import->Parameters[I]);
          Args.push_back(T->isPointerTy() ? B.CreateIntToPtr(V, T)
                                          : B.CreateZExtOrTrunc(V, T));
        }
        for (const auto &S : Candidates)
          Args[S.Argument] = B.CreateGlobalString(S.Decoded, "source_string");
        auto *Scope = Call->getMetadata("neverd.state.scope");
        // Fixed bank writes occur before the imported operation, matching the
        // declared gateway contract. Guest effects cannot observe this bank.
        for (auto W : G.Writes)
          write(B, Call->getArgOperand(0), W.Slot, B.getInt64(W.Value), Scope);
        auto *FT = signature(M.getContext(), *Import);
        auto ParameterTypes = nativeParameters(*Import);
        llvm::Value *Callee;
        if (Import->TargetArgument) {
          Callee = Args[*Import->TargetArgument];
          Args.erase(Args.begin() + *Import->TargetArgument);
        } else {
          Callee = M.getOrInsertFunction(Import->Name, FT).getCallee();
          auto *Declaration = llvm::cast<llvm::Function>(Callee);
          if (signedType(Import->Result))
            Declaration->addFnAttr(LLVMSourceSignedResult);
          for (unsigned I = 0; I < Declaration->arg_size(); ++I)
            if (signedType(ParameterTypes[I]))
              Declaration->addParamAttr(
                  I, llvm::Attribute::get(M.getContext(),
                                          LLVMSourceSignedParameter));
        }
        if (Import->PassContext)
          Args.insert(Args.begin(), Call->getArgOperand(1));
        llvm::FunctionCallee Target(FT, Callee);
        auto *Native =
            B.CreateCall(Target, Args,
                         Import->Result == StateSourceType::Void ? ""
                         : Import->TargetArgument ? "indirect_result"
                                                  : Import->Name + "_result");
        if (signedType(Import->Result))
          Native->addFnAttr(
              llvm::Attribute::get(M.getContext(), LLVMSourceSignedResult));
        auto NativeIndex = [&](unsigned I) {
          return unsigned(Import->PassContext) + I -
                 unsigned(Import->TargetArgument &&
                          I > *Import->TargetArgument);
        };
        for (unsigned I = 0; I < Import->Parameters.size(); ++I) {
          if (Import->TargetArgument == I)
            continue;
          unsigned P = NativeIndex(I);
          if (signedType(Import->Parameters[I]))
            Native->addParamAttr(
                P, llvm::Attribute::get(M.getContext(),
                                        LLVMSourceSignedParameter));
          // Fixed pointer operands already carry a pointer expression; keep
          // by-value string literals readable. Integer casts also select the
          // declared overload when a platform header provides alternatives.
          if (P < FT->getNumParams() &&
              Import->Parameters[I] == StateSourceType::Pointer)
            continue;
          Native->addParamAttr(
              P, llvm::Attribute::get(
                     M.getContext(), LLVMSourceArgumentType,
                     sourceScalarSpelling(Import->Parameters[I])));
        }
        for (const auto &S : Candidates)
          Native->addParamAttr(
              NativeIndex(S.Argument),
              llvm::Attribute::get(M.getContext(),
                                   LLVMSourceByValueStringParameter));
        Native->setMetadata(llvm::LLVMContext::MD_noalias, Scope);
        llvm::Value *Result = B.getInt64(0);
        if (Native->getType()->isPointerTy())
          Result = B.CreatePtrToInt(Native, B.getInt64Ty());
        else if (!Native->getType()->isVoidTy())
          Result = B.CreateIntCast(Native, B.getInt64Ty(),
                                   signedType(Import->Result));
        if (Import->VoidResult != StateSourceVoidResult::Unspecified)
          write(B, Call->getArgOperand(0),
                {unsigned(*Offset), unsigned(*Bytes)}, Result, Scope);
      };
      if (!Import) {
        Call->replaceAllUsesWith(OriginalCall(B));
        ++R.UnresolvedCalls;
      } else if (llvm::isa<llvm::ConstantInt>(ScalarMatches) &&
                 Candidates.empty()) {
        NativeCall(B);
        Call->replaceAllUsesWith(B.getInt64(0));
        ++R.RewrittenCalls;
      } else {
        auto *Entry = Call->getParent();
        auto *F = Entry->getParent();
        auto *Tail =
            Entry->splitBasicBlock(Call->getIterator(), "call_continue");
        auto *Then =
            llvm::BasicBlock::Create(M.getContext(), "source_call", F, Tail);
        auto *Else =
            llvm::BasicBlock::Create(M.getContext(), "original_call", F, Tail);
        auto *Check = Candidates.empty()
                          ? Then
                          : llvm::BasicBlock::Create(M.getContext(),
                                                     "string_check", F, Then);
        Entry->getTerminator()->eraseFromParent();
        llvm::IRBuilder<> Branch(Entry);
        Branch.CreateCondBr(ScalarMatches, Check, Else);
        if (!Candidates.empty()) {
          llvm::IRBuilder<> CheckBuilder(Check);
          llvm::Value *Matches = CheckBuilder.getTrue();
          for (const auto &S : Candidates) {
            auto *Pointer = CheckBuilder.CreateIntToPtr(
                Call->getArgOperand(N + 4 + S.Argument),
                CheckBuilder.getPtrTy());
            // The contract supplies a readable extent. Compare every candidate
            // byte including its terminator; snapshots never establish current
            // memory contents. Unaligned integer loads are endian-aware.
            for (size_t P = 0; P < S.Encoded.size();) {
              unsigned Width = 8;
              while (Width > S.Encoded.size() - P)
                Width /= 2;
              uint64_t Expected = 0;
              for (unsigned J = 0; J < Width; ++J) {
                unsigned Shift =
                    M.getDataLayout().isLittleEndian() ? J : Width - 1 - J;
                Expected |= uint64_t(uint8_t(S.Encoded[P + J])) << (Shift * 8);
              }
              auto *T = CheckBuilder.getIntNTy(Width * 8);
              auto *Load = CheckBuilder.CreateAlignedLoad(
                  T, slot(CheckBuilder, Pointer, P), llvm::Align(1),
                  "string_bytes");
              Load->setMetadata(llvm::LLVMContext::MD_noalias,
                                Call->getMetadata("neverd.state.scope"));
              Matches = CheckBuilder.CreateAnd(
                  Matches,
                  CheckBuilder.CreateICmpEQ(
                      Load, llvm::ConstantInt::get(T, Expected)),
                  "string_matches");
              P += Width;
            }
          }
          CheckBuilder.CreateCondBr(Matches, Then, Else);
        }
        llvm::IRBuilder<> Known(Then), Unknown(Else);
        NativeCall(Known);
        Known.CreateBr(Tail);
        auto *Status = OriginalCall(Unknown);
        Unknown.CreateBr(Tail);
        auto *Merged = llvm::PHINode::Create(B.getInt64Ty(), 2, "call_status",
                                             Call->getIterator());
        Merged->addIncoming(B.getInt64(0), Then);
        Merged->addIncoming(Status, Else);
        Call->replaceAllUsesWith(Merged);
        ++R.RewrittenCalls;
        ++R.GuardedCalls;
      }
      Call->eraseFromParent();
    }
    Probe->eraseFromParent();
  }
  state_source::refineBankAccesses(M, R, GatewayWrites);
  if (R.Contract.LocalizeBank)
    if (auto E = localizeProfitableBanks(M, R))
      return E;
  if (llvm::verifyModule(M, &llvm::errs()))
    return invalid("source projection failed verification");
  return llvm::Error::success();
}
} // namespace neverd
