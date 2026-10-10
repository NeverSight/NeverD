//===- LLVMCExprWriter.cpp - LLVM IR value/expression rendering -*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Value and expression rendering for the LLVM IR C emitter: converts LLVM
/// Values, Constants, and inline-able instructions to C source strings.
///
//===----------------------------------------------------------------------===//

#include "LLVMCFrameLayout.h"
#include "LLVMCWriter.h"

#include "neverd/ArchSupport.h"
#include "neverd/Common.h"
#include "neverd/backend/c/MsvcCallee.h"
#include "neverd/backend/c/render/CTypeFormat.h"
#include "neverd/backend/llvm/LLVMName.h"
#include "neverd/ir/NdTypes.h"
#include "neverd/ir/TargetRegInfo.h"

#include "llvm/ADT/MapVector.h"
#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/Analysis/ConstantFolding.h"
#include "llvm/Analysis/ValueTracking.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Operator.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <functional>
#include <map>
#include <set>
#include <stdexcept>
#include <vector>

namespace neverd {

namespace {
std::string integerConstantText(const llvm::APInt &Bits) {
  if (Bits.getBitWidth() == 1)
    return Bits.isZero() ? "0" : "1";
  // The positive magnitude of INT64_MIN is not a signed decimal C literal.
  if (Bits.getBitWidth() == 64 && Bits.isMinSignedValue())
    return "(-9223372036854775807LL - 1)";
  return Bits.isNegative() ? std::to_string(Bits.getSExtValue())
                           : std::to_string(Bits.getZExtValue());
}

// Fold only literal integer DAGs. Do not borrow display-time load values or
// pointer provenance: LLVM owns bit widths, overflow flags, and poison here.
const llvm::ConstantInt *foldLiteralInteger(const llvm::Value *V,
                                            const llvm::DataLayout &DL,
                                            unsigned &Budget) {
  if (Budget == 0)
    return nullptr;
  --Budget;
  if (const auto *C = llvm::dyn_cast<llvm::ConstantInt>(V))
    return C->getBitWidth() <= 64 ? C : nullptr;
  const auto *I = llvm::dyn_cast<llvm::Instruction>(V);
  if (!I || !I->getType()->isIntegerTy() ||
      I->getType()->getIntegerBitWidth() > 64 ||
      I->hasPoisonGeneratingFlags() ||
      (!I->isBinaryOp() &&
       !llvm::isa<llvm::ICmpInst, llvm::CastInst, llvm::SelectInst>(I)))
    return nullptr;
  llvm::SmallVector<llvm::Constant *, 3> Operands;
  for (const llvm::Value *Operand : I->operands()) {
    if (!Operand->getType()->isIntegerTy())
      return nullptr;
    const auto *C = foldLiteralInteger(Operand, DL, Budget);
    if (!C)
      return nullptr;
    Operands.push_back(const_cast<llvm::ConstantInt *>(C));
  }
  return llvm::dyn_cast_or_null<llvm::ConstantInt>(
      llvm::ConstantFoldInstOperands(I, Operands, DL, nullptr,
                                     /*AllowNonDeterministic=*/false));
}

void checkConstantFoldBudget(const llvm::Constant *Root) {
  // LLVM's layout-aware constant folder recursively visits expressions and
  // vectors. Bound that input before entering it, independently of the scalar
  // instruction planner and display-time immediate-fold budget.
  constexpr unsigned MaxWork = 256, MaxDepth = 32;
  llvm::SmallVector<std::pair<const llvm::Constant *, unsigned>, 16> Pending;
  Pending.emplace_back(Root, 0);
  unsigned Work = 0;
  while (!Pending.empty()) {
    const auto [C, Depth] = Pending.pop_back_val();
    if (++Work > MaxWork || Depth > MaxDepth)
      throw std::runtime_error("LLVM C constant-fold budget exceeded");
    if (!llvm::isa<llvm::ConstantExpr, llvm::ConstantVector>(C))
      continue;
    if (C->getNumOperands() > MaxWork - Work - Pending.size())
      throw std::runtime_error("LLVM C constant-fold budget exceeded");
    for (const auto &Operand : C->operands())
      Pending.emplace_back(llvm::cast<llvm::Constant>(Operand.get()),
                           Depth + 1);
  }
}

bool looksUnsignedCExpr(const std::string &S) {
  return S.starts_with("(unsigned)") || S.starts_with("(uint") ||
         S.starts_with("((unsigned)") || S.starts_with("((uint");
}

bool integerComparisonNeedsNormalization(const llvm::Type *Type) {
  return Type->isIntegerTy() && !Type->isIntegerTy(32) &&
         !Type->isIntegerTy(64) && !Type->isIntegerTy(128);
}

std::string unsignedCmpOperand(const std::string &S) {
  return looksUnsignedCExpr(S) ? S : "(unsigned)" + S;
}

std::string signedIntegerOperand(const llvm::Type *Type,
                                 const std::string &Text) {
  const unsigned Width = Type->getIntegerBitWidth();
  if (Width > 512)
    throw std::runtime_error("LLVMC signed operand exceeds 512-bit carrier");
  if (Width > 128)
    return "(_BitInt(" + std::to_string(Width) + "))(" + Text + ")";
  const unsigned Carrier = Width <= 8    ? 8
                           : Width <= 16 ? 16
                           : Width <= 32 ? 32
                           : Width <= 64 ? 64
                                         : 128;
  const std::string Signed =
      Carrier == 128 ? "__int128_t" : "int" + std::to_string(Carrier) + "_t";
  if (Width == Carrier)
    return "(" + Signed + ")(" + Text + ")";
  const std::string Unsigned =
      Carrier == 128 ? "__uint128_t" : "uint" + std::to_string(Carrier) + "_t";
  const std::string Shift = std::to_string(Carrier - Width);
  return "((" + Signed + ")((" + Unsigned + ")(" + Text + ") << " + Shift +
         ") >> " + Shift + ")";
}

std::string signedIntegerOperand(const llvm::Value *Value,
                                 const std::string &Text) {
  return signedIntegerOperand(Value->getType(), Text);
}

/// Drop one parenthesis pair that wraps a whole compare operand only when
/// doing so cannot expose an operator with lower or equal precedence. The
/// rendered operand may forward a stored value through a load, so inspecting
/// only the LLVM operand's opcode cannot establish its C precedence.
std::string peelOperandWrap(std::string S) {
  if (S.size() < 2 || S.front() != '(' || S.back() != ')')
    return S;
  int Depth = 0;
  for (size_t I = 0; I < S.size(); ++I) {
    if (S[I] == '(')
      ++Depth;
    else if (S[I] == ')') {
      --Depth;
      if (Depth == 0 && I + 1 != S.size())
        return S;
    }
  }
  if (Depth != 0)
    return S;
  const std::string Inner = S.substr(1, S.size() - 2);
  if (Inner.find_first_of("&|^?:=<>!,") != std::string::npos)
    return S;
  return Inner;
}

std::string syntheticFrameSlotName(int64_t Disp) {
  const uint64_t Mag =
      Disp < 0 ? static_cast<uint64_t>(-Disp) : static_cast<uint64_t>(Disp);
  return (Disp < 0 ? "var_m" : "var_") + llvm::utohexstr(Mag);
}

const llvm::Value *losslessIntegerView(const llvm::CastInst *Cast,
                                       bool NormalizedBoolean) {
  // A predicate already spelled as C zero or one can lose its extension.
  // An arbitrary i1 operation still needs its source-width normalization.
  // Other width changes remain visible unless an extension/truncation cancels.
  if (NormalizedBoolean && llvm::isa<llvm::ZExtInst>(Cast) &&
      Cast->getSrcTy()->isIntegerTy(1))
    return Cast->getOperand(0);
  if (llvm::isa<llvm::TruncInst>(Cast)) {
    const auto *Extension = llvm::dyn_cast<llvm::CastInst>(Cast->getOperand(0));
    if (Extension && llvm::isa<llvm::ZExtInst, llvm::SExtInst>(Extension) &&
        Extension->getSrcTy() == Cast->getDestTy())
      return Extension->getOperand(0);
  }
  return nullptr;
}

bool orCanPeelAsAdd(const llvm::Value *Base, const llvm::ConstantInt *Bits) {
  if (Bits->isZero())
    return true;
  const auto *And = llvm::dyn_cast<llvm::BinaryOperator>(Base);
  if (!And || And->getOpcode() != llvm::Instruction::And)
    return false;
  const auto *Mask = llvm::dyn_cast<llvm::ConstantInt>(And->getOperand(0));
  if (!Mask)
    Mask = llvm::dyn_cast<llvm::ConstantInt>(And->getOperand(1));
  // When the AND clears every OR bit, OR and ADD have the same value.
  // The mask itself is printed in C, so this proof does not depend on an
  // LLVM alloca alignment that the generated C declaration may not retain.
  return Mask && (Mask->getValue() & Bits->getValue()).isZero();
}
} // namespace

bool LLVMCWriter::isNormalizedBoolean(const llvm::Value *V) const {
  // This is a proof about the emitted C expression, not its LLVM i1 type:
  // an inlined i1 add can promote to int and produce two before truncation.
  llvm::SmallVector<const llvm::Value *, 8> Pending{V};
  unsigned Budget = 64;
  while (!Pending.empty()) {
    const auto *Current = Pending.pop_back_val();
    if (!Current || !Current->getType()->isIntegerTy() || Budget == 0)
      return false;
    --Budget;
    if (const auto *CI = llvm::dyn_cast<llvm::ConstantInt>(Current)) {
      if (!CI->isZero() && !CI->isOne())
        return false;
      continue;
    }
    if (llvm::isa<llvm::CmpInst>(Current))
      continue;
    if (const auto *Cast = llvm::dyn_cast<llvm::CastInst>(Current)) {
      if (llvm::isa<llvm::ZExtInst, llvm::TruncInst>(Cast) ||
          (llvm::isa<llvm::SExtInst>(Cast) &&
           !Cast->getSrcTy()->isIntegerTy(1))) {
        Pending.push_back(Cast->getOperand(0));
        continue;
      }
    }
    if (const auto *BO = llvm::dyn_cast<llvm::BinaryOperator>(Current)) {
      if (BO->getOpcode() == llvm::Instruction::And ||
          BO->getOpcode() == llvm::Instruction::Or ||
          BO->getOpcode() == llvm::Instruction::Xor) {
        Pending.push_back(BO->getOperand(0));
        Pending.push_back(BO->getOperand(1));
        continue;
      }
    }
    if (const auto *LI = llvm::dyn_cast<llvm::LoadInst>(Current)) {
      if (!LI->isSimple())
        return false;
      if (const auto *Stored = allocaStoredValueBefore(LI);
          Stored && Stored != Current) {
        Pending.push_back(Stored);
        continue;
      }
    }
    return false;
  }
  return true;
}

const llvm::Value *LLVMCWriter::peelIntegerView(const llvm::Value *V) const {
  std::set<const llvm::Value *> Seen;
  while (V && Seen.insert(V).second) {
    if (const auto *Cast = llvm::dyn_cast<llvm::CastInst>(V)) {
      if (const llvm::Value *Inner = losslessIntegerView(
              Cast, isNormalizedBoolean(Cast->getOperand(0)))) {
        V = Inner;
        continue;
      }
    }
    if (const auto *LI = llvm::dyn_cast<llvm::LoadInst>(V)) {
      if (const llvm::Value *Stored = allocaStoredValueBefore(LI);
          Stored && Stored != V) {
        V = Stored;
        continue;
      }
    }
    break;
  }
  return V;
}

std::string LLVMCWriter::resolveNdDataName(llvm::StringRef Name) const {
  const std::optional<va_t> Addr = parseNdDataSymbol(Name);
  if (!Addr)
    return {};
  return namedImageObject(*Addr);
}

bool LLVMCWriter::isImageDataAddress(va_t Addr) const {
  if (!Img || Addr == 0 || Addr == InvalidVA)
    return false;
  if (Img->findImportAt(Addr))
    return false;
  const Segment *Seg = Img->getSegmentFor(Addr);
  if (!Seg || !Seg->isReadable())
    return false;
  if (Seg->isExecutable() && !Seg->isWritable())
    return false;
  return true;
}

std::optional<va_t>
LLVMCWriter::uniqueAllocaImageImmediate(const llvm::AllocaInst *Slot) const {
  if (!Slot)
    return std::nullopt;
  std::optional<va_t> Found;
  for (const llvm::User *U : Slot->users()) {
    const auto *SI = llvm::dyn_cast<llvm::StoreInst>(U);
    if (!SI || SI->getPointerOperand()->stripPointerCasts() != Slot)
      continue;
    const llvm::Value *Stored = SI->getValueOperand()->stripPointerCasts();
    if (const auto *I2P = llvm::dyn_cast<llvm::IntToPtrInst>(Stored))
      Stored = I2P->getOperand(0);
    const auto *CI = llvm::dyn_cast<llvm::ConstantInt>(Stored);
    if (!CI || CI->getBitWidth() > 64)
      return std::nullopt;
    if (CI->isZero())
      continue;
    const auto Addr = static_cast<va_t>(CI->getZExtValue());
    if (!isImageDataAddress(Addr))
      return std::nullopt;
    if (Found && *Found != Addr)
      return std::nullopt;
    Found = Addr;
  }
  return Found;
}

std::string LLVMCWriter::namedImageObject(va_t Addr) const {
  if (auto It = ImageObjectNames.find(Addr); It != ImageObjectNames.end())
    return It->second;
  std::string Raw;
  if (Opts.UserNames)
    if (auto It = Opts.UserNames->find(Addr); It != Opts.UserNames->end())
      Raw = It->second;
  if (Raw.empty() && Dbg) {
    if (auto Data = Dbg->resolveDataObject(Addr);
        Data && !Data->Name.empty() &&
        !llvm::StringRef(Data->Name).starts_with("??_C@"))
      Raw = Data->Name;
  }
  if (Raw.empty() && Img) {
    if (const Symbol *Sym = Img->findSymbolAt(Addr);
        Sym && !Sym->IsFunc && !Sym->Name.empty() &&
        llvm::StringRef(Sym->Name).find(kAutoFuncPrefix) != 0) {
      // A mangled name keeps the underscores its scheme starts with
      // (`_ZTV8QDomNode` is `QDomNode_vtable`).
      const llvm::StringRef CName =
          cNameOfSymbol(Sym->Name, Opts.Format, Opts.TheArch);
      Raw = symbolScheme(CName) != SymbolScheme::None
                ? CName.str()
                : stripLeadingUnderscores(Sym->Name).str();
    }
  }
  if (Raw.empty()) {
    // As the listing names it: a pointer slot, or the size of its accesses.
    const auto Use = ImageDataUses.find(Addr);
    const bool PointerSlot =
        (Img && (Img->CodePtrRelocSlots.count(Addr) ||
                 Img->DataPtrRelocSlots.count(Addr))) ||
        (Use != ImageDataUses.end() && Use->second.CallSlot);
    std::optional<uint64_t> AccessBytes;
    if (Use != ImageDataUses.end() && Use->second.AccessBytes.size() == 1)
      AccessBytes = *Use->second.AccessBytes.begin();
    Raw = makeDataName(Addr, PointerSlot, AccessBytes);
    SynthesizedImageObjects.insert(Addr);
  }
  std::string Name = ImageIdentifierAllocator.allocate(Raw, "g");
  ImageObjectNames[Addr] = Name;
  return Name;
}

std::optional<va_t> LLVMCWriter::imageDataVA(const llvm::Value *V) const {
  if (!V)
    return std::nullopt;
  V = V->stripPointerCasts();
  // A home may copy a load from another home that eventually copies this
  // same load back. No image address is established by that cycle.
  static thread_local llvm::SmallPtrSet<const llvm::Value *, 32> Active;
  // Cycle detection does not bound a long acyclic cast/home chain. This is
  // optional image provenance: exhaustion keeps the ordinary SSA projection.
  if (Active.size() >= 64 || !Active.insert(V).second)
    return std::nullopt;
  struct ActiveGuard {
    llvm::SmallPtrSet<const llvm::Value *, 32> &Values;
    const llvm::Value *Value;
    ~ActiveGuard() { Values.erase(Value); }
  } Guard{Active, V};
  if (const auto *GEP = llvm::dyn_cast<llvm::GEPOperator>(V)) {
    auto Base = imageDataVA(GEP->getPointerOperand());
    if (!Base)
      return std::nullopt;
    llvm::APInt Off(64, 0);
    if (CurMod && GEP->accumulateConstantOffset(CurMod->getDataLayout(), Off))
      return static_cast<va_t>(static_cast<int64_t>(*Base) +
                               Off.getSExtValue());
    if (GEP->getNumOperands() == 2) {
      if (auto *CI = llvm::dyn_cast<llvm::ConstantInt>(GEP->getOperand(1)))
        return static_cast<va_t>(static_cast<int64_t>(*Base) +
                                 CI->getSExtValue());
      if (auto Imm = foldImmediate(GEP->getOperand(1))) {
        uint64_t Off = 0;
        if (!llvm::StringRef(*Imm).getAsInteger(0, Off))
          return static_cast<va_t>(static_cast<int64_t>(*Base) +
                                   static_cast<int64_t>(Off));
      }
    }
    return std::nullopt;
  }
  if (auto *CE = llvm::dyn_cast<llvm::ConstantExpr>(V)) {
    switch (CE->getOpcode()) {
    case llvm::Instruction::BitCast:
    case llvm::Instruction::AddrSpaceCast:
    case llvm::Instruction::IntToPtr:
    case llvm::Instruction::PtrToInt:
      return imageDataVA(CE->getOperand(0));
    default:
      return std::nullopt;
    }
  }
  if (auto *GV = llvm::dyn_cast<llvm::GlobalVariable>(V)) {
    if (auto Addr = parseNdDataSymbol(GV->getName()))
      return Addr;
    if (auto Addr = parseNdCodePtrSymbol(GV->getName()))
      return Addr;
  }
  if (const auto *I2P = llvm::dyn_cast<llvm::IntToPtrInst>(V))
    return imageDataVA(I2P->getOperand(0));
  if (const auto *P2I = llvm::dyn_cast<llvm::PtrToIntInst>(V))
    return imageDataVA(P2I->getOperand(0));
  if (const auto *Cast = llvm::dyn_cast<llvm::CastInst>(V))
    return imageDataVA(Cast->getOperand(0));
  if (const auto *LI = llvm::dyn_cast<llvm::LoadInst>(V)) {
    if (const llvm::AllocaInst *Slot =
            asAllocaPointer(LI->getPointerOperand())) {
      if (const llvm::Value *Stored = allocaStoredValueBefore(LI);
          Stored && Stored != V)
        if (auto Addr = imageDataVA(Stored))
          return Addr;
      if (auto Addr = uniqueAllocaImageImmediate(Slot))
        return Addr;
    }
  }
  if (const auto *CI = llvm::dyn_cast<llvm::ConstantInt>(V)) {
    if (CI->getBitWidth() > 64)
      return std::nullopt;
    const auto Addr = static_cast<va_t>(CI->getZExtValue());
    if (isImageDataAddress(Addr))
      return Addr;
  }
  if (V->getType()->isPointerTy()) {
    if (auto Imm = foldImmediate(V)) {
      uint64_t Addr = 0;
      if (!llvm::StringRef(*Imm).getAsInteger(0, Addr) &&
          isImageDataAddress(static_cast<va_t>(Addr)))
        return static_cast<va_t>(Addr);
    }
  }
  return std::nullopt;
}

std::optional<std::pair<const llvm::GlobalVariable *, uint64_t>>
LLVMCWriter::imageByteArrayBacking(const llvm::Value *V,
                                   uint64_t AccessSize) const {
  if (!CurMod || AccessSize == 0)
    return std::nullopt;
  const auto Addr = imageDataVA(V);
  if (!Addr)
    return std::nullopt;
  for (const auto &GV : CurMod->globals()) {
    const auto Base = parseNdDataSymbol(GV.getName());
    const auto *Array = llvm::dyn_cast<llvm::ArrayType>(GV.getValueType());
    if (!Base || !Array || !Array->getElementType()->isIntegerTy(8) ||
        !GV.hasInitializer() ||
        (!llvm::isa<llvm::ConstantAggregateZero>(GV.getInitializer()) &&
         !llvm::isa<llvm::ConstantDataArray>(GV.getInitializer())) ||
        *Addr < *Base || AccessSize > Array->getNumElements())
      continue;
    const uint64_t Offset = *Addr - *Base;
    if (Offset > Array->getNumElements() - AccessSize)
      continue;
    return std::make_pair(&GV, Offset);
  }
  return std::nullopt;
}

std::optional<std::string>
LLVMCWriter::imageByteArrayPointer(const llvm::Value *V,
                                   uint64_t AccessSize) const {
  const auto Backing = imageByteArrayBacking(V, AccessSize);
  if (!Backing)
    return std::nullopt;
  const auto Base = parseNdDataSymbol(Backing->first->getName());
  return "(" + namedImageObject(*Base) + " + " +
         std::to_string(Backing->second) + ")";
}

std::optional<uint64_t> LLVMCWriter::foldReadonlyScalar(va_t Addr,
                                                        uint16_t Size) const {
  if (!Img)
    return std::nullopt;
  const Segment *Seg = Img->getSegmentFor(Addr);
  if (!Seg || Seg->isWritable())
    return std::nullopt;
  if (Size != 1 && Size != 2 && Size != 4 && Size != 8)
    return std::nullopt;
  const uint8_t *Bytes = Img->readVA(Addr, Size);
  if (!Bytes)
    return std::nullopt;
  uint64_t Value = 0;
  for (uint16_t I = 0; I < Size; ++I)
    Value |= static_cast<uint64_t>(Bytes[I]) << (8 * I);
  return Value;
}

std::string LLVMCWriter::imageDataCName(const llvm::Value *V) const {
  // Full-module output owns these bytes in a pointer mirror. Its interior
  // fields are offsets into that object, not separate globals at the original
  // VAs. Single-function output instead declares the referenced image objects.
  if (!OnlyFunction && V->getType()->isPointerTy())
    if (const auto *Global =
            llvm::dyn_cast<llvm::GlobalVariable>(llvm::getUnderlyingObject(V));
        Global && parseNdCodePtrSymbol(Global->getName()))
      return {};
  const auto VA = imageDataVA(V);
  if (!VA)
    return {};
  return namedImageObject(*VA);
}

std::string LLVMCWriter::freshVar(const std::string &Hint) {
  std::string Name;
  // The synthetic frame is named at its first printed use. Number it on its
  // own so omitting an earlier unread spill does not rename that slot.
  if (Hint == "frame") {
    unsigned N = 0;
    do {
      Name = Hint + std::to_string(N++);
    } while (UsedNames.count(Name));
    UsedNames.insert(Name);
    return Name;
  }
  do {
    Name = Hint + std::to_string(NextVar++);
  } while (UsedNames.count(Name));
  UsedNames.insert(Name);
  return Name;
}

std::string LLVMCWriter::getName(const llvm::Value *V) {
  auto It = ValNames.find(V);
  if (It != ValNames.end())
    return It->second;

  std::string Hint = "v";
  if (V->hasName()) {
    std::string Raw = V->getName().str();
    std::string Clean;
    for (char Ch : Raw) {
      if (std::isalnum(static_cast<unsigned char>(Ch)) || Ch == '_')
        Clean += Ch;
      else
        Clean += '_';
    }
    if (!Clean.empty() && !std::isdigit(static_cast<unsigned char>(Clean[0])))
      Hint = Clean;
  }
  auto Name = freshVar(Hint);
  ValNames[V] = Name;
  return Name;
}

std::string LLVMCWriter::constStr(const llvm::Constant *C) {
  if (const auto *Vector =
          llvm::dyn_cast<llvm::FixedVectorType>(C->getType())) {
    if (!isCVectorType(C->getType()))
      throw std::runtime_error(
          "LLVM C constant has an unsupported vector type");
    std::string Text = "(" + typeToCLLVM(C->getType()) + "){";
    for (unsigned Lane = 0; Lane < Vector->getNumElements(); ++Lane) {
      if (Lane)
        Text += ", ";
      const auto *Element = C->getAggregateElement(Lane);
      if (!Element)
        throw std::runtime_error("LLVM C vector constant has no lane value");
      // Undefined lanes permit a stable zero choice, without observing or
      // inventing a value for any defined lane.
      if (const auto *FP = llvm::dyn_cast<llvm::ConstantFP>(Element)) {
        const auto Bits = FP->getValueAPF().bitcastToAPInt();
        auto *Integer =
            llvm::IntegerType::get(C->getContext(), Bits.getBitWidth());
        // C union member access preserves the representation, including NaN
        // payloads and signed zero, without compiler-specific bit-cast support.
        Text += "((union { " + typeToCLLVM(Integer) + " bits; " +
                typeToCLLVM(Element->getType()) + " value; }){ .bits = 0x" +
                llvm::utohexstr(Bits.getZExtValue()) + "ULL }).value";
      } else {
        Text += llvm::isa<llvm::UndefValue, llvm::PoisonValue>(Element)
                    ? "0"
                    : constStr(Element);
      }
    }
    return Text + "}";
  }
  if (auto *CI = llvm::dyn_cast<llvm::ConstantInt>(C)) {
    if (CI->getBitWidth() > 64) {
      if (CI->getBitWidth() > 128) {
        const std::string Type = typeToCLLVM(CI->getType());
        std::string Text = "(";
        const auto &Bits = CI->getValue();
        for (unsigned Offset = 0; Offset < Bits.getBitWidth(); Offset += 64) {
          if (Offset)
            Text += " | ";
          const unsigned Count = std::min(64U, Bits.getBitWidth() - Offset);
          Text += "((" + Type + ")0x" +
                  llvm::utohexstr(Bits.extractBitsAsZExtValue(Count, Offset)) +
                  "ULL << " + std::to_string(Offset) + ")";
        }
        return Text + ")";
      }
      auto Value = CI->getValue().zextOrTrunc(128);
      auto Low = Value.extractBitsAsZExtValue(64, 0);
      auto High = Value.extractBitsAsZExtValue(64, 64);
      return "(((__uint128_t)0x" + llvm::utohexstr(High) +
             "ULL << 64) | (__uint128_t)0x" + llvm::utohexstr(Low) + "ULL)";
    }
    if (auto Lit = imageStringLiteral(Img, CI->getZExtValue(),
                                      /*AllowEmpty=*/true))
      return *Lit;
    return integerConstantText(CI->getValue());
  }

  if (auto *CF = llvm::dyn_cast<llvm::ConstantFP>(C)) {
    llvm::SmallString<32> Buf;
    CF->getValueAPF().toString(Buf, 0, 0);
    auto S = std::string(Buf.str());
    if (C->getType()->isFloatTy())
      S += "f";
    return S;
  }

  if (llvm::isa<llvm::ConstantPointerNull>(C))
    return "((void*)0)";

  if (auto *CE = llvm::dyn_cast<llvm::ConstantExpr>(C)) {
    // IRBuilder can retain scalar constant expressions containing vector
    // bitcasts/extracts. Resolve their complete bit pattern through LLVM's
    // layout-aware folder before choosing a C spelling.
    if (CurMod) {
      checkConstantFoldBudget(CE);
      if (const auto *Folded =
              llvm::ConstantFoldConstant(CE, CurMod->getDataLayout());
          Folded != CE)
        return constStr(Folded);
    }
    if (CE->getOpcode() == llvm::Instruction::GetElementPtr) {
      std::string Image = imageDataCName(CE);
      if (!Image.empty())
        return "&" + Image;
      return gepExpr(*llvm::cast<llvm::GEPOperator>(CE));
    }
    if (CE->getOpcode() == llvm::Instruction::PtrToInt) {
      auto *Src = CE->getOperand(0);
      if (auto *Fn = llvm::dyn_cast<llvm::Function>(Src)) {
        return "(" + typeToCLLVM(CE->getType()) + ")(void*)" +
               functionIdentifier(*Fn);
      }
      // A global denotes its address, never its initializer bytes. Arrays
      // decay to that address; imported data already carries an address.
      if (const auto *Global = llvm::dyn_cast<llvm::GlobalVariable>(Src);
          Global && !Global->isDeclaration() &&
          !Global->getValueType()->isArrayTy() &&
          !ExternalDataIdentifiers.count(Global))
        return "(" + typeToCLLVM(CE->getType()) + ")(void*)&" + valueStr(Src);
      return "(" + typeToCLLVM(CE->getType()) + ")(void*)" + valueStr(Src);
    }
    if (CE->getOpcode() == llvm::Instruction::IntToPtr) {
      if (auto *CI = llvm::dyn_cast<llvm::ConstantInt>(CE->getOperand(0));
          CI && CI->getBitWidth() <= 64)
        if (auto Lit = imageStringLiteral(Img, CI->getZExtValue(),
                                          /*AllowEmpty=*/true))
          return *Lit;
    }
    if (CE->getOpcode() == llvm::Instruction::BitCast ||
        CE->getOpcode() == llvm::Instruction::IntToPtr) {
      return "(" + typeToCLLVM(CE->getType()) + ")" +
             valueStr(CE->getOperand(0));
    }
    if ((CE->getOpcode() == llvm::Instruction::Add ||
         CE->getOpcode() == llvm::Instruction::Sub) &&
        (CE->getType()->isIntegerTy(8) || CE->getType()->isIntegerTy(16) ||
         CE->getType()->isIntegerTy(32) || CE->getType()->isIntegerTy(64))) {
      // Symbol-relative integer addresses remain relocatable in C too. Keep
      // each operand at the LLVM bit width and normalize the result after C's
      // integer promotions; do not evaluate a pointer using its old image VA.
      const std::string Type = typeToCLLVM(CE->getType());
      const std::string Left =
          "(" + Type + ")(" + constStr(CE->getOperand(0)) + ")";
      const std::string Right =
          "(" + Type + ")(" + constStr(CE->getOperand(1)) + ")";
      return "(" + Type + ")(" + Left +
             (CE->getOpcode() == llvm::Instruction::Add ? " + " : " - ") +
             Right + ")";
    }
    std::string Detail;
    llvm::raw_string_ostream Stream(Detail);
    Stream << "LLVM C constant expression is not supported: " << *CE;
    throw std::runtime_error(Stream.str());
  }

  if (auto *GV = llvm::dyn_cast<llvm::GlobalValue>(C)) {
    if (const auto *Fn = llvm::dyn_cast<llvm::Function>(GV))
      return functionIdentifier(*Fn);
    if (const auto *Global = llvm::dyn_cast<llvm::GlobalVariable>(GV))
      if (const auto It = ExternalDataIdentifiers.find(Global);
          It != ExternalDataIdentifiers.end())
        return It->second;
    std::string N = GV->getName().str();
    if (N.empty())
      return getName(C);

    std::string Resolved = resolveNdDataName(N);
    if (!Resolved.empty())
      return Resolved;

    // A symbol defined elsewhere keeps its C name: `__cxa_atexit` is not
    // `_cxa_atexit`.
    if (GV->isDeclaration())
      return canonicalizeCProjectionIdentifier(cNameOfGlobal(N, true),
                                               "nd_symbol");

    if (N[0] == '_')
      N = N.substr(1);
    std::string Clean;
    for (char Ch : N) {
      if (std::isalnum(static_cast<unsigned char>(Ch)) || Ch == '_')
        Clean += Ch;
      else
        Clean += '_';
    }
    if (Clean.empty() || std::isdigit(static_cast<unsigned char>(Clean[0])))
      Clean = "g_" + Clean;
    return Clean;
  }

  // LLVM arrays are first-class values. Their C carrier is a record containing
  // an array, so copies, calls and PHIs keep value semantics instead of decay.
  if (C->getType()->isAggregateType()) {
    std::string Text = "(" + typeToCLLVM(C->getType()) + "){";
    const bool Array = C->getType()->isArrayTy();
    if (Array)
      Text += "{";
    if (llvm::isa<llvm::UndefValue, llvm::ConstantAggregateZero>(C)) {
      Text += "0";
    } else {
      const unsigned Count =
          Array ? llvm::cast<llvm::ArrayType>(C->getType())->getNumElements()
                : llvm::cast<llvm::StructType>(C->getType())->getNumElements();
      for (unsigned I = 0; I < Count; ++I) {
        if (I)
          Text += ", ";
        const auto *Element = C->getAggregateElement(I);
        // Aggregate integer elements are literal bits, never image addresses.
        // Low byte values can coincide with strings in a low-VA ELF header.
        if (const auto *Integer = llvm::dyn_cast<llvm::ConstantInt>(Element))
          Text += integerConstantText(Integer->getValue());
        else
          Text += constStr(Element);
      }
    }
    return Text + (Array ? "}}" : "}");
  }

  if (llvm::isa<llvm::UndefValue>(C))
    return "0";

  if (auto *CA = llvm::dyn_cast<llvm::ConstantAggregate>(C)) {
    std::string S = "{";
    for (unsigned I = 0; I < CA->getNumOperands(); ++I) {
      if (I > 0)
        S += ", ";
      S += constStr(llvm::cast<llvm::Constant>(CA->getOperand(I)));
    }
    S += "}";
    return S;
  }

  if (auto *CDA = llvm::dyn_cast<llvm::ConstantDataSequential>(C)) {
    if (CDA->isString())
      return "\"" + escapeCString(CDA->getAsString()) + "\"";
    std::string S = "{";
    for (unsigned I = 0; I < CDA->getNumElements(); ++I) {
      if (I > 0)
        S += ", ";
      S += constStr(CDA->getElementAsConstant(I));
    }
    S += "}";
    return S;
  }

  if (llvm::isa<llvm::ConstantAggregateZero>(C))
    return "{0}";

  return "0";
}

std::optional<std::string>
LLVMCWriter::foldImmediate(const llvm::Value *V) const {
  if (MaterializedExpressions.count(V))
    return std::nullopt;
  // The local expression walk can reenter through imageDataVA and alloca
  // store classification. A mutable loop may then revisit this same root
  // with a fresh local Seen set. No immediate is established by that cycle.
  if (ActiveImmediateFolds.empty())
    ImmediateFoldWork = 64;
  if (ImmediateFoldWork == 0 || !ActiveImmediateFolds.insert(V).second)
    return std::nullopt;
  --ImmediateFoldWork;
  struct FoldGuard {
    std::set<const llvm::Value *> &Active;
    const llvm::Value *Value;
    ~FoldGuard() { Active.erase(Value); }
  } Guard{ActiveImmediateFolds, V};
  auto SupportsWidth = [](const llvm::Value *Value) {
    return Value && (!Value->getType()->isIntegerTy() ||
                     Value->getType()->getIntegerBitWidth() <= 64);
  };
  auto ParseInteger = [](llvm::StringRef Text,
                         unsigned Width) -> std::optional<llvm::APInt> {
    if (Text == "(-9223372036854775807LL - 1)")
      return llvm::APInt(64, UINT64_C(1) << 63).zextOrTrunc(Width);
    const bool Negative = Text.consume_front("-");
    llvm::APInt Bits;
    if (Text.getAsInteger(0, Bits))
      return std::nullopt;
    Bits = Bits.zextOrTrunc(Width);
    return Negative ? -Bits : Bits;
  };
  auto IntegerText = [](const llvm::APInt &Bits) {
    return integerConstantText(Bits);
  };
  // Wider values retain their full representation through
  // constStr/renderInline.
  if (!SupportsWidth(V) || llvm::isa<llvm::FreezeInst>(V))
    return std::nullopt;
  if (const auto *Load = llvm::dyn_cast<llvm::LoadInst>(V))
    if (!Load->isSimple())
      return std::nullopt;
  if (CurMod) {
    if (const auto *C =
            foldLiteralInteger(V, CurMod->getDataLayout(), ImmediateFoldWork))
      return integerConstantText(C->getValue());
  }
  if (auto Known = KnownImmediates.find(V); Known != KnownImmediates.end())
    return Known->second;
  if (isUnknownPlaceholder(V))
    return std::nullopt;
  if (const auto *CI = llvm::dyn_cast<llvm::ConstantInt>(V)) {
    if (CI->getBitWidth() > 64)
      return std::nullopt;
    return integerConstantText(CI->getValue());
  }

  llvm::SmallPtrSet<const llvm::Value *, 16> Seen;
  auto Rec = [&](auto &&Self,
                 const llvm::Value *Cur) -> std::optional<std::string> {
    // Share this budget with reentrant image/alloca classification. A long
    // nonconstant SSA chain must not trigger an unbounded walk at every use.
    if (ImmediateFoldWork == 0)
      return std::nullopt;
    --ImmediateFoldWork;
    if (!SupportsWidth(Cur) || MaterializedExpressions.count(Cur) ||
        llvm::isa<llvm::FreezeInst>(Cur) || !Seen.insert(Cur).second)
      return std::nullopt;
    if (auto Known = KnownImmediates.find(Cur); Known != KnownImmediates.end())
      return Known->second;
    if (isUnknownPlaceholder(Cur))
      return std::nullopt;
    if (const auto *CI = llvm::dyn_cast<llvm::ConstantInt>(Cur)) {
      if (CI->getBitWidth() > 64)
        return std::nullopt;
      return integerConstantText(CI->getValue());
    }
    if (const auto *LI = llvm::dyn_cast<llvm::LoadInst>(Cur)) {
      if (!LI->isSimple())
        return std::nullopt;
      if (auto Fwd = Analysis.ForwardedLoads.find(LI);
          Fwd != Analysis.ForwardedLoads.end())
        return Self(Self, Fwd->second);
      if (const llvm::AllocaInst *Slot =
              asAllocaPointer(LI->getPointerOperand())) {
        if (const auto *Stored = allocaStoredValueBefore(LI))
          return Self(Self, Stored);
        // A block-local forwarding index cannot answer a cross-block load.
        // Only collapse such a load when every incoming path initialized the
        // exact slot and all of its stores establish the same bit pattern.
        if (allocaLoadHasReachingStore(LI)) {
          if (auto Known = OmittedAllocaImmediates.find(Slot);
              Known != OmittedAllocaImmediates.end())
            return Known->second;
          if (auto Common = uniqueAllocaImmediate(Slot))
            return Common;
        }
      }
      if (auto VA = imageDataVA(LI->getPointerOperand())) {
        const auto *Ty = LI->getType();
        const uint16_t Size =
            Ty && Ty->isIntegerTy()
                ? static_cast<uint16_t>(Ty->getIntegerBitWidth() / 8)
                : 0;
        if (auto Imm = foldReadonlyScalar(*VA, Size))
          return "0x" + llvm::utohexstr(*Imm);
      }
      return std::nullopt;
    }
    if (const auto *Cast = llvm::dyn_cast<llvm::CastInst>(Cur)) {
      if (!Cast->getSrcTy()->isIntegerTy() || !Cast->getDestTy()->isIntegerTy())
        return std::nullopt;
      auto Text = Self(Self, Cast->getOperand(0));
      if (!Text)
        return std::nullopt;
      auto Bits = ParseInteger(*Text, Cast->getSrcTy()->getIntegerBitWidth());
      if (!Bits)
        return std::nullopt;
      const unsigned Width = Cast->getDestTy()->getIntegerBitWidth();
      switch (Cast->getOpcode()) {
      case llvm::Instruction::Trunc:
        *Bits = Bits->trunc(Width);
        break;
      case llvm::Instruction::ZExt:
        *Bits = Bits->zext(Width);
        break;
      case llvm::Instruction::SExt:
        *Bits = Bits->sext(Width);
        break;
      case llvm::Instruction::BitCast:
        break;
      default:
        return std::nullopt;
      }
      llvm::StringRef OriginalText(*Text);
      const bool Negative = OriginalText.consume_front("-");
      llvm::APInt OriginalBits;
      if (OriginalText.getAsInteger(0, OriginalBits))
        return std::nullopt;
      llvm::SmallString<64> Original;
      OriginalBits.toString(Original, 10, false);
      if (Negative)
        Original.insert(Original.begin(), '-');
      llvm::SmallString<64> Folded;
      Bits->toString(Folded, 10, llvm::isa<llvm::SExtInst>(Cast));
      // Preserve hexadecimal constants when the cast kept their value.
      return Folded == Original ? Text : std::optional(Folded.str().str());
    }
    if (const auto *Phi = llvm::dyn_cast<llvm::PHINode>(Cur)) {
      if (Phi->getNumIncomingValues() == 0)
        return std::nullopt;
      std::optional<std::string> Common;
      for (const llvm::Value *Inc : Phi->incoming_values()) {
        auto Imm = Self(Self, Inc);
        if (!Imm)
          return std::nullopt;
        if (!Common)
          Common = std::move(Imm);
        else if (*Common != *Imm)
          return std::nullopt;
      }
      return Common;
    }
    // The unoptimized MedIR route also keeps constant counts in scalar homes.
    // Reuse only the existing proved immediate values, then restore the LLVM
    // operand width before comparing or taking an unsigned remainder.
    if (const auto *I = llvm::dyn_cast<llvm::Instruction>(Cur);
        I && !I->hasPoisonGeneratingFlags() &&
        (llvm::isa<llvm::ICmpInst>(I) ||
         I->getOpcode() == llvm::Instruction::URem)) {
      llvm::SmallVector<llvm::Constant *, 2> Operands;
      for (const llvm::Value *Operand : I->operands()) {
        auto *Ty = llvm::dyn_cast<llvm::IntegerType>(Operand->getType());
        if (!Ty || Ty->getBitWidth() > 64)
          return std::nullopt;
        auto Text = Self(Self, Operand);
        if (!Text)
          return std::nullopt;
        uint64_t Bits = 0;
        int64_t Signed = 0;
        if (llvm::StringRef(*Text).getAsInteger(0, Bits)) {
          if (llvm::StringRef(*Text).getAsInteger(0, Signed))
            return std::nullopt;
          Bits = static_cast<uint64_t>(Signed);
        }
        Operands.push_back(llvm::ConstantInt::get(Ty, Bits));
      }
      if (CurMod)
        if (const auto *C = llvm::dyn_cast_or_null<llvm::ConstantInt>(
                llvm::ConstantFoldInstOperands(
                    I, Operands, CurMod->getDataLayout(), nullptr, false)))
          return llvm::toString(C->getValue(), 10,
                                !C->getType()->isIntegerTy(1));
      return std::nullopt;
    }
    if (const auto *BO = llvm::dyn_cast<llvm::BinaryOperator>(Cur)) {
      if ((BO->getOpcode() == llvm::Instruction::Xor ||
           BO->getOpcode() == llvm::Instruction::Sub) &&
          BO->getOperand(0) == BO->getOperand(1))
        return std::string("0");
      if (BO->getOpcode() == llvm::Instruction::Add ||
          BO->getOpcode() == llvm::Instruction::Sub) {
        auto LHS = Self(Self, BO->getOperand(0));
        if (!LHS)
          return std::nullopt;
        auto RHS = Self(Self, BO->getOperand(1));
        if (!LHS || !RHS || !BO->getType()->isIntegerTy())
          return std::nullopt;
        const unsigned Width = BO->getType()->getIntegerBitWidth();
        auto A = ParseInteger(*LHS, Width);
        auto B = ParseInteger(*RHS, Width);
        if (!A || !B)
          return std::nullopt;
        const bool Add = BO->getOpcode() == llvm::Instruction::Add;
        bool Overflow = false;
        if (BO->hasNoUnsignedWrap()) {
          (void)(Add ? A->uadd_ov(*B, Overflow) : A->usub_ov(*B, Overflow));
          if (Overflow)
            return std::nullopt;
        }
        if (BO->hasNoSignedWrap()) {
          (void)(Add ? A->sadd_ov(*B, Overflow) : A->ssub_ov(*B, Overflow));
          if (Overflow)
            return std::nullopt;
        }
        return IntegerText(Add ? *A + *B : *A - *B);
      }
    }
    return std::nullopt;
  };
  return Rec(Rec, V);
}

namespace {
void completeDisplayRecord(DebugContext *Dbg, const TypeRef &Ty) {
  if (!Dbg || !Ty)
    return;
  TypeRef Cur = Ty;
  while (Cur && Cur->Kind == NdTypeKind::Ptr)
    Cur = Cur->Pointee;
  if (!Cur)
    return;
  Dbg->completeType(Cur);
  if (Cur->Kind != NdTypeKind::Struct || Cur->IsEnum)
    return;
  if (Cur->FieldDisplayTypes.size() != Cur->FieldDisplayOffsets.size())
    return;
  for (const auto &F : Cur->FieldDisplayTypes) {
    if (!F)
      continue;
    TypeRef Inner = F;
    while (Inner && Inner->Kind == NdTypeKind::Ptr)
      Inner = Inner->Pointee;
    if (Inner && Inner->Kind == NdTypeKind::Struct && !Inner->IsEnum)
      Dbg->completeType(Inner);
  }
}

std::optional<std::string> canonicalizeDisplayPath(llvm::StringRef Field) {
  std::string Path;
  llvm::StringRef Rest(Field);
  while (!Rest.empty()) {
    const auto Split = Rest.split('.');
    const std::string Comp = Split.first.str();
    const std::string Name = canonicalizeCProjectionIdentifier(Comp, "");
    if (Name.empty() || Name != Comp)
      return std::nullopt;
    if (!Path.empty())
      Path += '.';
    Path += Name;
    Rest = Split.second;
  }
  if (Path.empty())
    return std::nullopt;
  return Path;
}
} // namespace

uint16_t LLVMCWriter::llvmAccessSize(const llvm::Type *Ty) const {
  if (!Ty)
    return 0;
  if (CurMod) {
    const uint64_t Size =
        CurMod->getDataLayout().getTypeStoreSize(const_cast<llvm::Type *>(Ty));
    if (Size && Size <= UINT16_MAX)
      return static_cast<uint16_t>(Size);
  }
  if (Ty->isIntegerTy())
    return static_cast<uint16_t>(Ty->getIntegerBitWidth() / 8);
  return 0;
}

TypeRef LLVMCWriter::cDisplayType(const TypeRef &Ty) const {
  if (!Ty)
    return Ty;
  if (Ty->Kind == NdTypeKind::Ptr)
    return NdType::makePtr(cDisplayType(Ty->Pointee));
  if (Ty->Kind == NdTypeKind::Struct && !Ty->SourceName.empty()) {
    auto Out = NdType::makeNamedRecord(cNamedTypeSpelling(Ty->SourceName),
                                       Ty->Size ? Ty->Size : 8, Ty->IsEnum);
    Out->FieldDisplayNames = Ty->FieldDisplayNames;
    Out->FieldDisplayOffsets = Ty->FieldDisplayOffsets;
    Out->FieldDisplayTypes = Ty->FieldDisplayTypes;
    return Out;
  }
  return Ty;
}

bool LLVMCWriter::isReservedFrameName(llvm::StringRef Name) const {
  if (Name.empty() || Name == "this" || Name == "result")
    return true;
  if (!DebugFn)
    return false;
  for (const auto &Param : DebugFn->Params)
    if (!Param.first.empty() && Name == Param.first)
      return true;
  return false;
}

void LLVMCWriter::discoverSyntheticFrame(llvm::Function &Fn) {
  SyntheticFrame = nullptr;
  FrameBaseOffset = 0;
  const llvm::AllocaInst *Named = nullptr;
  const llvm::AllocaInst *Largest = nullptr;
  uint64_t LargestN = 0;
  for (auto &BB : Fn) {
    for (auto &Inst : BB) {
      const auto *AI = llvm::dyn_cast<llvm::AllocaInst>(&Inst);
      if (!AI)
        continue;
      const auto *Arr = llvm::dyn_cast<llvm::ArrayType>(AI->getAllocatedType());
      if (!Arr || !Arr->getElementType()->isIntegerTy(8))
        continue;
      const uint64_t N = Arr->getNumElements();
      if (AI->getName().starts_with("frame"))
        Named = AI;
      if (N > LargestN) {
        LargestN = N;
        Largest = AI;
      }
    }
  }
  SyntheticFrame = Named ? Named : (LargestN >= 16 ? Largest : nullptr);
  if (!SyntheticFrame)
    return;
  const llvm::Module *Mod = CurMod ? CurMod : Fn.getParent();
  if (!Mod)
    return;
  FrameBaseOffset =
      llvmc::syntheticFrameBaseOffset(*SyntheticFrame, Mod->getDataLayout())
          .value_or(0);
  SpLocalOwner.clear();
  SpLocalNameCandidates.clear();
  SpLocalAmbiguousNames.clear();
  if (!Dbg || !FunctionEntry || !SyntheticFrame || FrameBaseOffset == 0)
    return;
  const uint64_t Residue =
      syntheticEntryStackResidue(Opts.TheArch, Opts.Format);
  if (FrameBaseOffset <= Residue)
    return;
  const int64_t Aligned = static_cast<int64_t>(FrameBaseOffset - Residue);
  std::map<int64_t, std::set<uint64_t>> Owners;
  std::set<uint64_t> Taken;
  std::set<const llvm::Value *> Seen;
  auto frameOff = [&](auto &&self, const llvm::Value *V,
                      int Depth) -> std::optional<uint64_t> {
    if (!V || Depth > 8 || !Seen.insert(V).second)
      return std::nullopt;
    if (const auto Peeled = peelPointerOffset(V)) {
      if (Peeled->first == SyntheticFrame)
        return Peeled->second;
    }
    if (const auto *Cast = llvm::dyn_cast<llvm::CastInst>(V))
      if (auto Off = self(self, Cast->getOperand(0), Depth + 1))
        return Off;
    if (const auto *BO = llvm::dyn_cast<llvm::BinaryOperator>(V)) {
      const bool Add = BO->getOpcode() == llvm::Instruction::Add;
      const bool Sub = BO->getOpcode() == llvm::Instruction::Sub;
      if (Add || Sub) {
        if (const auto *CI =
                llvm::dyn_cast<llvm::ConstantInt>(BO->getOperand(1))) {
          if (auto Base = self(self, BO->getOperand(0), Depth + 1))
            return Add ? *Base + CI->getZExtValue()
                       : *Base - CI->getZExtValue();
        }
      }
    }
    const auto *LI = llvm::dyn_cast<llvm::LoadInst>(V);
    if (!LI)
      return std::nullopt;
    const llvm::AllocaInst *Slot = asAllocaPointer(LI->getPointerOperand());
    if (!Slot)
      return std::nullopt;
    for (const llvm::User *U : Slot->users()) {
      const auto *SI = llvm::dyn_cast<llvm::StoreInst>(U);
      if (!SI || SI->getPointerOperand() != Slot)
        continue;
      if (auto Off = self(self, SI->getValueOperand(), Depth + 1))
        return Off;
    }
    return std::nullopt;
  };
  for (llvm::BasicBlock &BB : Fn) {
    for (llvm::Instruction &Inst : BB) {
      const auto *CB = llvm::dyn_cast<llvm::CallBase>(&Inst);
      if (!CB)
        continue;
      for (const llvm::Use &Arg : CB->args())
        if (auto Off = frameOff(frameOff, Arg.get(), 0))
          Taken.insert(*Off);
    }
  }
  for (uint64_t Off : Taken) {
    const int64_t Disp =
        static_cast<int64_t>(Off) - static_cast<int64_t>(FrameBaseOffset);
    for (int Delta = 0; Delta < 16 && Aligned - Delta > 0; ++Delta) {
      const int64_t Query = Disp + (Aligned - Delta);
      if (auto Var = Dbg->resolveStackPointerVariable(FunctionEntry, Query))
        Owners[Var->StackOffset].insert(Off);
    }
  }
  for (const auto &[Stack, Offs] : Owners)
    if (Offs.size() == 1)
      SpLocalOwner[Stack] = *Offs.begin();
  // Only a small alignment residue is unknown here. Enumerate exact PDB SP
  // locations once, then reject a second S_FRAMEPROC-relative spelling of
  // the same name when it is outside every possible SP-relative slot.
  const int64_t ScanEnd = std::min<int64_t>(Aligned, 4096);
  std::map<std::string, int64_t> FirstSPNameOffset;
  for (int64_t Stack = -15; Stack <= ScanEnd; ++Stack) {
    const auto Var = Dbg->resolveStackPointerVariable(FunctionEntry, Stack);
    if (!Var || Var->IsParam || Var->Name.empty() ||
        isReservedFrameName(Var->Name))
      continue;
    auto [First, Inserted] = FirstSPNameOffset.emplace(Var->Name, Stack);
    if (!Inserted && First->second != Stack)
      SpLocalAmbiguousNames.insert(Var->Name);
    auto &Candidates = SpLocalNameCandidates[Var->Name];
    for (int64_t Delta = 0; Delta < 16; ++Delta) {
      const int64_t Off = Stack + static_cast<int64_t>(Residue) + Delta;
      if (Off >= 0)
        Candidates.insert(static_cast<uint64_t>(Off));
    }
  }
  FrameDebugCache.clear();
}

std::optional<VariableSym> LLVMCWriter::debugFrameVariable(int64_t Disp) const {
  if (auto Hit = FrameDebugCache.find(Disp); Hit != FrameDebugCache.end())
    return Hit->second;
  auto Accept =
      [this](std::optional<VariableSym> Var) -> std::optional<VariableSym> {
    if (!Var || Var->IsParam || Var->Name.empty())
      return std::nullopt;
    const std::string Name = canonicalizeCProjectionIdentifier(Var->Name, "");
    if (Name.empty() || Name != Var->Name || isReservedFrameName(Name) ||
        SpLocalAmbiguousNames.count(Name))
      return std::nullopt;
    if (Dbg && Var->Type)
      Dbg->completeType(Var->Type);
    Var->Type = cDisplayType(Var->Type);
    Var->Name = Name;
    return Var;
  };
  std::optional<VariableSym> Found;
  if (Dbg && FunctionEntry)
    Found = Accept(Dbg->resolveVariable(FunctionEntry, Disp));
  if (Found) {
    const auto Candidates = SpLocalNameCandidates.find(Found->Name);
    const uint64_t AccessOff =
        static_cast<uint64_t>(static_cast<int64_t>(FrameBaseOffset) + Disp);
    if (Candidates != SpLocalNameCandidates.end() &&
        !Candidates->second.count(AccessOff))
      Found = std::nullopt;
  }
  if (!Found && Dbg && FunctionEntry && FrameBaseOffset) {
    const uint64_t Residue =
        syntheticEntryStackResidue(Opts.TheArch, Opts.Format);
    const int64_t Aligned = static_cast<int64_t>(
        FrameBaseOffset > Residue ? FrameBaseOffset - Residue : 0);
    // MedLLVM aligns FrameSize; PDB S_LOCAL RSP offsets use the unaligned
    // total.  A local already claimed by an address-taken frame slot is not
    // reused for a different offset.
    for (int64_t Delta = 0; !Found && Delta < 16 && Aligned - Delta > 0;
         ++Delta) {
      Found = Accept(Dbg->resolveStackPointerVariable(
          FunctionEntry, Disp + (Aligned - Delta)));
      if (!Found)
        continue;
      const auto Owned = SpLocalOwner.find(Found->StackOffset);
      const uint64_t AccessOff =
          static_cast<uint64_t>(static_cast<int64_t>(FrameBaseOffset) + Disp);
      if (Owned != SpLocalOwner.end() && Owned->second != AccessOff)
        Found = std::nullopt;
    }
  }
  FrameDebugCache[Disp] = Found;
  return Found;
}

const char *argListStandardField(const TypeRef &Ty, uint64_t Rel,
                                 uint16_t AccessSize) {
  if (!Ty || Ty->SourceName != "ArgList" || Ty->IsEnum)
    return nullptr;
  if (AccessSize != 0 && AccessSize != 8)
    return nullptr;
  if (Rel == 0)
    return "types_";
  if (Rel == 8)
    return "values_";
  return nullptr;
}

std::optional<LLVMCWriter::TypedAccess>
LLVMCWriter::frameSlotAccess(const llvm::Value *Ptr, uint16_t AccessSize,
                             bool AddressOf, bool Synthesize,
                             bool Overlay) const {
  if (!Ptr || !SyntheticFrame || Analysis.RawFrameAllocas.count(SyntheticFrame))
    return std::nullopt;
  const auto Peeled = peelPointerOffset(Ptr);
  if (!Peeled || Peeled->first != SyntheticFrame)
    return std::nullopt;
  const uint64_t Off = Peeled->second;
  auto RawTouches = [&](uint64_t Begin, uint64_t Size) {
    for (const auto &[Frame, Offset] : Analysis.RawFrameLocations) {
      if (Frame != SyntheticFrame || Offset < 0)
        continue;
      const uint64_t Raw = static_cast<uint64_t>(Offset);
      if (Raw >= Begin && Raw - Begin < Size)
        return true;
    }
    return false;
  };
  // Projecting only one side of an ambiguous frame-pointer join to a named
  // local would separate its store from the later pointer read. Keep the
  // affected record in the original backing array.
  if (RawTouches(Off, 1))
    return std::nullopt;
  const int64_t Disp =
      static_cast<int64_t>(Off) - static_cast<int64_t>(FrameBaseOffset);

  auto Remember = [&](uint64_t SlotOff, const VariableSym &Var) {
    NamedFrameSlot &Slot = FrameSlots[SlotOff];
    Slot.Off = SlotOff;
    Slot.Name = Var.Name;
    if (Var.Type && (!Slot.Type || Var.Type->FieldDisplayNames.size() >
                                       Slot.Type->FieldDisplayNames.size()))
      Slot.Type = Var.Type;
  };

  const NamedFrameSlot *Owner = nullptr;
  uint64_t Rel = 0;
  if (auto Exact = debugFrameVariable(Disp)) {
    bool WidthOk = true;
    if (AccessSize && !AddressOf && Exact->Type && Exact->Type->Size &&
        Exact->Type->Size != AccessSize) {
      completeDisplayRecord(Dbg, Exact->Type);
      WidthOk = Exact->Type->displayFieldNameAt(0, AccessSize).has_value();
    }
    if (WidthOk) {
      Remember(Off, *Exact);
      Owner = &FrameSlots[Off];
      Rel = 0;
    }
  }
  if (!Owner) {
    for (const auto &[SlotOff, Slot] : FrameSlots) {
      if (SlotOff > Off)
        continue;
      const uint64_t Candidate = Off - SlotOff;
      uint16_t Cover = Slot.Type && Slot.Type->Size ? Slot.Type->Size : 0;
      if (Slot.Type && Slot.Type->SourceName == "ArgList" && Cover < 16)
        Cover = 16;
      if (Cover && Candidate >= Cover)
        continue;
      completeDisplayRecord(Dbg, Slot.Type);
      const bool Hit =
          Candidate == 0 ||
          argListStandardField(Slot.Type, Candidate, AccessSize) ||
          (Slot.Type && Slot.Type->displayFieldPathAt(Candidate, AccessSize));
      if (!Hit)
        continue;
      if (Owner && Owner->Off != SlotOff)
        return std::nullopt;
      Owner = &Slot;
      Rel = Candidate;
    }
  }
  if (!Owner) {
    constexpr uint64_t kMaxInterior = 256;
    for (uint64_t Candidate = 1; Candidate <= kMaxInterior; ++Candidate) {
      auto Var = debugFrameVariable(Disp - static_cast<int64_t>(Candidate));
      if (!Var)
        continue;
      completeDisplayRecord(Dbg, Var->Type);
      if (!Var->Type || (Var->Type->Size && Candidate >= Var->Type->Size))
        continue;
      if (!Var->Type->displayFieldPathAt(Candidate, AccessSize) &&
          !(AccessSize == 0 && Var->Type->displayFieldPathAt(Candidate)))
        continue;
      Remember(Off - Candidate, *Var);
      Owner = &FrameSlots[Off - Candidate];
      Rel = Candidate;
      break;
    }
  }
  if (!Owner && Synthesize && FrameBaseOffset != 0) {
    if (OverlappingFrameAccessOffsets.count(Off))
      return std::nullopt;
    const std::string Syn = syntheticFrameSlotName(Disp);
    if (!Syn.empty() && !isReservedFrameName(Syn)) {
      NamedFrameSlot &Slot = FrameSlots[Off];
      if (Slot.Name.empty()) {
        Slot.Off = Off;
        Slot.Name = Syn;
        if (!Slot.Type && AccessSize && AccessSize <= 8)
          Slot.Type = NdType::makeInt(AccessSize, false);
      }
      Owner = &Slot;
      Rel = 0;
    }
  }
  if (!Owner || Owner->Name.empty())
    return std::nullopt;
  uint64_t Cover = Owner->Type && Owner->Type->Size ? Owner->Type->Size : 8;
  if (Owner->Type && Owner->Type->SourceName == "ArgList" && Cover < 16)
    Cover = 16;
  if (RawTouches(Owner->Off, Cover))
    return std::nullopt;
  if (AddressOf)
    FrameSlots[Owner->Off].AddressTaken = true;
  completeDisplayRecord(Dbg, Owner->Type);
  if (Overlay)
    completeDisplayRecord(Dbg, Owner->CallType);
  const TypeRef Display =
      Overlay && Owner->CallType ? Owner->CallType : Owner->Type;
  const bool ReinterpretRecord =
      Overlay && Owner->CallType && Owner->Type &&
      Display->Kind == NdTypeKind::Struct && !Display->IsEnum &&
      Owner->Type->Kind == NdTypeKind::Struct && !Owner->Type->IsEnum &&
      !Display->SourceName.empty() && !Owner->Type->SourceName.empty() &&
      cNamedTypeSpelling(Display->SourceName) !=
          cNamedTypeSpelling(Owner->Type->SourceName);
  const std::string ViewTag = ReinterpretRecord
                                  ? cNamedTypeSpelling(Display->SourceName)
                                  : std::string();
  auto FieldText = [&](llvm::StringRef Field) {
    if (ReinterpretRecord)
      return "((" + ViewTag + " *)&" + Owner->Name + ")->" + Field.str();
    return Owner->Name + "." + Field.str();
  };
  TypedAccess Out;
  if (Rel == 0) {
    if (AddressOf) {
      Out.Text = "&" + Owner->Name;
      Out.Type = Owner->Type ? NdType::makePtr(Owner->Type) : nullptr;
      return Out;
    }
    if (AccessSize && Owner->Type && Owner->Type->Size &&
        Owner->Type->Size < AccessSize &&
        (!Display || !Display->Size || Display->Size < AccessSize) &&
        (AccessSize == 2 || AccessSize == 4 || AccessSize == 8 ||
         AccessSize == 16)) {
      TypeRef Raw = NdType::makeInt(AccessSize, false);
      Out.Text = "*(" + typeToC(Raw) + " *)&" + Owner->Name;
      Out.Type = Raw;
      return Out;
    }
    if (AccessSize && Display) {
      if (auto Field = Display->displayFieldPathAt(
              0, AccessSize, /*EnterNestedAtZero=*/true)) {
        if (auto Path = canonicalizeDisplayPath(*Field)) {
          Out.Text = FieldText(*Path);
          Out.Type = Display->displayFieldTypeAt(0, AccessSize);
          return Out;
        }
      }
    }
    if (Overlay && Display && AccessSize && AccessSize < 16) {
      if (auto Field = Display->displayFieldNameAt(0)) {
        if (auto Path = canonicalizeDisplayPath(*Field)) {
          Out.Text = FieldText(*Path);
          Out.Type = Display->displayFieldTypeAt(0);
          return Out;
        }
      }
    }
    if (const char *Std = argListStandardField(Display ? Display : Owner->Type,
                                               /*Rel=*/0, AccessSize)) {
      Out.Text = FieldText(Std);
      Out.Type = NdType::makeInt(8, false);
      return Out;
    }
    if (ReinterpretRecord && Display->Size == AccessSize) {
      Out.Text = "*((" + ViewTag + " *)&" + Owner->Name + ")";
      Out.Type = Display;
      return Out;
    }
    // A narrow access to a named record is a partial memory operation, not
    // an assignment of the scalar to the whole record. Keep its exact width
    // when debug info does not identify a field of that width.
    if (Owner->Type && Owner->Type->Kind == NdTypeKind::Struct &&
        !Owner->Type->IsEnum && Owner->Type->Size > AccessSize &&
        (AccessSize == 1 || AccessSize == 2 || AccessSize == 4 ||
         AccessSize == 8)) {
      Out.Text =
          "*(uint" + std::to_string(AccessSize * 8) + "_t *)&" + Owner->Name;
      Out.Type = NdType::makeInt(AccessSize, false);
      return Out;
    }
    Out.Text = Owner->Name;
    Out.Type = Owner->Type;
    return Out;
  }
  const bool EnterNested = !AddressOf;
  auto Field = Display
                   ? Display->displayFieldPathAt(Rel, AccessSize, EnterNested)
                   : std::nullopt;
  if (!Field && AccessSize == 0 && Display)
    Field = Display->displayFieldPathAt(Rel, 0, EnterNested);
  auto Path = Field ? canonicalizeDisplayPath(*Field) : std::nullopt;
  if (!Path && Owner->Type && Display.get() != Owner->Type.get()) {
    Field = Owner->Type->displayFieldPathAt(Rel, AccessSize, EnterNested);
    Path = Field ? canonicalizeDisplayPath(*Field) : std::nullopt;
    if (Path) {
      Out.Text = Owner->Name + "." + *Path;
      Out.Type = Owner->Type->displayFieldTypeAt(Rel, AccessSize);
      if (AddressOf)
        Out.Text = "&" + Out.Text;
      return Out;
    }
  }
  if (!Path) {
    if (const char *Std = argListStandardField(Display ? Display : Owner->Type,
                                               Rel, AccessSize)) {
      Out.Text = FieldText(Std);
      Out.Type = Rel == 8 ? NdType::makePtr(NdType::makeInt(8))
                          : NdType::makeInt(8, false);
      if (AddressOf)
        Out.Text = "&" + Out.Text;
      return Out;
    }
    return std::nullopt;
  }
  Out.Text = FieldText(*Path);
  Out.Type = Display ? Display->displayFieldTypeAt(Rel, AccessSize) : nullptr;
  if (AddressOf)
    Out.Text = "&" + Out.Text;
  return Out;
}

std::optional<std::pair<const llvm::Value *, uint64_t>>
LLVMCWriter::peelPointerOffset(const llvm::Value *V) const {
  if (auto It = Analysis.FramePointerLocations.find(V);
      It != Analysis.FramePointerLocations.end())
    return std::make_pair(It->second.first,
                          static_cast<uint64_t>(It->second.second));
  uint64_t Off = 0;
  llvm::SmallPtrSet<const llvm::Value *, 32> Seen;
  static thread_local int PeelDepth = 0;
  struct DepthGuard {
    int &D;
    explicit DepthGuard(int &D) : D(D) { ++D; }
    ~DepthGuard() { --D; }
  } Guard(PeelDepth);
  const bool Nested = PeelDepth > 1;
  while (V && Seen.size() < 64 && Seen.insert(V).second) {
    if (const auto *I2P = llvm::dyn_cast<llvm::IntToPtrInst>(V)) {
      V = I2P->getOperand(0);
      continue;
    }
    if (const auto *P2I = llvm::dyn_cast<llvm::PtrToIntInst>(V)) {
      V = P2I->getOperand(0);
      continue;
    }
    if (const auto *Cast = llvm::dyn_cast<llvm::CastInst>(V)) {
      V = Cast->getOperand(0);
      continue;
    }
    if (const auto *BO = llvm::dyn_cast<llvm::BinaryOperator>(V)) {
      if (BO->getOpcode() == llvm::Instruction::Add ||
          BO->getOpcode() == llvm::Instruction::Or) {
        const llvm::Value *Base = BO->getOperand(0);
        const auto *CI = llvm::dyn_cast<llvm::ConstantInt>(BO->getOperand(1));
        if (!CI) {
          CI = llvm::dyn_cast<llvm::ConstantInt>(Base);
          Base = BO->getOperand(1);
        }
        if (CI && CI->getValue().getActiveBits() <= 64 &&
            (BO->getOpcode() == llvm::Instruction::Add ||
             orCanPeelAsAdd(Base, CI))) {
          // A narrow address add uses modular source-width arithmetic. A
          // negative displacement (for example, a 32-bit stack push) must be
          // sign-extended while peeling it back to the host-width alloca;
          // zero extension would move the access several gigabytes away.
          Off += static_cast<uint64_t>(CI->getSExtValue());
          V = Base;
          continue;
        }
      }
      if (BO->getOpcode() == llvm::Instruction::Sub) {
        if (const auto *CI =
                llvm::dyn_cast<llvm::ConstantInt>(BO->getOperand(1))) {
          Off -= static_cast<uint64_t>(CI->getSExtValue());
          V = BO->getOperand(0);
          continue;
        }
      }
    }
    if (const auto *GEP = llvm::dyn_cast<llvm::GEPOperator>(V)) {
      llvm::APInt Acc(64, 0);
      if (CurMod &&
          GEP->accumulateConstantOffset(CurMod->getDataLayout(), Acc)) {
        Off += Acc.getZExtValue();
        V = GEP->getPointerOperand();
        continue;
      }
      if (GEP->getNumOperands() == 2) {
        if (const auto *CI =
                llvm::dyn_cast<llvm::ConstantInt>(GEP->getOperand(1))) {
          Off += static_cast<uint64_t>(CI->getSExtValue());
          V = GEP->getPointerOperand();
          continue;
        }
      }
    }
    if (const auto *LI = llvm::dyn_cast<llvm::LoadInst>(V)) {
      const llvm::AllocaInst *Slot = asAllocaPointer(LI->getPointerOperand());
      if (Slot) {
        // A typed record pointer (CNode* cursor) is the field base. Do not
        // walk through it to the bins load that defined it.
        if (isTypedRecordCursorSlot(Slot))
          break;
        // Nested peels come from isThisFieldAddress / isFrameFieldValueHome.
        // Calling those again here recurses on cursor `m_pNext` homes.
        if (!Nested) {
          // A loaded this-field pointer (`this->m_pData.p`) plus a
          // nonzero offset is a nested record access.  Walking through
          // it would add that offset onto `this`.
          if (Off != 0 && isThisFieldValueHome(Slot))
            break;
          if (Off != 0 && isFrameFieldValueHome(Slot))
            break;
        }
        if (const llvm::Value *Stored =
                allocaStoredValueBefore(LI, /*BoundExpansion=*/false);
            Stored && Stored != V) {
          V = Stored;
          continue;
        }
        if (ThisHomes.count(Slot) && DebugThisArg) {
          V = DebugThisArg;
          continue;
        }
      }
      if (Off != 0) {
        if (auto Text = ValueTexts.find(LI);
            Text != ValueTexts.end() && !Text->second.empty()) {
          if (auto Ty = ValueTypes.find(LI);
              Ty != ValueTypes.end() && Ty->second &&
              Ty->second->Kind == NdTypeKind::Ptr && Ty->second->Pointee &&
              Ty->second->Pointee->Kind == NdTypeKind::Struct)
            break;
        }
      }
    }
    break;
  }
  if (!V)
    return std::nullopt;
  return std::make_pair(V, Off);
}

TypeRef LLVMCWriter::typeOfValue(const llvm::Value *V) const {
  if (!V)
    return nullptr;
  if (auto It = ValueTypes.find(V); It != ValueTypes.end())
    return It->second;
  if (const auto *LI = llvm::dyn_cast<llvm::LoadInst>(V)) {
    if (const llvm::AllocaInst *Slot = asAllocaPointer(LI->getPointerOperand()))
      if (auto It = AllocaTypes.find(Slot); It != AllocaTypes.end())
        return It->second;
  }
  const auto Peeled = peelPointerOffset(V);
  if (!Peeled || Peeled->first != DebugThisArg || !DebugThisRecord)
    return nullptr;
  if (Peeled->second == 0 && DebugFn && !DebugFn->Params.empty())
    return DebugFn->Params[0].second;
  completeDisplayRecord(Dbg, DebugThisRecord);
  return DebugThisRecord->displayFieldTypeAt(Peeled->second);
}

std::optional<LLVMCWriter::TypedAccess>
LLVMCWriter::typedRecordAccess(const llvm::Value *Ptr, uint16_t AccessSize,
                               bool EnterNestedAtZero) const {
  if (!Ptr)
    return std::nullopt;
  const auto Peeled = peelPointerOffset(Ptr);
  if (!Peeled)
    return std::nullopt;
  TypeRef Rec;
  std::string BaseName;
  if (Peeled->first == DebugThisArg && DebugThisRecord) {
    Rec = DebugThisRecord;
    auto Name = ValNames.find(DebugThisArg);
    if (Name == ValNames.end() || Name->second.empty())
      return std::nullopt;
    BaseName = Name->second;
  } else if (const auto *LI = llvm::dyn_cast<llvm::LoadInst>(Peeled->first)) {
    const llvm::AllocaInst *Slot = asAllocaPointer(LI->getPointerOperand());
    if (Slot) {
      auto TyIt = AllocaTypes.find(Slot);
      if (TyIt == AllocaTypes.end() || !TyIt->second ||
          TyIt->second->Kind != NdTypeKind::Ptr || !TyIt->second->Pointee ||
          TyIt->second->Pointee->Kind != NdTypeKind::Struct ||
          TyIt->second->Pointee->SourceName.empty())
        return std::nullopt;
      Rec = TyIt->second->Pointee;
      if (isThisFieldValueHome(Slot) || isFrameFieldValueHome(Slot)) {
        if (auto Text = AllocaTexts.find(Slot);
            Text != AllocaTexts.end() && !Text->second.empty())
          BaseName = Text->second;
      }
      if (BaseName.empty()) {
        auto Name = ValNames.find(Slot);
        if (Name == ValNames.end() || Name->second.empty())
          return std::nullopt;
        BaseName = Name->second;
      }
    } else {
      auto TyIt = ValueTypes.find(LI);
      auto Text = ValueTexts.find(LI);
      if (TyIt == ValueTypes.end() || !TyIt->second ||
          TyIt->second->Kind != NdTypeKind::Ptr || !TyIt->second->Pointee ||
          TyIt->second->Pointee->Kind != NdTypeKind::Struct ||
          TyIt->second->Pointee->SourceName.empty() ||
          Text == ValueTexts.end() || Text->second.empty())
        return std::nullopt;
      Rec = TyIt->second->Pointee;
      BaseName = Text->second;
    }
  } else
    return std::nullopt;
  completeDisplayRecord(Dbg, Rec);
  auto Field =
      Rec->displayFieldPathAt(Peeled->second, AccessSize, EnterNestedAtZero);
  if (!Field)
    return std::nullopt;
  auto Path = canonicalizeDisplayPath(*Field);
  if (!Path)
    return std::nullopt;
  TypedAccess Out;
  Out.Text = BaseName + "->" + *Path;
  Out.Type = Rec->displayFieldTypeAt(Peeled->second, AccessSize);
  return Out;
}

namespace {
const llvm::Value *peelMulScale(
    const llvm::Value *V, uint64_t Scale,
    const std::function<const llvm::Value *(const llvm::LoadInst *)> &StoredOf,
    const std::function<const llvm::AllocaInst *(const llvm::Value *)> &AsSlot,
    const std::function<bool(const llvm::Value *)> &NormalizedBoolean) {
  if (!V || !Scale)
    return nullptr;
  std::set<const llvm::Value *> Seen;
  while (V && Seen.size() < 64 && Seen.insert(V).second) {
    if (const auto *Cast = llvm::dyn_cast<llvm::CastInst>(V)) {
      if (const llvm::Value *Inner = losslessIntegerView(
              Cast, NormalizedBoolean(Cast->getOperand(0)))) {
        V = Inner;
        continue;
      }
    }
    if (const auto *LI = llvm::dyn_cast<llvm::LoadInst>(V)) {
      if (const llvm::AllocaInst *Slot = AsSlot(LI->getPointerOperand())) {
        if (const llvm::Value *Stored = StoredOf(LI); Stored && Stored != V) {
          V = Stored;
          continue;
        }
      }
    }
    break;
  }
  const auto *BO = llvm::dyn_cast<llvm::BinaryOperator>(V);
  if (!BO)
    return nullptr;
  if (BO->getOpcode() == llvm::Instruction::Mul) {
    if (const auto *CI = llvm::dyn_cast<llvm::ConstantInt>(BO->getOperand(1)))
      if (CI->getZExtValue() == Scale)
        return BO->getOperand(0);
    if (const auto *CI = llvm::dyn_cast<llvm::ConstantInt>(BO->getOperand(0)))
      if (CI->getZExtValue() == Scale)
        return BO->getOperand(1);
  }
  if (BO->getOpcode() == llvm::Instruction::Shl)
    if (const auto *CI = llvm::dyn_cast<llvm::ConstantInt>(BO->getOperand(1)))
      if (CI->getZExtValue() < 64 && (1ull << CI->getZExtValue()) == Scale)
        return BO->getOperand(0);
  return nullptr;
}
} // namespace

std::optional<LLVMCWriter::TypedAccess>
LLVMCWriter::typedIndexAccess(const llvm::Value *Ptr) {
  if (!Ptr)
    return std::nullopt;
  const llvm::Value *Addr = Ptr;
  std::set<const llvm::Value *> Seen;
  while (Addr && Seen.size() < 64 && Seen.insert(Addr).second) {
    if (const auto *I2P = llvm::dyn_cast<llvm::IntToPtrInst>(Addr)) {
      Addr = I2P->getOperand(0);
      continue;
    }
    if (const auto *LI = llvm::dyn_cast<llvm::LoadInst>(Addr)) {
      if (const llvm::AllocaInst *Slot =
              asAllocaPointer(LI->getPointerOperand())) {
        if (const auto *Stored =
                allocaStoredValueBefore(LI, /*BoundExpansion=*/false);
            Stored && Stored != Addr) {
          Addr = Stored;
          continue;
        }
      }
    }
    break;
  }
  const auto *Add = llvm::dyn_cast<llvm::BinaryOperator>(Addr);
  if (!Add || Add->getOpcode() != llvm::Instruction::Add)
    return std::nullopt;
  const llvm::Value *Base = nullptr;
  const llvm::Value *Index = nullptr;
  auto AsSlot = [this](const llvm::Value *V) { return asAllocaPointer(V); };
  auto StoredOf = [this](const llvm::LoadInst *LI) {
    return allocaStoredValueBefore(LI, /*BoundExpansion=*/false);
  };
  auto NormalizedBoolean = [this](const llvm::Value *V) {
    return isNormalizedBoolean(V);
  };
  if ((Index = peelMulScale(Add->getOperand(1), 8, StoredOf, AsSlot,
                            NormalizedBoolean)))
    Base = Add->getOperand(0);
  else if ((Index = peelMulScale(Add->getOperand(0), 8, StoredOf, AsSlot,
                                 NormalizedBoolean)))
    Base = Add->getOperand(1);
  if (!Base || !Index)
    return std::nullopt;
  TypeRef BaseTy = typeOfValue(Base);
  if (!BaseTy || BaseTy->Kind != NdTypeKind::Ptr || !BaseTy->Pointee ||
      BaseTy->Pointee->Kind != NdTypeKind::Ptr)
    return std::nullopt;
  TypeRef Elem = BaseTy->Pointee;
  if (!Elem->Pointee || Elem->Pointee->Kind != NdTypeKind::Struct ||
      Elem->Pointee->SourceName.empty())
    return std::nullopt;
  completeDisplayRecord(Dbg, Elem);
  completeDisplayRecord(Dbg, Elem->Pointee);
  std::string BaseName;
  if (auto Acc = typedRecordAccess(Base, 8))
    BaseName = Acc->Text;
  else if (const auto *LI = llvm::dyn_cast<llvm::LoadInst>(Base)) {
    if (const llvm::AllocaInst *Slot =
            asAllocaPointer(LI->getPointerOperand())) {
      if (auto Text = AllocaTexts.find(Slot); Text != AllocaTexts.end())
        BaseName = Text->second;
      else if (auto Name = ValNames.find(Slot); Name != ValNames.end())
        BaseName = Name->second;
    }
  }
  if (BaseName.empty())
    return std::nullopt;
  std::string Idx = indexExprStr(Index);
  if (Idx.empty())
    return std::nullopt;
  TypedAccess Out;
  Out.Text = BaseName + "[" + Idx + "]";
  Out.Type = std::move(Elem);
  return Out;
}

std::string LLVMCWriter::indexExprStr(const llvm::Value *V) {
  if (!V)
    return {};
  if (auto Imm = foldImmediate(V))
    return *Imm;
  std::set<const llvm::Value *> Seen;
  while (V && Seen.insert(V).second) {
    if (const auto *Load = llvm::dyn_cast<llvm::LoadInst>(V))
      if (!Load->isSimple())
        return getName(Load);
    if (const auto *Cast = llvm::dyn_cast<llvm::CastInst>(V)) {
      if (const llvm::Value *Inner = peelIntegerView(Cast); Inner != Cast) {
        V = Inner;
        continue;
      }
    }
    if (const auto *LI = llvm::dyn_cast<llvm::LoadInst>(V)) {
      if (const llvm::AllocaInst *Slot =
              asAllocaPointer(LI->getPointerOperand())) {
        if (const llvm::Value *Stored = allocaStoredValueBefore(LI);
            Stored && Stored != V) {
          V = Stored;
          continue;
        }
        if (auto Text = AllocaTexts.find(Slot);
            Text != AllocaTexts.end() && !Text->second.empty())
          return Text->second;
      }
    }
    break;
  }
  if (!V)
    return {};
  if (const auto *Cast = llvm::dyn_cast<llvm::CastInst>(V)) {
    if (llvm::isa<llvm::TruncInst, llvm::ZExtInst, llvm::SExtInst>(Cast)) {
      const std::string Inner = indexExprStr(Cast->getOperand(0));
      if (!Inner.empty())
        return castStr(Cast->getOpcode(), Inner, Cast->getSrcTy(),
                       Cast->getDestTy());
    }
  }
  if (const auto *BO = llvm::dyn_cast<llvm::BinaryOperator>(V)) {
    if (BO->getOpcode() == llvm::Instruction::URem ||
        BO->getOpcode() == llvm::Instruction::SRem) {
      const std::string LHS = indexExprStr(BO->getOperand(0));
      const std::string RHS = indexExprStr(BO->getOperand(1));
      if (!LHS.empty() && !RHS.empty()) {
        if (BO->getOpcode() == llvm::Instruction::URem) {
          const std::string Type = typeToCLLVM(BO->getType());
          return "((" + Type + ")(" + LHS + ") % (" + Type + ")(" + RHS + "))";
        }
        return "(" + signedIntegerOperand(BO->getOperand(0), LHS) + " % " +
               signedIntegerOperand(BO->getOperand(1), RHS) + ")";
      }
      // A composed remainder must contain both operands. Returning its own
      // local name here would turn its defining statement into x = x and
      // incorrectly classify it as a reprintable expression.
      return {};
    }
  }
  if (const auto *LI = llvm::dyn_cast<llvm::LoadInst>(V)) {
    if (auto Acc = typedRecordAccess(LI->getPointerOperand(),
                                     llvmAccessSize(LI->getType())))
      return Acc->Text;
  }
  if (auto Acc = typedRecordAccess(V, 4))
    return Acc->Text;
  if (const auto *Arg = llvm::dyn_cast<llvm::Argument>(V)) {
    if (auto Name = ValNames.find(Arg); Name != ValNames.end())
      return Name->second;
    return "arg" + std::to_string(Arg->getArgNo());
  }
  if (const auto *I = llvm::dyn_cast<llvm::Instruction>(V)) {
    if (!Analysis.Inlinable.count(I)) {
      if (const auto *LI = llvm::dyn_cast<llvm::LoadInst>(I);
          LI && computedAllocaLoadIsForwarded(LI))
        return {};
      if (auto Name = ValNames.find(V); Name != ValNames.end())
        return Name->second;
    }
  }
  return {};
}

std::string LLVMCWriter::ultimateComposedText(const llvm::Value *V) {
  std::set<const llvm::Value *> Seen;
  while (V && Seen.insert(V).second) {
    if (const auto *Load = llvm::dyn_cast<llvm::LoadInst>(V))
      if (!Load->isSimple())
        return getName(Load);
    if (auto Text = ValueTexts.find(V);
        Text != ValueTexts.end() && !Text->second.empty())
      return Text->second;
    if (const auto *LI = llvm::dyn_cast<llvm::LoadInst>(V)) {
      if (auto Acc = typedRecordAccess(LI->getPointerOperand(),
                                       llvmAccessSize(LI->getType())))
        return Acc->Text;
      if (auto Acc = typedIndexAccess(LI->getPointerOperand()))
        return Acc->Text;
      if (const llvm::Value *Stored = allocaStoredValueBefore(LI);
          Stored && Stored != V) {
        V = Stored;
        continue;
      }
    }
    if (isComposedRemValue(V))
      return indexExprStr(V);
    break;
  }
  return {};
}

const llvm::Value *LLVMCWriter::jleZeroCore(const llvm::BinaryOperator *BO) {
  if (!BO || BO->getOpcode() != llvm::Instruction::Or)
    return nullptr;
  const llvm::Value *LHS = peelIntegerView(BO->getOperand(0));
  const llvm::Value *RHS = peelIntegerView(BO->getOperand(1));
  const auto *LCmp = llvm::dyn_cast<llvm::ICmpInst>(LHS);
  const auto *RCmp = llvm::dyn_cast<llvm::ICmpInst>(RHS);
  if (!LCmp || !RCmp)
    return nullptr;
  auto zeroSide = [](const llvm::ICmpInst *Cmp) -> const llvm::Value * {
    if (const auto *CI = llvm::dyn_cast<llvm::ConstantInt>(Cmp->getOperand(1));
        CI && CI->isZero())
      return Cmp->getOperand(0);
    if (const auto *CI = llvm::dyn_cast<llvm::ConstantInt>(Cmp->getOperand(0));
        CI && CI->isZero())
      return Cmp->getOperand(1);
    return nullptr;
  };
  auto unwrapFlag = [&](const llvm::ICmpInst *Cmp) -> const llvm::ICmpInst * {
    if (Cmp->getPredicate() != llvm::CmpInst::ICMP_NE &&
        Cmp->getPredicate() != llvm::CmpInst::ICMP_EQ)
      return Cmp;
    const llvm::Value *Tested = zeroSide(Cmp);
    if (!Tested)
      return Cmp;
    if (const auto *Inner =
            llvm::dyn_cast<llvm::ICmpInst>(peelIntegerView(Tested)))
      // NE zero preserves a Boolean flag; EQ zero negates it. This matcher
      // recognizes the positive equality/sign disjunction only.
      return Cmp->getPredicate() == llvm::CmpInst::ICMP_NE ? Inner : nullptr;
    return Cmp;
  };
  const llvm::ICmpInst *Eq = nullptr;
  const llvm::ICmpInst *Slt = nullptr;
  auto Take = [&](const llvm::ICmpInst *Cmp) {
    Cmp = unwrapFlag(Cmp);
    if (!Cmp)
      return;
    if (Cmp->getPredicate() == llvm::CmpInst::ICMP_EQ)
      Eq = Cmp;
    else if (Cmp->getPredicate() == llvm::CmpInst::ICMP_SLT)
      Slt = Cmp;
  };
  Take(LCmp);
  Take(RCmp);
  if (!Eq || !Slt)
    return nullptr;
  auto core = [&](const llvm::Value *Val) -> const llvm::Value * {
    Val = peelIntegerView(Val);
    if (const auto *And = llvm::dyn_cast<llvm::BinaryOperator>(Val);
        And && And->getOpcode() == llvm::Instruction::And) {
      const llvm::Value *A = peelIntegerView(And->getOperand(0));
      const llvm::Value *B = peelIntegerView(And->getOperand(1));
      if (A && A == B)
        return A;
    }
    return Val;
  };
  const llvm::Value *L = zeroSide(Eq);
  const llvm::Value *R = zeroSide(Slt);
  if (!L || !R || Slt->getOperand(0) != R)
    return nullptr;
  const llvm::Value *Core = core(L);
  // A zero-extended Boolean is nonnegative in its comparison width, even
  // though interpreting its original i1 as signed makes true equal -1.
  // The compact signed comparison must keep both original operand widths.
  if (!Core || Core != core(R) || Core->getType() != L->getType() ||
      Core->getType() != R->getType())
    return nullptr;
  return Core;
}

std::optional<std::string>
LLVMCWriter::invertedRelationalText(const llvm::Value *V) {
  auto Text = invertedRelationalTextImpl(V);
  const auto *I = llvm::dyn_cast_or_null<llvm::Instruction>(V);
  if (Text && SourceRecorder && I)
    *Text = SourceRecorder->expression(*I, std::move(*Text));
  return Text;
}

std::optional<std::string>
LLVMCWriter::invertedRelationalTextImpl(const llvm::Value *V) {
  std::set<const llvm::Value *> Seen;
  while (V && Seen.insert(V).second) {
    if (const auto *LI = llvm::dyn_cast<llvm::LoadInst>(V)) {
      if (const llvm::Value *Stored = allocaStoredValueBefore(LI);
          Stored && Stored != V) {
        V = Stored;
        continue;
      }
    }
    if (const auto *Cast = llvm::dyn_cast<llvm::CastInst>(V)) {
      if (const llvm::Value *Inner = peelIntegerView(Cast); Inner != Cast) {
        V = Inner;
        continue;
      }
    }
    if (const auto *BO = llvm::dyn_cast<llvm::BinaryOperator>(V)) {
      if (BO->getOpcode() == llvm::Instruction::Or)
        if (const llvm::Value *Core = jleZeroCore(BO))
          return signedIntegerOperand(Core, valueStr(Core)) + " > 0";
      return std::nullopt;
    }
    if (const auto *ICmp = llvm::dyn_cast<llvm::ICmpInst>(V)) {
      const llvm::Value *LHS = ICmp->getOperand(0);
      const llvm::Value *RHS = ICmp->getOperand(1);
      const auto *RC = llvm::dyn_cast<llvm::ConstantInt>(RHS);
      const auto *LC = llvm::dyn_cast<llvm::ConstantInt>(LHS);
      const llvm::Value *Inner = nullptr;
      const bool Ne = ICmp->getPredicate() == llvm::CmpInst::ICMP_NE;
      const bool Eq = ICmp->getPredicate() == llvm::CmpInst::ICMP_EQ;
      if (RC && RC->isZero())
        Inner = LHS;
      else if (LC && LC->isZero())
        Inner = RHS;
      if (Inner && (Ne || Eq)) {
        if (integerComparisonNeedsNormalization(Inner->getType()) &&
            !isNormalizedBoolean(Inner))
          return icmpInlineText(*ICmp, /*Invert=*/true);
        const llvm::Value *Shown = peelIntegerView(Inner);
        if (!Shown)
          Shown = Inner;
        if (Ne) {
          V = Shown;
          continue;
        }
        return std::nullopt;
      }
      return icmpInlineText(*ICmp, /*Invert=*/true);
    }
    break;
  }
  return std::nullopt;
}

std::string LLVMCWriter::condStr(const llvm::Value *V) {
  auto Text = condStrImpl(V);
  const auto *I = llvm::dyn_cast_or_null<llvm::Instruction>(V);
  // Branch structuring can remove an outer Boolean negation. Leave its
  // delimiters outside the event so that private markers do not change the
  // ordinary simplification or get sliced by it.
  if (SourceRecorder && I && llvm::StringRef(Text).starts_with("!(") &&
      Text.back() == ')')
    return "!(" +
           SourceRecorder->expression(*I, Text.substr(2, Text.size() - 3)) +
           ")";
  return SourceRecorder && I ? SourceRecorder->expression(*I, std::move(Text))
                             : Text;
}

std::string LLVMCWriter::condStrImpl(const llvm::Value *V) {
  if (auto Text = scalarConditionText(V))
    return *Text;
  std::set<const llvm::Value *> Seen;
  while (V && Seen.insert(V).second) {
    if (const auto *LI = llvm::dyn_cast<llvm::LoadInst>(V)) {
      if (const llvm::Value *Stored = allocaStoredValueBefore(LI);
          Stored && Stored != V) {
        V = Stored;
        continue;
      }
    }
    if (const auto *Cast = llvm::dyn_cast<llvm::CastInst>(V)) {
      if (const llvm::Value *Inner = peelIntegerView(Cast); Inner != Cast) {
        V = Inner;
        continue;
      }
    }
    if (const auto *BO = llvm::dyn_cast<llvm::BinaryOperator>(V)) {
      if (BO->getOpcode() == llvm::Instruction::Or ||
          BO->getOpcode() == llvm::Instruction::And) {
        const llvm::Value *LHS = peelIntegerView(BO->getOperand(0));
        const llvm::Value *RHS = peelIntegerView(BO->getOperand(1));
        if (BO->getOpcode() == llvm::Instruction::And) {
          auto NegatedCompare = [&](const llvm::Value *Value) {
            const auto *Outer = llvm::dyn_cast<llvm::ICmpInst>(Value);
            if (!Outer || Outer->getPredicate() != llvm::CmpInst::ICMP_EQ)
              return static_cast<const llvm::ICmpInst *>(nullptr);
            for (unsigned I = 0; I != 2; ++I)
              if (const auto *Zero =
                      llvm::dyn_cast<llvm::ConstantInt>(Outer->getOperand(I));
                  Zero && Zero->isZero() &&
                  isNormalizedBoolean(Outer->getOperand(1 - I)))
                return llvm::dyn_cast<llvm::ICmpInst>(
                    peelIntegerView(Outer->getOperand(1 - I)));
            return static_cast<const llvm::ICmpInst *>(nullptr);
          };
          const auto *Less = NegatedCompare(LHS);
          const auto *Equal = NegatedCompare(RHS);
          if (Less && Equal && Less->getPredicate() == llvm::CmpInst::ICMP_EQ)
            std::swap(Less, Equal);
          if (Less && Equal &&
              Less->getPredicate() == llvm::CmpInst::ICMP_ULT &&
              Equal->getPredicate() == llvm::CmpInst::ICMP_EQ &&
              Less->getOperand(0)->getType()->isIntegerTy()) {
            const auto *A = Less->getOperand(0);
            const auto *B = Less->getOperand(1);
            bool SameOperands =
                (Equal->getOperand(0) == A && Equal->getOperand(1) == B) ||
                (Equal->getOperand(0) == B && Equal->getOperand(1) == A);
            for (unsigned I = 0; I != 2; ++I) {
              const auto *Zero =
                  llvm::dyn_cast<llvm::ConstantInt>(Equal->getOperand(I));
              const auto *Sub = llvm::dyn_cast<llvm::BinaryOperator>(
                  Equal->getOperand(1 - I));
              if (Zero && Zero->isZero() && Sub &&
                  Sub->getOpcode() == llvm::Instruction::Sub &&
                  Sub->getType() == A->getType())
                SameOperands |=
                    (Sub->getOperand(0) == A && Sub->getOperand(1) == B) ||
                    (Sub->getOperand(0) == B && Sub->getOperand(1) == A);
            }
            // Only !ULT && !EQ over the same-width values proves UGT. In
            // particular, ULT && !EQ and mixed-width subtractions do not.
            if (SameOperands)
              return unsignedCompareOperand(A, valueStr(A)) + " > " +
                     unsignedCompareOperand(B, valueStr(B));
          }
        }
        // Keep the Boolean operands' actual polarity. Recognizing only the
        // underlying ULT and subtraction loses negations and can turn an
        // impossible conjunction into a greater-than comparison.
        if (BO->getOpcode() == llvm::Instruction::Or)
          if (const llvm::Value *Core = jleZeroCore(BO))
            return signedIntegerOperand(Core, valueStr(Core)) + " <= 0";
        if (llvm::isa<llvm::ICmpInst>(LHS) && llvm::isa<llvm::ICmpInst>(RHS)) {
          const char *Op =
              BO->getOpcode() == llvm::Instruction::Or ? " || " : " && ";
          auto Piece = [&](const llvm::Value *Val) {
            const auto *Cmp = llvm::cast<llvm::ICmpInst>(Val);
            if (Analysis.Inlinable.count(Cmp))
              return icmpInlineText(*Cmp);
            return valueStr(Val);
          };
          return Piece(LHS) + Op + Piece(RHS);
        }
      }
    }
    if (const auto *ICmp = llvm::dyn_cast<llvm::ICmpInst>(V)) {
      const llvm::Value *LHS = ICmp->getOperand(0);
      const llvm::Value *RHS = ICmp->getOperand(1);
      const auto *RC = llvm::dyn_cast<llvm::ConstantInt>(RHS);
      const auto *LC = llvm::dyn_cast<llvm::ConstantInt>(LHS);
      const llvm::Value *Inner = nullptr;
      const bool Ne = ICmp->getPredicate() == llvm::CmpInst::ICMP_NE;
      const bool Eq = ICmp->getPredicate() == llvm::CmpInst::ICMP_EQ;
      if (RC && RC->isZero())
        Inner = LHS;
      else if (LC && LC->isZero())
        Inner = RHS;
      if (Inner && (Ne || Eq)) {
        if (integerComparisonNeedsNormalization(Inner->getType()) &&
            !isNormalizedBoolean(Inner))
          return icmpInlineText(*ICmp);
        const llvm::Value *Shown = peelIntegerView(Inner);
        if (!Shown)
          Shown = Inner;
        if (Ne) {
          V = Shown;
          continue;
        }
        // Reuse the inverted condition owner before expanding a boolean
        // flag tree; the tree can contain repeated views of one call.
        if (auto Inverted = invertedRelationalText(Shown))
          return *Inverted;
        return "!(" + valueStr(Shown) + ")";
      }
      if (Analysis.Inlinable.count(ICmp))
        return icmpInlineText(*ICmp);
    }
    break;
  }
  return valueStr(V);
}

bool LLVMCWriter::isComposedRemValue(const llvm::Value *V) {
  if (!V)
    return false;
  std::set<const llvm::Value *> Seen;
  while (V && Seen.insert(V).second) {
    if (const auto *Cast = llvm::dyn_cast<llvm::CastInst>(V)) {
      if (const llvm::Value *Inner = peelIntegerView(Cast); Inner != Cast) {
        V = Inner;
        continue;
      }
    }
    if (const auto *LI = llvm::dyn_cast<llvm::LoadInst>(V)) {
      if (const llvm::Value *Stored = allocaStoredValueBefore(LI);
          Stored && Stored != V) {
        V = Stored;
        continue;
      }
    }
    break;
  }
  const auto *BO = llvm::dyn_cast<llvm::BinaryOperator>(V);
  return BO &&
         (BO->getOpcode() == llvm::Instruction::URem ||
          BO->getOpcode() == llvm::Instruction::SRem) &&
         !indexExprStr(BO).empty();
}

std::string LLVMCWriter::indirectCalleeStr(const llvm::Value *Callee,
                                           bool MarkChain) {
  if (!Callee)
    return {};
  std::vector<const llvm::Instruction *> Chain;
  auto Mark = [&](const llvm::Value *V) {
    if (const auto *I = llvm::dyn_cast<llvm::Instruction>(V))
      Chain.push_back(I);
    if (!V)
      return;
    for (const llvm::User *U : V->users()) {
      const auto *SI = llvm::dyn_cast<llvm::StoreInst>(U);
      if (SI && SI->getValueOperand() == V)
        Chain.push_back(SI);
    }
  };
  std::set<const llvm::Value *> Seen;
  const llvm::Value *V = Callee;
  auto PeelCast = [&](const llvm::Value *Cur, bool HideCasts) {
    std::set<const llvm::Value *> CastSeen;
    while (Cur && CastSeen.insert(Cur).second) {
      if (const auto *I2P = llvm::dyn_cast<llvm::IntToPtrInst>(Cur)) {
        if (HideCasts)
          Mark(Cur);
        Cur = I2P->getOperand(0);
        continue;
      }
      if (const auto *P2I = llvm::dyn_cast<llvm::PtrToIntInst>(Cur)) {
        if (HideCasts)
          Mark(Cur);
        Cur = P2I->getOperand(0);
        continue;
      }
      if (const auto *Cast = llvm::dyn_cast<llvm::CastInst>(Cur)) {
        if (HideCasts)
          Mark(Cur);
        Cur = Cast->getOperand(0);
        continue;
      }
      if (const auto *Fr = llvm::dyn_cast<llvm::FreezeInst>(Cur)) {
        if (HideCasts)
          Mark(Cur);
        Cur = Fr->getOperand(0);
        continue;
      }
      break;
    }
    return Cur;
  };
  while (V && Seen.insert(V).second) {
    V = PeelCast(V, /*HideCasts=*/true);
    if (!V)
      return {};
    if (const auto *LI = llvm::dyn_cast<llvm::LoadInst>(V)) {
      if (const llvm::AllocaInst *Slot =
              asAllocaPointer(LI->getPointerOperand())) {
        if (const llvm::Value *Stored = allocaStoredValueBefore(LI);
            Stored && Stored != V && Seen.count(Stored) == 0) {
          Mark(LI);
          V = Stored;
          continue;
        }
      }
      break;
    }
    break;
  }
  const auto *SlotLI = llvm::dyn_cast<llvm::LoadInst>(V);
  if (!SlotLI)
    return {};
  Mark(SlotLI);
  const llvm::Value *Addr = PeelCast(SlotLI->getPointerOperand(),
                                     /*HideCasts=*/true);
  const auto *BO = llvm::dyn_cast<llvm::BinaryOperator>(Addr);
  if (!BO || (BO->getOpcode() != llvm::Instruction::Add &&
              BO->getOpcode() != llvm::Instruction::Or)) {
    if (const auto *LI = llvm::dyn_cast<llvm::LoadInst>(Addr)) {
      if (const llvm::AllocaInst *Slot =
              asAllocaPointer(LI->getPointerOperand())) {
        if (const llvm::Value *Stored = allocaStoredValueBefore(LI)) {
          Mark(LI);
          Addr = PeelCast(Stored, /*HideCasts=*/true);
        }
      }
    }
    BO = llvm::dyn_cast<llvm::BinaryOperator>(Addr);
  }
  uint64_t Off = 0;
  const llvm::Value *Vtbl = nullptr;
  if (BO && (BO->getOpcode() == llvm::Instruction::Add ||
             BO->getOpcode() == llvm::Instruction::Or)) {
    Mark(BO);
    if (const auto *CI = llvm::dyn_cast<llvm::ConstantInt>(BO->getOperand(1))) {
      Off = CI->getZExtValue();
      Vtbl = BO->getOperand(0);
    } else if (const auto *CI =
                   llvm::dyn_cast<llvm::ConstantInt>(BO->getOperand(0))) {
      Off = CI->getZExtValue();
      Vtbl = BO->getOperand(1);
    }
    if (!Vtbl || Off == 0)
      return {};
    Vtbl = PeelCast(Vtbl, /*HideCasts=*/true);
    if (const auto *LI = llvm::dyn_cast<llvm::LoadInst>(Vtbl)) {
      if (const llvm::AllocaInst *Slot =
              asAllocaPointer(LI->getPointerOperand()))
        if (const llvm::Value *Stored = allocaStoredValueBefore(LI); Stored) {
          Mark(LI);
          Vtbl = PeelCast(Stored, /*HideCasts=*/true);
        }
    }
  } else if (const auto *LI = llvm::dyn_cast<llvm::LoadInst>(Addr)) {
    Mark(LI);
    Vtbl = LI;
    Off = 0;
  } else
    return {};
  const auto *VtblLI = llvm::dyn_cast<llvm::LoadInst>(Vtbl);
  if (!VtblLI)
    return {};
  Mark(VtblLI);
  const llvm::Value *Obj = PeelCast(VtblLI->getPointerOperand(),
                                    /*HideCasts=*/false);
  std::string ObjStr;
  if (const auto *ObjLI = llvm::dyn_cast<llvm::LoadInst>(Obj)) {
    if (auto Text = ValueTexts.find(ObjLI);
        Text != ValueTexts.end() && !Text->second.empty())
      ObjStr = Text->second;
    else if (const llvm::AllocaInst *Slot =
                 asAllocaPointer(ObjLI->getPointerOperand())) {
      if (auto Text = AllocaTexts.find(Slot);
          Text != AllocaTexts.end() && !Text->second.empty())
        ObjStr = Text->second;
      else if (auto Home = AllocaHomeValues.find(Slot);
               Home != AllocaHomeValues.end() && Home->second)
        ObjStr = valueStr(Home->second);
    }
    if (ObjStr.empty())
      if (auto Acc = typedRecordAccess(ObjLI->getPointerOperand(),
                                       llvmAccessSize(ObjLI->getType())))
        ObjStr = Acc->Text;
  }
  if (ObjStr.empty())
    if (auto Acc = typedRecordAccess(Obj, 8))
      ObjStr = Acc->Text;
  if (ObjStr.empty()) {
    if (auto Text = ValueTexts.find(Obj);
        Text != ValueTexts.end() && !Text->second.empty())
      ObjStr = Text->second;
    else
      ObjStr = valueStr(Obj);
  }
  if (ObjStr.empty())
    return {};
  if (MarkChain)
    for (const llvm::Instruction *I : Chain)
      if (!MaterializedExpressions.count(I))
        Analysis.Inlinable.insert(I);
  std::string Text = Off == 0 ? "(**(void ***)(" + ObjStr + "))"
                              : "(*(void **)((uintptr_t)(*(void **)(" + ObjStr +
                                    ")) + " + std::to_string(Off) + "))";
  if (SourceRecorder && !MarkChain)
    for (const auto *I : Chain)
      Text = SourceRecorder->expression(*I, std::move(Text));
  return Text;
}

void LLVMCWriter::markIndirectCalleeChains(llvm::Function &Fn) {
  for (auto &BB : Fn)
    for (auto &Inst : BB)
      if (const auto *CB = llvm::dyn_cast<llvm::CallBase>(&Inst))
        (void)indirectCalleeStr(CB->getCalledOperand(), /*MarkChain=*/true);
}

std::string LLVMCWriter::valueStr(const llvm::Value *V, bool *PointerSpelling) {
  if (PointerSpelling)
    *PointerSpelling = false;
  if (UseScalarExpressionTypes)
    if (auto Text = scalarExpressionText(V))
      return *Text;
  if (MaterializedExpressions.count(V)) {
    if (PointerSpelling)
      *PointerSpelling = V->getType()->isPointerTy();
    return getName(V);
  }
  auto AddressText = [&](std::string Text) {
    if (PointerSpelling)
      *PointerSpelling = true;
    return Text;
  };
  if (const auto *Load = llvm::dyn_cast<llvm::LoadInst>(V))
    if (!Load->isSimple())
      return getName(Load);
  if (const auto *LI = llvm::dyn_cast<llvm::LoadInst>(V)) {
    if (auto It = SelectUseText.find(LI); It != SelectUseText.end())
      return It->second;
  }
  if (isCallClobberValue(V))
    return "0 /* unknown */";
  if (llvm::isa<llvm::FreezeInst>(V))
    return getName(V);
  if (auto Imm = foldImmediate(V))
    return *Imm;
  // A scalar FP bitcast is materialized with a byte copy. Display provenance
  // may name its integer input, but that name cannot replace the float value.
  if (const auto *Inst = llvm::dyn_cast<llvm::Instruction>(V);
      Inst && isFloatingPointBitcast(*Inst))
    return getName(V);
  if (const auto *Cast = llvm::dyn_cast<llvm::CastInst>(V);
      Cast &&
      llvm::isa<llvm::TruncInst, llvm::ZExtInst, llvm::SExtInst>(Cast)) {
    if (const llvm::Value *Inner =
            losslessIntegerView(Cast, isNormalizedBoolean(Cast->getOperand(0)));
        Inner && Inner->getType() == Cast->getType())
      return valueStr(Inner, PointerSpelling);
    // Typed/composed display names can describe the operand, but cannot
    // replace a width-changing value. Keep materialized casts single-use
    // here so an inlined producer is not evaluated again for every use.
    return Analysis.Inlinable.count(Cast) ? renderInline(*Cast) : getName(Cast);
  }
  if (const auto *CB = llvm::dyn_cast<llvm::CallBase>(V))
    if (std::string Addr = ctorThisAddress(*CB); !Addr.empty())
      return AddressText(std::move(Addr));
  if (auto VA = imageDataVA(V))
    if (auto Lit = imageStringLiteral(Img, *VA, /*AllowEmpty=*/true))
      return AddressText(*Lit);
  if (!llvm::isa<llvm::LoadInst>(V)) {
    if (auto Acc = frameSlotAccess(V, 0, /*AddressOf=*/true))
      return AddressText(Acc->Text);
    if (auto Acc = frameSlotAccess(V, 8, /*AddressOf=*/true))
      return AddressText(Acc->Text);
    if (auto Acc = typedRecordAccess(V, 8, /*EnterNestedAtZero=*/false))
      return AddressText("&" + Acc->Text);
    if (auto Acc = typedRecordAccess(V, 4, /*EnterNestedAtZero=*/false))
      return AddressText("&" + Acc->Text);
  }
  if (const auto *LI = llvm::dyn_cast<llvm::LoadInst>(V)) {
    if (auto Acc = frameSlotAccess(LI->getPointerOperand(),
                                   llvmAccessSize(LI->getType()),
                                   /*AddressOf=*/false))
      return SourceRecorder ? SourceRecorder->expression(*LI, Acc->Text)
                            : Acc->Text;
    if (auto Acc = frameSlotAccess(LI, 0, /*AddressOf=*/true))
      return AddressText(Acc->Text);
  }
  if (const auto Peeled = peelPointerOffset(V)) {
    if (const auto *AI = llvm::dyn_cast<llvm::AllocaInst>(Peeled->first)) {
      const auto *Arr = llvm::dyn_cast<llvm::ArrayType>(AI->getAllocatedType());
      if (Arr && Arr->getElementType()->isIntegerTy(8)) {
        const std::string Name = getName(const_cast<llvm::AllocaInst *>(AI));
        const int64_t Signed = static_cast<int64_t>(Peeled->second);
        if (Signed == 0)
          return AddressText("&" + Name);
        if (Signed > 0)
          return AddressText("((char*)&" + Name + " + " +
                             std::to_string(Signed) + ")");
        return AddressText("((char*)&" + Name + " - " +
                           std::to_string(-Signed) + ")");
      }
    }
  }
  if (const auto *LI = llvm::dyn_cast<llvm::LoadInst>(V)) {
    if (const llvm::AllocaInst *Slot = asAllocaPointer(LI->getPointerOperand());
        Slot && isTypedRecordCursorSlot(Slot)) {
      if (isThisFieldValueHome(Slot) || !cursorSlotIsObserved(Slot)) {
        if (auto Text = AllocaTexts.find(Slot);
            Text != AllocaTexts.end() && !Text->second.empty())
          return Text->second;
        if (auto Text = ValueTexts.find(LI);
            Text != ValueTexts.end() && !Text->second.empty())
          return Text->second;
      }
      if (cursorSlotIsObserved(Slot) && !isThisFieldValueHome(Slot)) {
        if (auto Name = ValNames.find(Slot); Name != ValNames.end())
          return Name->second;
        return getName(Slot);
      }
    }
  }
  if (const auto *StoredLoad = llvm::dyn_cast<llvm::LoadInst>(V)) {
    if (!typedRecordAccess(StoredLoad->getPointerOperand(),
                           llvmAccessSize(StoredLoad->getType()))) {
      const llvm::AllocaInst *CursorHome = nullptr;
      unsigned Stores = 0;
      for (const llvm::User *U : StoredLoad->users()) {
        const auto *SI = llvm::dyn_cast<llvm::StoreInst>(U);
        if (!SI || SI->getValueOperand() != StoredLoad)
          continue;
        const llvm::AllocaInst *Slot = asAllocaPointer(SI->getPointerOperand());
        if (!Slot || !isTypedRecordCursorSlot(Slot) ||
            !cursorSlotIsObserved(Slot))
          continue;
        CursorHome = Slot;
        ++Stores;
      }
      if (Stores == 1 && CursorHome) {
        if (auto Name = ValNames.find(CursorHome); Name != ValNames.end())
          return Name->second;
        return getName(CursorHome);
      }
    }
  }
  if (auto Text = ValueTexts.find(V);
      Text != ValueTexts.end() && !Text->second.empty()) {
    // Cached field/index spellings still evaluate the original memory load.
    // A cached variable name does not: never attribute that reference as an
    // emitted memory operation.
    if (const auto *LI = llvm::dyn_cast<llvm::LoadInst>(V);
        SourceRecorder && LI) {
      auto Access = typedRecordAccess(LI->getPointerOperand(),
                                      llvmAccessSize(LI->getType()));
      if (!Access)
        Access = typedIndexAccess(LI->getPointerOperand());
      if (Access && Access->Text == Text->second)
        return SourceRecorder->expression(*LI, Text->second);
    }
    return Text->second;
  }
  if (const auto *LI = llvm::dyn_cast<llvm::LoadInst>(V)) {
    if (auto Acc = typedRecordAccess(LI->getPointerOperand(),
                                     llvmAccessSize(LI->getType())))
      return SourceRecorder ? SourceRecorder->expression(*LI, Acc->Text)
                            : Acc->Text;
  }
  if (std::string Text = composedReprintText(V); !Text.empty())
    return Text;
  if (const auto *BO = llvm::dyn_cast<llvm::BinaryOperator>(V)) {
    if (BO->getOpcode() == llvm::Instruction::URem ||
        BO->getOpcode() == llvm::Instruction::SRem) {
      if (std::string Rem = indexExprStr(BO); !Rem.empty())
        return Rem;
    }
  }
  if (const auto *LI = llvm::dyn_cast<llvm::LoadInst>(V)) {
    if (const llvm::AllocaInst *Slot =
            asAllocaPointer(LI->getPointerOperand())) {
      if (Slot == PhiTailSlot && PhiTailIncoming)
        return valueStr(PhiTailIncoming, PointerSpelling);
      if (joinFieldDefaultText(Slot))
        return getName(const_cast<llvm::AllocaInst *>(Slot));
      if (isJoinCallArgAlloca(Slot) && Slot != PhiTailSlot)
        return getName(Slot);
      if (const llvm::Value *Stored = allocaStoredValueBefore(LI);
          Stored && Stored != V) {
        if (auto Imm = foldImmediate(Stored))
          return *Imm;
        return valueStr(Stored, PointerSpelling);
      }
    }
  }
  auto Fwd = Analysis.ForwardedLoads.find(V);
  if (Fwd != Analysis.ForwardedLoads.end())
    return valueStr(Fwd->second, PointerSpelling);
  if (auto *EV = llvm::dyn_cast<llvm::ExtractValueInst>(V)) {
    auto It = Analysis.IntrinsicStructNames.find(EV->getAggregateOperand());
    if (It != Analysis.IntrinsicStructNames.end())
      return It->second + "[" + std::to_string(EV->getIndices()[0]) + "]";
  }
  if (Analysis.Inlinable.count(V)) {
    auto It = InlineCache.find(V);
    if (It != InlineCache.end())
      return It->second;
    auto *Inst = llvm::dyn_cast<llvm::Instruction>(V);
    if (Inst) {
      auto Expr = renderInline(*Inst);
      InlineCache[V] = Expr;
      return Expr;
    }
  }
  if (auto *C = llvm::dyn_cast<llvm::Constant>(V))
    return constStr(C);
  if (const llvm::AllocaInst *Slot = llvm::dyn_cast<llvm::AllocaInst>(V))
    return AddressText("&" + getName(Slot));
  return getName(V);
}

std::string LLVMCWriter::blockLabel(const llvm::BasicBlock *BB) {
  auto It = BlockLabels.find(BB);
  if (It != BlockLabels.end())
    return It->second;
  std::string Label;
  if (BB->hasName()) {
    std::string Raw = BB->getName().str();
    Label = "L_";
    for (char Ch : Raw) {
      if (std::isalnum(static_cast<unsigned char>(Ch)) || Ch == '_')
        Label += Ch;
      else
        Label += '_';
    }
  } else {
    Label = "L_" + std::to_string(BlockLabels.size());
  }
  BlockLabels[BB] = Label;
  return Label;
}

std::string LLVMCWriter::binopStr(unsigned Opcode, const std::string &LHS,
                                  const std::string &RHS, llvm::Type *Ty) {
  auto Unsigned = [&](const std::string &Text) {
    const std::string Cast = "(" + typeToCLLVM(Ty) + ")";
    // Keep an existing cast at the operation's width. A generic unsigned
    // cast would truncate i64/i128 before division, remainder, or shifting.
    return Text.starts_with(Cast) ? Text : Cast + "(" + Text + ")";
  };
  switch (Opcode) {
  case llvm::Instruction::Add:
  case llvm::Instruction::FAdd:
    return LHS + " + " + RHS;
  case llvm::Instruction::Sub:
  case llvm::Instruction::FSub:
    return LHS + " - " + RHS;
  case llvm::Instruction::Mul:
    // A full u16 product can overflow C's signed integer promotion even
    // though the LLVM bitvector multiplication is defined.
    if (Ty->isIntegerTy()) {
      const std::string Carrier =
          Ty->getIntegerBitWidth() < 32 ? "uint32_t" : typeToCLLVM(Ty);
      const std::string Product =
          "(" + Carrier + ")(" + LHS + ") * (" + Carrier + ")(" + RHS + ")";
      return castStr(llvm::Instruction::Trunc, Product, Ty, Ty);
    }
    return LHS + " * " + RHS;
  case llvm::Instruction::FMul:
    return LHS + " * " + RHS;
  case llvm::Instruction::UDiv:
    return Unsigned(LHS) + " / " + Unsigned(RHS);
  case llvm::Instruction::SDiv:
    return castStr(llvm::Instruction::Trunc,
                   signedIntegerOperand(Ty, LHS) + " / " +
                       signedIntegerOperand(Ty, RHS),
                   Ty, Ty);
  case llvm::Instruction::FDiv:
    return LHS + " / " + RHS;
  case llvm::Instruction::URem:
    return Unsigned(LHS) + " % " + Unsigned(RHS);
  case llvm::Instruction::SRem:
    return castStr(llvm::Instruction::Trunc,
                   signedIntegerOperand(Ty, LHS) + " % " +
                       signedIntegerOperand(Ty, RHS),
                   Ty, Ty);
  case llvm::Instruction::FRem:
    return LHS + " % " + RHS;
  case llvm::Instruction::Shl:
    return castStr(llvm::Instruction::Trunc, Unsigned(LHS) + " << " + RHS, Ty,
                   Ty);
  case llvm::Instruction::LShr:
    return Unsigned(LHS) + " >> " + RHS;
  case llvm::Instruction::AShr:
    // LLVM integers have no declared signedness. The C carrier is unsigned,
    // so reconstruct the source-width sign before either assigned or inline
    // arithmetic shifts (including narrow operands promoted by C).
    // Restore an unsigned, width-normalized bitvector result as well: direct
    // odd-width returns must not leak sign-extension bits, and following
    // wrapping arithmetic must not inherit C signed-overflow behavior.
    return castStr(llvm::Instruction::Trunc,
                   signedIntegerOperand(Ty, LHS) + " >> " + RHS, Ty, Ty);
  case llvm::Instruction::And:
    return LHS + " & " + RHS;
  case llvm::Instruction::Or:
    return LHS + " | " + RHS;
  case llvm::Instruction::Xor:
    return LHS + " ^ " + RHS;
  default:
    return LHS + " /* unknown_binop */ " + RHS;
  }
}

std::string LLVMCWriter::integerPointerOperandStr(const llvm::Value *Operand) {
  if (!Operand)
    return {};
  bool PointerSpelling = false;
  std::string Text = valueStr(Operand, &PointerSpelling);
  if (!Operand->getType()->isIntegerTy() || !PointerSpelling)
    return Text;
  // The pointer-offset printer may return `&frame` for an integer IR value.
  // Convert that actual pointer spelling, not merely pointer provenance: an
  // already widened integer must not be truncated back through uintptr_t.
  return "(" + typeToCLLVM(Operand->getType()) + ")(uintptr_t)(" + Text + ")";
}

std::optional<std::string>
LLVMCWriter::targetPointerMaskIdentity(const llvm::BinaryOperator &Op) {
  if (Op.getOpcode() != llvm::Instruction::And || !Op.getType()->isIntegerTy())
    return std::nullopt;
  const unsigned Width = Op.getType()->getIntegerBitWidth();
  const unsigned PointerWidth = getTargetRegInfo(Opts.TheArch).PointerSize * 8;
  if (!PointerWidth || Width <= PointerWidth || Width > 64)
    return std::nullopt;
  const auto *Mask = llvm::dyn_cast<llvm::ConstantInt>(Op.getOperand(1));
  const llvm::Value *Base = Op.getOperand(0);
  if (!Mask) {
    Mask = llvm::dyn_cast<llvm::ConstantInt>(Base);
    Base = Op.getOperand(1);
  }
  if (!Mask ||
      Mask->getValue() != llvm::APInt::getLowBitsSet(Width, PointerWidth))
    return std::nullopt;
  const auto *Cast = llvm::dyn_cast<llvm::Operator>(Base);
  if (!Cast || Cast->getOpcode() != llvm::Instruction::PtrToInt ||
      !Cast->getOperand(0)->getType()->isPointerTy())
    return std::nullopt;
  // The target-width mask is an identity on its pointer. A generated C
  // pointer may occupy high host bits, so reapplying the numeric mask would
  // truncate a valid host address before the matching inttoptr.
  return "(" + typeToCLLVM(Op.getType()) + ")(uintptr_t)(" +
         valueStr(Cast->getOperand(0)) + ")";
}

std::string LLVMCWriter::castStr(unsigned Opcode, const std::string &Src,
                                 llvm::Type *SrcTy, llvm::Type *DstTy) {
  const std::string Dst = typeToCLLVM(DstTy);
  if (!SrcTy->isIntegerTy() || !DstTy->isIntegerTy())
    return "(" + Dst + ")" + Src;
  const unsigned SrcWidth = SrcTy->getIntegerBitWidth();
  const unsigned DstWidth = DstTy->getIntegerBitWidth();
  if (SrcWidth > 512 || DstWidth > 512)
    throw std::runtime_error("LLVMC integer cast exceeds 512-bit carrier");

  auto Cast = [](const std::string &Type, const std::string &Text) {
    return "(" + Type + ")(" + Text + ")";
  };
  auto Normalize = [&](const std::string &Text, llvm::Type *Type) {
    const unsigned Width = Type->getIntegerBitWidth();
    const std::string Carrier = typeToCLLVM(Type);
    std::string Result = Cast(Carrier, Text);
    const std::string Prefix = "(" + Carrier + ")";
    if (Text.starts_with(Prefix)) {
      const std::string Operand = Text.substr(Prefix.size());
      if (peelOperandWrap(Operand) != Operand)
        Result = Text;
    }
    // Wider carriers are exact C23 _BitInt(N); shifting by N to manufacture
    // a mask would itself be undefined. Only rounded narrow carriers need it.
    if (Width <= 128 && Width != 8 && Width != 16 && Width != 32 &&
        Width != 64 && Width != 128)
      Result = "(" + Result + " & (((" + Carrier + ")1 << " +
               std::to_string(Width) + ") - 1))";
    return Result;
  };
  switch (Opcode) {
  case llvm::Instruction::Trunc:
    return Normalize(Src, DstTy);
  case llvm::Instruction::ZExt:
    // A folded constant may use a negative spelling, and narrow C integers
    // promote to int. Reestablish the source bit pattern before widening.
    return Cast(Dst, Normalize(Src, SrcTy));
  case llvm::Instruction::SExt: {
    if (SrcWidth == 8 || SrcWidth == 16 || SrcWidth == 32 || SrcWidth == 64) {
      const std::string Signed = "int" + std::to_string(SrcWidth) + "_t";
      return Normalize(Cast(Signed, Src), DstTy);
    }
    // LLVM also permits widths without a signed C carrier. Move the sign bit
    // into an unsigned destination and extend it without signed overflow.
    const std::string Sign =
        "((" + Dst + ")1 << " + std::to_string(SrcWidth - 1) + ")";
    const std::string Value = Cast(Dst, Normalize(Src, SrcTy));
    return Normalize("(" + Value + " ^ " + Sign + ") - " + Sign, DstTy);
  }
  default:
    return "(" + Dst + ")" + Src;
  }
}

bool LLVMCWriter::operandIsUnsignedWidth(const llvm::Value *V,
                                         unsigned Bits) const {
  if (!V || Bits == 0 || Bits % 8 != 0 || Bits > 128)
    return false;
  // Without a debug signature, integer parameters use the exact unsigned
  // carrier from typeToCLLVM. Named debug enums and pointer views need their
  // own evidence and must not inherit this assumption.
  if (llvm::isa<llvm::Argument>(V) && !DebugFn &&
      V->getType()->isIntegerTy(Bits))
    return true;
  const llvm::Value *Cur = peelIntegerView(V);
  const auto *LI = llvm::dyn_cast_or_null<llvm::LoadInst>(Cur);
  if (!LI)
    return false;
  const auto Width = static_cast<uint16_t>(Bits / 8);
  auto Acc = typedRecordAccess(LI->getPointerOperand(), Width);
  if (!Acc || !Acc->Type || Acc->Type->Kind != NdTypeKind::Int)
    return false;
  if (Acc->Type->IsSigned || Acc->Type->IsEnum)
    return false;
  return Acc->Type->Size == Width;
}

std::string LLVMCWriter::unsignedCompareOperand(const llvm::Value *V,
                                                std::string Text) {
  unsigned Bits = 0;
  if (V && V->getType()->isIntegerTy())
    Bits = V->getType()->getIntegerBitWidth();
  // C promotes small carriers to int, while nonstandard LLVM widths occupy
  // a wider C carrier. Normalize both operands before these promotions can
  // compare a negative literal with a positive spelling of the same bits.
  if (V && integerComparisonNeedsNormalization(V->getType()))
    return castStr(llvm::Instruction::Trunc, Text, V->getType(), V->getType());
  if (Bits && operandIsUnsignedWidth(V, Bits))
    return Text;
  // The other operand is normalized separately. A nonnegative literal keeps
  // the same value under the usual conversions to that unsigned carrier.
  if (const auto *Constant = llvm::dyn_cast_or_null<llvm::ConstantInt>(V);
      Constant && !Constant->isNegative())
    return Text;
  if (Bits > 32) {
    const std::string Cast = "(" + typeToCLLVM(V->getType()) + ")";
    return Text.starts_with(Cast) ? Text : Cast + "(" + Text + ")";
  }
  return unsignedCmpOperand(Text);
}

std::string LLVMCWriter::cmpStr(llvm::CmpInst::Predicate Pred,
                                const std::string &LHS, const std::string &RHS,
                                bool /*IsFP*/, bool CastUnsigned) {
  auto UnsignedSide = [&](const std::string &S) {
    return CastUnsigned ? unsignedCmpOperand(S) : S;
  };
  switch (Pred) {
  case llvm::CmpInst::ICMP_EQ:
  case llvm::CmpInst::FCMP_OEQ:
  case llvm::CmpInst::FCMP_UEQ:
    return LHS + " == " + RHS;
  case llvm::CmpInst::ICMP_NE:
  case llvm::CmpInst::FCMP_ONE:
  case llvm::CmpInst::FCMP_UNE:
    return LHS + " != " + RHS;
  case llvm::CmpInst::ICMP_UGT:
    return UnsignedSide(LHS) + " > " + UnsignedSide(RHS);
  case llvm::CmpInst::ICMP_UGE:
    return UnsignedSide(LHS) + " >= " + UnsignedSide(RHS);
  case llvm::CmpInst::ICMP_ULT:
    return UnsignedSide(LHS) + " < " + UnsignedSide(RHS);
  case llvm::CmpInst::ICMP_ULE:
    return UnsignedSide(LHS) + " <= " + UnsignedSide(RHS);
  case llvm::CmpInst::ICMP_SGT:
  case llvm::CmpInst::FCMP_OGT:
  case llvm::CmpInst::FCMP_UGT:
    return LHS + " > " + RHS;
  case llvm::CmpInst::ICMP_SGE:
  case llvm::CmpInst::FCMP_OGE:
  case llvm::CmpInst::FCMP_UGE:
    return LHS + " >= " + RHS;
  case llvm::CmpInst::ICMP_SLT:
  case llvm::CmpInst::FCMP_OLT:
  case llvm::CmpInst::FCMP_ULT:
    return LHS + " < " + RHS;
  case llvm::CmpInst::ICMP_SLE:
  case llvm::CmpInst::FCMP_OLE:
  case llvm::CmpInst::FCMP_ULE:
    return LHS + " <= " + RHS;
  case llvm::CmpInst::FCMP_ORD:
    return LHS + " == " + LHS + " && " + RHS + " == " + RHS;
  case llvm::CmpInst::FCMP_UNO:
    return LHS + " != " + LHS + " || " + RHS + " != " + RHS;
  default:
    return LHS + " /* unknown_cmp */ " + RHS;
  }
}

std::string LLVMCWriter::logicalShiftLhs(const llvm::Instruction &Shift,
                                         std::string LHS) {
  if ((Shift.getOpcode() != llvm::Instruction::LShr &&
       Shift.getOpcode() != llvm::Instruction::Shl) ||
      !Shift.getType()->isIntegerTy())
    return LHS;
  const auto *Amt = llvm::dyn_cast<llvm::ConstantInt>(Shift.getOperand(1));
  if (!Amt || Amt->getBitWidth() > 64)
    return LHS;
  const uint64_t Sh = Amt->getZExtValue();
  const llvm::Value *Cur = Shift.getOperand(0);
  std::set<const llvm::Value *> Seen;
  const llvm::CastInst *Widen = nullptr;
  while (Cur && Seen.insert(Cur).second) {
    if (const auto *LI = llvm::dyn_cast<llvm::LoadInst>(Cur)) {
      if (const llvm::Value *Stored = allocaStoredValueBefore(LI);
          Stored && Stored != Cur) {
        Cur = Stored;
        continue;
      }
    }
    break;
  }
  if (const auto *Cast = llvm::dyn_cast_or_null<llvm::CastInst>(Cur);
      Cast && llvm::isa<llvm::ZExtInst, llvm::SExtInst>(Cast) &&
      Cast->getSrcTy()->isIntegerTy() && Cast->getDestTy()->isIntegerTy()) {
    const unsigned SrcW = Cast->getSrcTy()->getIntegerBitWidth();
    const unsigned DstW = Cast->getDestTy()->getIntegerBitWidth();
    if (Sh >= SrcW && Sh < DstW)
      Widen = Cast;
  }
  if (!Widen)
    return LHS;
  return castStr(Widen->getOpcode(), valueStr(Widen->getOperand(0)),
                 Widen->getSrcTy(), Widen->getDestTy());
}

std::string LLVMCWriter::icmpInlineText(const llvm::ICmpInst &CI, bool Invert) {
  if (UseScalarExpressionTypes)
    if (auto Text = scalarExpressionText(&CI, /*ForceExpression=*/true, Invert))
      return peelOperandWrap(*Text);
  const auto Predicate = Invert ? CI.getInversePredicate() : CI.getPredicate();
  std::string LHS =
      peelOperandWrap(comparedOperandText(CI.getOperand(0), CI.getOperand(1)));
  std::string RHS =
      peelOperandWrap(comparedOperandText(CI.getOperand(1), CI.getOperand(0)));
  // Equality has higher precedence than bitwise operators in C.  Retain the
  // grouping of an inlined status-word bit test such as `(sw & C2) != 0`.
  auto GroupBitwise = [](const llvm::Value *V, std::string &Text) {
    const auto *Op = llvm::dyn_cast<llvm::BinaryOperator>(V);
    if (Op && (Op->getOpcode() == llvm::Instruction::And ||
               Op->getOpcode() == llvm::Instruction::Or ||
               Op->getOpcode() == llvm::Instruction::Xor))
      Text = "(" + Text + ")";
  };
  GroupBitwise(CI.getOperand(0), LHS);
  GroupBitwise(CI.getOperand(1), RHS);
  const auto *Type = CI.getOperand(0)->getType();
  if (Type->isIntegerTy() && llvm::CmpInst::isSigned(Predicate)) {
    LHS = signedIntegerOperand(CI.getOperand(0), LHS);
    RHS = signedIntegerOperand(CI.getOperand(1), RHS);
  } else if (llvm::CmpInst::isUnsigned(Predicate) ||
             (Type->isIntegerTy() && CI.isEquality()) ||
             integerComparisonNeedsNormalization(Type)) {
    LHS = unsignedCompareOperand(CI.getOperand(0), std::move(LHS));
    RHS = unsignedCompareOperand(CI.getOperand(1), std::move(RHS));
  }
  return cmpStr(Predicate, LHS, RHS, false, /*CastUnsigned=*/false);
}

bool LLVMCWriter::isFloatingPointBitcast(const llvm::Instruction &Inst) {
  const auto *Cast = llvm::dyn_cast<llvm::BitCastInst>(&Inst);
  if (!Cast)
    return false;
  const auto *Source = Cast->getSrcTy();
  const auto *Destination = Cast->getDestTy();
  const auto *Float = Source->isIntegerTy() ? Destination : Source;
  const auto *Integer = Source->isIntegerTy() ? Source : Destination;
  return (Float->isFloatTy() || Float->isDoubleTy() || Float->isBFloatTy() ||
          Float->isX86_FP80Ty()) &&
         Integer->isIntegerTy() &&
         Float->getPrimitiveSizeInBits() == Integer->getPrimitiveSizeInBits();
}

std::string LLVMCWriter::fcmpInlineText(const llvm::FCmpInst &Compare) {
  const char *Builtin = nullptr;
  bool Negate = false;
  switch (Compare.getPredicate()) {
  case llvm::CmpInst::FCMP_FALSE:
  case llvm::CmpInst::FCMP_TRUE:
    return "((void)(" + valueStr(Compare.getOperand(0)) + "), (void)(" +
           valueStr(Compare.getOperand(1)) + "), " +
           (Compare.getPredicate() == llvm::CmpInst::FCMP_TRUE ? "1)" : "0)");
  case llvm::CmpInst::FCMP_OEQ:
  case llvm::CmpInst::FCMP_UNE:
    return "(" + valueStr(Compare.getOperand(0)) +
           (Compare.getPredicate() == llvm::CmpInst::FCMP_OEQ ? " == "
                                                              : " != ") +
           valueStr(Compare.getOperand(1)) + ")";
  case llvm::CmpInst::FCMP_UEQ:
    Negate = true;
    [[fallthrough]];
  case llvm::CmpInst::FCMP_ONE:
    Builtin = "__builtin_islessgreater";
    break;
  case llvm::CmpInst::FCMP_UGE:
    Negate = true;
    [[fallthrough]];
  case llvm::CmpInst::FCMP_OLT:
    Builtin = "__builtin_isless";
    break;
  case llvm::CmpInst::FCMP_UGT:
    Negate = true;
    [[fallthrough]];
  case llvm::CmpInst::FCMP_OLE:
    Builtin = "__builtin_islessequal";
    break;
  case llvm::CmpInst::FCMP_ULE:
    Negate = true;
    [[fallthrough]];
  case llvm::CmpInst::FCMP_OGT:
    Builtin = "__builtin_isgreater";
    break;
  case llvm::CmpInst::FCMP_ULT:
    Negate = true;
    [[fallthrough]];
  case llvm::CmpInst::FCMP_OGE:
    Builtin = "__builtin_isgreaterequal";
    break;
  case llvm::CmpInst::FCMP_ORD:
    Negate = true;
    [[fallthrough]];
  case llvm::CmpInst::FCMP_UNO:
    Builtin = "__builtin_isunordered";
    break;
  default:
    throw std::runtime_error("unsupported LLVM C floating-point predicate");
  }
  // Each operand appears once, including unordered predicates. A captured
  // floating-point producer must not run twice merely to check for NaN.
  return std::string(Negate ? "!" : "") + Builtin + "(" +
         valueStr(Compare.getOperand(0)) + ", " +
         valueStr(Compare.getOperand(1)) + ")";
}

std::string LLVMCWriter::gepExpr(const llvm::GEPOperator &GEP) {
  const auto &Layout = CurMod->getDataLayout();
  const unsigned AddressSpace = GEP.getPointerAddressSpace();
  const unsigned Bits = Layout.getIndexSizeInBits(AddressSpace);
  if (AddressSpace != 0 || (Bits != 32 && Bits != 64) ||
      Bits != Layout.getPointerSizeInBits(AddressSpace))
    throw std::runtime_error("unsupported LLVM C GEP pointer layout");
  llvm::SmallMapVector<llvm::Value *, llvm::APInt, 4> Variables;
  llvm::APInt Offset(Bits, 0);
  if (!GEP.collectOffset(Layout, Bits, Variables, Offset))
    throw std::runtime_error("unsupported LLVM C GEP offset");
  const std::string Carrier = "uint" + std::to_string(Bits) + "_t";
  std::string Base = valueStr(GEP.getPointerOperand());
  if (const auto *Global =
          llvm::dyn_cast<llvm::GlobalVariable>(GEP.getPointerOperand());
      Global && !Global->getValueType()->isArrayTy())
    Base = "&" + Base;
  std::string Sum = "(" + Carrier + ")(uintptr_t)(" + Base + ")";
  if (!Offset.isZero())
    Sum += " + (" + Carrier + ")0x" + llvm::utohexstr(Offset.getZExtValue()) +
           "ULL";
  for (const auto &[Index, Scale] : Variables) {
    // GEP indices are signed at their original width, then sign extended or
    // truncated to the pointer index width. Unsigned carrier arithmetic keeps
    // address/offset multiplication wrapping without C signed-overflow UB.
    Sum += " + (" + Carrier + ")(" +
           signedIntegerOperand(Index, valueStr(Index)) + ") * (" + Carrier +
           ")0x" + llvm::utohexstr(Scale.getZExtValue()) + "ULL";
  }
  return "(void*)(uintptr_t)(" + Carrier + ")(" + Sum + ")";
}

std::string LLVMCWriter::aggregateMemberPath(llvm::Type *Ty,
                                             llvm::ArrayRef<unsigned> Indices) {
  std::string Path;
  for (unsigned Index : Indices) {
    if (auto *Array = llvm::dyn_cast<llvm::ArrayType>(Ty)) {
      Path += ".elements[" + std::to_string(Index) + "]";
      Ty = Array->getElementType();
    } else {
      auto *Struct = llvm::cast<llvm::StructType>(Ty);
      Path += ".field_" + std::to_string(Index);
      Ty = Struct->getElementType(Index);
    }
  }
  return Path;
}

std::string LLVMCWriter::renderInline(const llvm::Instruction &Inst) {
  std::string Text = renderInlineImpl(Inst);
  return SourceRecorder ? SourceRecorder->expression(Inst, std::move(Text))
                        : Text;
}

std::string LLVMCWriter::renderInlineImpl(const llvm::Instruction &Inst) {
  if (Inst.getOpcode() == llvm::Instruction::FNeg)
    return "(-" + valueStr(Inst.getOperand(0)) + ")";
  if (const auto *Extract = llvm::dyn_cast<llvm::ExtractElementInst>(&Inst))
    return "(" + valueStr(Extract->getVectorOperand()) + ")[" +
           valueStr(Extract->getIndexOperand()) + "]";
  if (const auto *CB = llvm::dyn_cast<llvm::CallBase>(&Inst))
    return callExpr(*CB);
  if (const auto *CI = llvm::dyn_cast<llvm::ICmpInst>(&Inst))
    return "(" + icmpInlineText(*CI) + ")";
  if (const auto *Compare = llvm::dyn_cast<llvm::FCmpInst>(&Inst))
    return fcmpInlineText(*Compare);
  if (Inst.isBinaryOp()) {
    if (auto Identity =
            targetPointerMaskIdentity(llvm::cast<llvm::BinaryOperator>(Inst)))
      return *Identity;
    const bool NeedsIntegerPointerOperand = Inst.getType()->isIntegerTy();
    std::string LHS =
        logicalShiftLhs(Inst, NeedsIntegerPointerOperand
                                  ? integerPointerOperandStr(Inst.getOperand(0))
                                  : valueStr(Inst.getOperand(0)));
    const std::string RHS = NeedsIntegerPointerOperand
                                ? integerPointerOperandStr(Inst.getOperand(1))
                                : valueStr(Inst.getOperand(1));
    return "(" + binopStr(Inst.getOpcode(), LHS, RHS, Inst.getType()) + ")";
  }
  if (const auto *Trunc = llvm::dyn_cast<llvm::TruncInst>(&Inst)) {
    // Widen, arithmetic, then trunc back to that width. At 32 bits or
    // more the printed type already wraps, so the outer cast is redundant
    // for both zero- and sign-extend. Narrower C promotes to signed int,
    // so the product stays in an unsigned carrier and the trunc remains.
    const auto *Bin = llvm::dyn_cast<llvm::BinaryOperator>(
        peelIntegerView(Trunc->getOperand(0)));
    const unsigned Op = Bin ? Bin->getOpcode() : 0;
    if (Bin && Trunc->getType()->isIntegerTy() &&
        Bin->getType()->isIntegerTy() &&
        (Op == llvm::Instruction::Add || Op == llvm::Instruction::Sub ||
         Op == llvm::Instruction::Mul)) {
      const unsigned DestW = Trunc->getType()->getIntegerBitWidth();
      const unsigned BinW = Bin->getType()->getIntegerBitWidth();
      auto SourceAtDest = [&](const llvm::Value *V) -> const llvm::Value * {
        const auto *Cast = llvm::dyn_cast<llvm::CastInst>(V);
        if (!Cast ||
            !(llvm::isa<llvm::ZExtInst>(Cast) ||
              llvm::isa<llvm::SExtInst>(Cast) ||
              llvm::isa<llvm::TruncInst>(Cast)) ||
            !Cast->getSrcTy()->isIntegerTy() ||
            Cast->getSrcTy()->getIntegerBitWidth() != DestW)
          return nullptr;
        const llvm::Value *Root = peelIntegerView(Cast->getOperand(0));
        if (!Root || !Root->getType()->isIntegerTy() ||
            Root->getType()->getIntegerBitWidth() != DestW)
          return nullptr;
        return Root;
      };
      const llvm::Value *L = nullptr;
      const llvm::Value *R = nullptr;
      if (DestW > 0 && DestW < BinW) {
        L = SourceAtDest(Bin->getOperand(0));
        R = SourceAtDest(Bin->getOperand(1));
      }
      const char *Token = Op == llvm::Instruction::Add   ? " + "
                          : Op == llvm::Instruction::Sub ? " - "
                                                         : " * ";
      if (L && R && (DestW == 32 || DestW == 64 || DestW == 128) &&
          operandIsUnsignedWidth(L, DestW) && operandIsUnsignedWidth(R, DestW))
        return "(" + valueStr(L) + Token + valueStr(R) + ")";
      if (L && R && (DestW == 32 || DestW == 64 || DestW == 128))
        return "(" + binopStr(Op, valueStr(L), valueStr(R), Trunc->getType()) +
               ")";
      if (L && R) {
        llvm::Type *CarrierTy = BinW >= 32
                                    ? Bin->getType()
                                    : llvm::Type::getInt32Ty(Bin->getContext());
        const std::string Carrier = "(" + typeToCLLVM(CarrierTy) + ")";
        return castStr(llvm::Instruction::Trunc,
                       Carrier + valueStr(L) + Token + Carrier + valueStr(R),
                       CarrierTy, Trunc->getType());
      }
    }
  }
  if (Inst.isCast()) {
    auto Src = valueStr(Inst.getOperand(0));
    return castStr(Inst.getOpcode(), Src, Inst.getOperand(0)->getType(),
                   Inst.getType());
  }
  if (llvm::isa<llvm::GetElementPtrInst>(Inst))
    return gepExpr(*llvm::cast<llvm::GEPOperator>(&Inst));
  if (auto *Sel = llvm::dyn_cast<llvm::SelectInst>(&Inst)) {
    if (auto Condition = foldImmediate(Sel->getCondition());
        Condition && (*Condition == "0" || *Condition == "1"))
      return valueStr(*Condition == "1" ? Sel->getTrueValue()
                                        : Sel->getFalseValue());
    return "(" + valueStr(Sel->getCondition()) + " ? " +
           valueStr(Sel->getTrueValue()) + " : " +
           valueStr(Sel->getFalseValue()) + ")";
  }
  if (const auto *RMW = llvm::dyn_cast<llvm::AtomicRMWInst>(&Inst))
    return atomicRMWText(*RMW);
  return getName(&Inst);
}

std::string LLVMCWriter::atomicRMWText(const llvm::AtomicRMWInst &AI) {
  const char *Op = nullptr;
  bool CompareExchangeMinMax = false;
  bool SignedMinMax = false;
  bool Max = false;
  switch (AI.getOperation()) {
  case llvm::AtomicRMWInst::Add:
    Op = "__atomic_fetch_add";
    break;
  case llvm::AtomicRMWInst::Sub:
    Op = "__atomic_fetch_sub";
    break;
  case llvm::AtomicRMWInst::And:
    Op = "__atomic_fetch_and";
    break;
  case llvm::AtomicRMWInst::Or:
    Op = "__atomic_fetch_or";
    break;
  case llvm::AtomicRMWInst::Xor:
    Op = "__atomic_fetch_xor";
    break;
  case llvm::AtomicRMWInst::Xchg:
    Op = "__atomic_exchange_n";
    break;
  case llvm::AtomicRMWInst::Max:
  case llvm::AtomicRMWInst::Min:
  case llvm::AtomicRMWInst::UMax:
  case llvm::AtomicRMWInst::UMin:
    CompareExchangeMinMax = true;
    SignedMinMax = AI.getOperation() == llvm::AtomicRMWInst::Max ||
                   AI.getOperation() == llvm::AtomicRMWInst::Min;
    Max = AI.getOperation() == llvm::AtomicRMWInst::Max ||
          AI.getOperation() == llvm::AtomicRMWInst::UMax;
    break;
  default:
    llvm::report_fatal_error("unsupported atomicrmw operation");
  }
  const char *Ord = nullptr;
  switch (AI.getOrdering()) {
  case llvm::AtomicOrdering::Unordered:
  case llvm::AtomicOrdering::Monotonic:
    Ord = "__ATOMIC_RELAXED";
    break;
  case llvm::AtomicOrdering::Acquire:
    Ord = "__ATOMIC_ACQUIRE";
    break;
  case llvm::AtomicOrdering::Release:
    Ord = "__ATOMIC_RELEASE";
    break;
  case llvm::AtomicOrdering::AcquireRelease:
    Ord = "__ATOMIC_ACQ_REL";
    break;
  case llvm::AtomicOrdering::SequentiallyConsistent:
    Ord = "__ATOMIC_SEQ_CST";
    break;
  default:
    llvm::report_fatal_error("unsupported atomic ordering");
  }
  HasCIntrinsics = true;
  std::string Ptr = valueStr(AI.getPointerOperand());
  if (AI.getType()->isIntegerTy()) {
    const uint16_t Width =
        static_cast<uint16_t>(AI.getType()->getIntegerBitWidth() / 8);
    if (auto Acc = typedRecordAccess(AI.getPointerOperand(), Width,
                                     /*EnterNestedAtZero=*/false))
      Ptr = "&" + Acc->Text;
  }
  std::string Val = valueStr(AI.getValOperand());
  if (const auto *CI = llvm::dyn_cast<llvm::ConstantInt>(
          peelIntegerView(AI.getValOperand()))) {
    const unsigned W =
        AI.getType()->isIntegerTy() ? AI.getType()->getIntegerBitWidth() : 0;
    if (W && W <= 64 && CI->getBitWidth() >= W &&
        CI->getValue().trunc(W).isAllOnes())
      Val = "-1";
  }
  if (CompareExchangeMinMax) {
    if (!AI.getType()->isIntegerTy())
      llvm::report_fatal_error("atomic min/max requires an integer type");
    const unsigned Bits = AI.getType()->getIntegerBitWidth();
    if (Bits != 8 && Bits != 16 && Bits != 32 && Bits != 64 && Bits != 128)
      llvm::report_fatal_error("unsupported atomic min/max integer width");
    const std::string Ty =
        SignedMinMax
            ? (Bits == 128 ? "__int128_t" : "int" + std::to_string(Bits) + "_t")
            : typeToCLLVM(AI.getType());
    // A successful CAS is the one atomic read-modify-write. Even when the
    // selected value equals the old value, do not skip it: a release RMW must
    // still participate in the modification order. Failed probes only need
    // relaxed ordering; the successful CAS carries the IR instruction's order.
    const std::string PtrName = freshVar("nd_atomic_ptr");
    const std::string ValName = freshVar("nd_atomic_val");
    const std::string OldName = freshVar("nd_atomic_old");
    const std::string NextName = freshVar("nd_atomic_next");
    return "__extension__ ({ " + Ty + " *" + PtrName + " = (" + Ty + "*)(" +
           Ptr + "); " + Ty + " " + ValName + " = (" + Ty + ")(" + Val + "); " +
           Ty + " " + OldName + " = __atomic_load_n(" + PtrName +
           ", __ATOMIC_RELAXED); " + Ty + " " + NextName + "; do { " +
           NextName + " = " + OldName + (Max ? " < " : " > ") + ValName +
           " ? " + ValName + " : " + OldName +
           "; } while (!__atomic_compare_exchange_n(" + PtrName + ", &" +
           OldName + ", " + NextName + ", 0, " + Ord +
           ", __ATOMIC_RELAXED)); " + OldName + "; })";
  }
  return std::string(Op) + "(" + Ptr + ", " + Val + ", " + Ord + ")";
}

bool LLVMCWriter::unreadMsvcThisReturn(const llvm::CallBase &Call) const {
  const MsvcCallee *Msvc = msvcCallee(printedCalleeName(Call), Opts.Format);
  if (!Msvc || (Msvc->Kind != MsvcCalleeKind::Ctor &&
                Msvc->Kind != MsvcCalleeKind::Dtor))
    return false;
  const auto *CI = llvm::dyn_cast<llvm::CallInst>(&Call);
  if (!CI)
    return false;
  for (const llvm::User *U : CI->users()) {
    if (llvm::isa<llvm::ReturnInst>(U))
      continue;
    const auto *UI = llvm::dyn_cast<llvm::Instruction>(U);
    if (!UI)
      return false;
    if (Analysis.DeadFrameStores.count(UI))
      continue;
    const auto *SI = llvm::dyn_cast<llvm::StoreInst>(UI);
    if (!SI || SI->getValueOperand() != CI)
      return false;
    const llvm::AllocaInst *Slot = asAllocaPointer(SI->getPointerOperand());
    if (!Slot || allocaHasLoad(Slot))
      return false;
  }
  return true;
}

std::string LLVMCWriter::ctorThisAddress(const llvm::CallBase &Call) {
  if (Call.arg_empty())
    return {};
  const MsvcCallee *Msvc = msvcCallee(printedCalleeName(Call), Opts.Format);
  if (!Msvc || Msvc->Kind != MsvcCalleeKind::Ctor)
    return {};
  const std::string Addr = valueStr(Call.getArgOperand(0));
  if (!Addr.starts_with("&"))
    return {};
  return Addr;
}

} // namespace neverd
