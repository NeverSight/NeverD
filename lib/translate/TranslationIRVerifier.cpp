//===- TranslationIRVerifier.cpp - Validate host translation IR ----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "neverd/translate/TranslationIRVerifier.h"

#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringMap.h"
#include "llvm/Analysis/ValueTracking.h"
#include "llvm/IR/CFG.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/DataLayout.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/GlobalAlias.h"
#include "llvm/IR/GlobalIFunc.h"
#include "llvm/IR/InlineAsm.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/IR/Intrinsics.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/PatternMatch.h"
#include "llvm/IR/Verifier.h"
#include "llvm/Support/Errc.h"
#include "llvm/Support/KnownBits.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/TargetParser/Triple.h"

#include <algorithm>
#include <numeric>
#include <optional>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

namespace neverd::translate {

char TranslationIRVerificationError::ID;

TranslationIRVerificationError::TranslationIRVerificationError(
    TranslationIRViolation Reason, std::string FunctionName, std::string Detail)
    : Reason(Reason), FunctionName(std::move(FunctionName)),
      Detail(std::move(Detail)) {}

void TranslationIRVerificationError::log(llvm::raw_ostream &OS) const {
  OS << "translation IR verification";
  if (!FunctionName.empty())
    OS << " for '" << FunctionName << "'";
  OS << ": ";
  switch (Reason) {
  case TranslationIRViolation::InvalidLLVMIR:
    OS << "LLVM IR is invalid";
    break;
  case TranslationIRViolation::HostTripleMismatch:
    OS << "host triple does not match";
    break;
  case TranslationIRViolation::HostDataLayoutMismatch:
    OS << "host data layout does not match";
    break;
  case TranslationIRViolation::NonStandardBlockABI:
    OS << "translated block does not use the canonical ABI";
    break;
  case TranslationIRViolation::InlineAssembly:
    OS << "inline assembly is forbidden";
    break;
  case TranslationIRViolation::TargetSpecificIntrinsic:
    OS << "target-specific intrinsic is forbidden";
    break;
  case TranslationIRViolation::GuestIntegerToPointer:
    OS << "guest integer was converted to a host pointer";
    break;
  case TranslationIRViolation::ExternalSymbolNotAllowed:
    OS << "external symbol or indirect call is not allowed";
    break;
  case TranslationIRViolation::HostExceptionHandling:
    OS << "host exception handling or unwinding is forbidden";
    break;
  case TranslationIRViolation::GuestObservableUndefOrPoison:
    OS << "undef or poison can become guest-observable";
    break;
  case TranslationIRViolation::UnprovenPoisonGeneratingFlag:
    OS << "poison-generating flag is not proven safe";
    break;
  case TranslationIRViolation::DirectGuestMemoryAccess:
    OS << "memory access does not originate in bounded private state";
    break;
  case TranslationIRViolation::UnboundedStateOrRuntimeAccess:
    OS << "state or runtime access is outside its declared extent";
    break;
  case TranslationIRViolation::InvalidPolicy:
    OS << "verification policy is invalid";
    break;
  case TranslationIRViolation::HostPointerExposure:
    OS << "private host pointer can become guest-visible";
    break;
  case TranslationIRViolation::MutableGlobalState:
    OS << "mutable global state is forbidden";
    break;
  case TranslationIRViolation::UnsupportedHostIROperation:
    OS << "operation is outside the scalar host-IR contract";
    break;
  case TranslationIRViolation::IntrinsicNotAllowed:
    OS << "LLVM intrinsic is not in the scalar translation allowlist";
    break;
  case TranslationIRViolation::RuntimeHelperABIMismatch:
    OS << "runtime helper does not match its exact ABI contract";
    break;
  case TranslationIRViolation::DirectBlockCall:
    OS << "translated block bypasses the runtime dispatcher";
    break;
  case TranslationIRViolation::UnboundedPrivateMemoryAccess:
    OS << "private memory access is outside its constant object";
    break;
  case TranslationIRViolation::UnprovenUndefinedBehavior:
    OS << "operation can trigger unproven LLVM undefined behavior";
    break;
  case TranslationIRViolation::UnprovenSemanticMetadata:
    OS << "semantic metadata is not proven by the translation contract";
    break;
  case TranslationIRViolation::MemorySlotNotAllowed:
    OS << "memory access is outside the declared ABI slots";
    break;
  case TranslationIRViolation::DispatchCycle:
    OS << "translated block contains a cycle that bypasses dispatch polling";
    break;
  case TranslationIRViolation::BackendLibcallRisk:
    OS << "operation can introduce an unregistered backend runtime call";
    break;
  case TranslationIRViolation::RuntimeProtocolViolation:
    OS << "runtime helper control protocol is not proven";
    break;
  }
  if (!Detail.empty())
    OS << " (" << Detail << ")";
}

std::error_code TranslationIRVerificationError::convertToErrorCode() const {
  return std::make_error_code(std::errc::invalid_argument);
}

namespace {

enum class PointerRoot : uint8_t {
  Unknown,
  State,
  Runtime,
  Private,
};

llvm::Error failure(TranslationIRViolation Reason,
                    const llvm::Function *Function = nullptr,
                    llvm::StringRef Detail = {}) {
  return llvm::make_error<TranslationIRVerificationError>(
      Reason, Function ? Function->getName().str() : std::string{},
      Detail.str());
}

bool isCanonicalBlockABI(const llvm::Function &Function) {
  const llvm::FunctionType *Type = Function.getFunctionType();
  return Function.getLinkage() == llvm::GlobalValue::ExternalLinkage &&
         Function.hasHiddenVisibility() &&
         Function.getCallingConv() == llvm::CallingConv::C &&
         Type->getReturnType()->isIntegerTy(32) && !Type->isVarArg() &&
         Type->getNumParams() == 2 && Type->getParamType(0)->isPointerTy() &&
         Type->getParamType(1)->isPointerTy() &&
         Type->getParamType(0)->getPointerAddressSpace() == 0 &&
         Type->getParamType(1)->getPointerAddressSpace() == 0 &&
         Function.doesNotThrow();
}

bool hasOnlyNoUnwindAttributes(const llvm::AttributeList &Attributes,
                               unsigned ParameterCount) {
  if (Attributes.hasRetAttrs())
    return false;
  for (unsigned Index = 0; Index != ParameterCount; ++Index)
    if (Attributes.hasParamAttrs(Index))
      return false;
  for (const llvm::Attribute Attribute : Attributes.getFnAttrs())
    if (!Attribute.isEnumAttribute() ||
        Attribute.getKindAsEnum() != llvm::Attribute::NoUnwind)
      return false;
  return true;
}

bool hasForbiddenFunctionCodegenState(const llvm::Function &Function) {
  return Function.hasPrefixData() || Function.hasPrologueData() ||
         Function.hasSection() || Function.hasComdat() || Function.hasGC() ||
         Function.getAddressSpace() != 0 || Function.hasPartition() ||
         Function.hasAtLeastLocalUnnamedAddr() ||
         Function.getAlign().has_value() ||
         Function.getDLLStorageClass() !=
             llvm::GlobalValue::DefaultStorageClass;
}

std::optional<unsigned> maxLegalIntegerWidth(const llvm::Triple &Triple) {
  switch (Triple.getArch()) {
  case llvm::Triple::aarch64:
  case llvm::Triple::x86_64:
    return 64;
  case llvm::Triple::arm:
  case llvm::Triple::thumb:
  case llvm::Triple::x86:
    return 32;
  default:
    return std::nullopt;
  }
}

bool hasControlFlowCycle(const llvm::Function &Function) {
  llvm::DenseMap<const llvm::BasicBlock *, unsigned> InDegree;
  for (const llvm::BasicBlock &Block : Function)
    InDegree.try_emplace(&Block, 0);
  for (const llvm::BasicBlock &Block : Function)
    for (const llvm::BasicBlock *Successor : llvm::successors(&Block))
      ++InDegree[Successor];

  llvm::SmallVector<const llvm::BasicBlock *, 16> Ready;
  for (const auto &[Block, Degree] : InDegree)
    if (Degree == 0)
      Ready.push_back(Block);
  size_t Visited = 0;
  while (!Ready.empty()) {
    const llvm::BasicBlock *Block = Ready.pop_back_val();
    ++Visited;
    for (const llvm::BasicBlock *Successor : llvm::successors(Block))
      if (--InDegree[Successor] == 0)
        Ready.push_back(Successor);
  }
  return Visited != InDegree.size();
}

PointerRoot rootForBase(const llvm::Value *Base,
                        const llvm::Function &Function) {
  if (const auto *Argument = llvm::dyn_cast<llvm::Argument>(Base)) {
    if (Argument->getParent() != &Function)
      return PointerRoot::Unknown;
    if (Argument->getArgNo() == 0)
      return PointerRoot::State;
    if (Argument->getArgNo() == 1)
      return PointerRoot::Runtime;
    return PointerRoot::Unknown;
  }
  if (llvm::isa<llvm::AllocaInst>(Base))
    return PointerRoot::Private;
  if (const auto *Global = llvm::dyn_cast<llvm::GlobalVariable>(Base))
    return Global->hasLocalLinkage() && Global->isConstant()
               ? PointerRoot::Private
               : PointerRoot::Unknown;
  return PointerRoot::Unknown;
}

struct PointerLocation {
  PointerRoot Root = PointerRoot::Unknown;
  const llvm::Value *Base = nullptr;
  int64_t Offset = 0;
  uint64_t Extent = 0;
};

PointerLocation locatePointer(const llvm::Value *Pointer,
                              const llvm::Function &Function,
                              const llvm::DataLayout &Layout,
                              uint64_t StateSize, uint64_t RuntimeSize) {
  PointerLocation Location;
  Location.Base =
      llvm::GetPointerBaseWithConstantOffset(Pointer, Location.Offset, Layout);
  Location.Root = rootForBase(Location.Base, Function);
  switch (Location.Root) {
  case PointerRoot::State:
    Location.Extent = StateSize;
    break;
  case PointerRoot::Runtime:
    Location.Extent = RuntimeSize;
    break;
  case PointerRoot::Private: {
    const auto *Global = llvm::dyn_cast<llvm::GlobalVariable>(Location.Base);
    if (!Global || !Global->hasInitializer()) {
      Location.Root = PointerRoot::Unknown;
      break;
    }
    const llvm::TypeSize Size = Layout.getTypeAllocSize(Global->getValueType());
    if (Size.isScalable()) {
      Location.Root = PointerRoot::Unknown;
      break;
    }
    Location.Extent = Size.getFixedValue();
    break;
  }
  case PointerRoot::Unknown:
    break;
  }
  return Location;
}

bool accessFits(int64_t Offset, uint64_t Width, uint64_t Extent) {
  if (Offset < 0 || Width > Extent)
    return false;
  return static_cast<uint64_t>(Offset) <= Extent - Width;
}

llvm::Error
validateMemorySlots(llvm::ArrayRef<TranslationIRMemorySlot> MemorySlots,
                    uint64_t StateSize, uint64_t RuntimeSize,
                    std::vector<TranslationIRMemorySlot> &Sorted) {
  Sorted.reserve(MemorySlots.size());
  for (const TranslationIRMemorySlot &Slot : MemorySlots) {
    const uint8_t Access = static_cast<uint8_t>(Slot.Access);
    if ((Slot.Region != TranslationIRMemoryRegion::State &&
         Slot.Region != TranslationIRMemoryRegion::Runtime) ||
        Slot.Size == 0 || Access == 0 || (Access & ~uint8_t{0x3}) != 0 ||
        Slot.Alignment == 0 || (Slot.Alignment & (Slot.Alignment - 1)) != 0)
      return failure(TranslationIRViolation::InvalidPolicy, nullptr,
                     "invalid translation memory slot");
    const uint64_t Extent = Slot.Region == TranslationIRMemoryRegion::State
                                ? StateSize
                                : RuntimeSize;
    if (Slot.Offset > Extent || Slot.Size > Extent - Slot.Offset)
      return failure(TranslationIRViolation::InvalidPolicy, nullptr,
                     "translation memory slot exceeds its ABI object");
    Sorted.push_back(Slot);
  }
  llvm::sort(Sorted, [](const auto &Left, const auto &Right) {
    if (Left.Region != Right.Region)
      return static_cast<uint8_t>(Left.Region) <
             static_cast<uint8_t>(Right.Region);
    return Left.Offset < Right.Offset;
  });
  for (size_t Index = 1; Index < Sorted.size(); ++Index) {
    const TranslationIRMemorySlot &Previous = Sorted[Index - 1];
    const TranslationIRMemorySlot &Current = Sorted[Index];
    if (Previous.Region == Current.Region &&
        Current.Offset < Previous.Offset + Previous.Size)
      return failure(TranslationIRViolation::InvalidPolicy, nullptr,
                     "translation memory slots overlap");
  }
  return llvm::Error::success();
}

uint64_t effectiveAlignment(uint64_t BaseAlignment, uint64_t Offset) {
  return Offset == 0 ? BaseAlignment : std::gcd(BaseAlignment, Offset);
}

llvm::Error verifyMemoryAccess(const llvm::Value *Pointer, llvm::Type *Type,
                               const llvm::Instruction &Instruction,
                               uint64_t StateSize, uint64_t RuntimeSize,
                               const llvm::DataLayout &Layout,
                               llvm::ArrayRef<TranslationIRMemorySlot> Slots,
                               uint64_t RequestedAlignment, bool IsWrite) {
  if (Pointer->getType()->getPointerAddressSpace() != 0)
    return failure(TranslationIRViolation::UnsupportedHostIROperation,
                   Instruction.getFunction(), "nonzero pointer address space");
  const PointerLocation Location = locatePointer(
      Pointer, *Instruction.getFunction(), Layout, StateSize, RuntimeSize);
  const llvm::TypeSize StoreSize = Layout.getTypeStoreSize(Type);
  if (StoreSize.isScalable())
    return failure(TranslationIRViolation::DirectGuestMemoryAccess,
                   Instruction.getFunction(), "scalable memory access");
  const uint64_t Width = StoreSize.getFixedValue();

  if (Location.Root == PointerRoot::Unknown)
    return failure(TranslationIRViolation::DirectGuestMemoryAccess,
                   Instruction.getFunction());
  if (!accessFits(Location.Offset, Width, Location.Extent)) {
    const TranslationIRViolation Reason =
        Location.Root == PointerRoot::Private
            ? TranslationIRViolation::UnboundedPrivateMemoryAccess
            : TranslationIRViolation::UnboundedStateOrRuntimeAccess;
    return failure(Reason, Instruction.getFunction());
  }
  if (Location.Root == PointerRoot::Private) {
    if (IsWrite)
      return failure(TranslationIRViolation::UnsupportedHostIROperation,
                     Instruction.getFunction(),
                     "write to a private constant object");
    const auto *Global = llvm::cast<llvm::GlobalVariable>(Location.Base);
    const uint64_t BaseAlignment = Global->getAlign().valueOrOne().value();
    if (RequestedAlignment >
        effectiveAlignment(BaseAlignment,
                           static_cast<uint64_t>(Location.Offset)))
      return failure(TranslationIRViolation::UnprovenUndefinedBehavior,
                     Instruction.getFunction(),
                     "private-memory alignment is not proven");
    return llvm::Error::success();
  }

  const TranslationIRMemoryRegion Region =
      Location.Root == PointerRoot::State ? TranslationIRMemoryRegion::State
                                          : TranslationIRMemoryRegion::Runtime;
  const TranslationIRMemoryAccess Required =
      IsWrite ? TranslationIRMemoryAccess::Write
              : TranslationIRMemoryAccess::Read;
  const uint64_t Offset = static_cast<uint64_t>(Location.Offset);
  const auto Key = std::pair{static_cast<uint8_t>(Region), Offset};
  auto Candidate = std::upper_bound(
      Slots.begin(), Slots.end(), Key,
      [](const auto &Left, const TranslationIRMemorySlot &Right) {
        const auto RightKey =
            std::pair{static_cast<uint8_t>(Right.Region), Right.Offset};
        return Left < RightKey;
      });
  if (Candidate == Slots.begin())
    return failure(TranslationIRViolation::MemorySlotNotAllowed,
                   Instruction.getFunction());
  --Candidate;
  const TranslationIRMemorySlot &Slot = *Candidate;
  if (Slot.Region != Region ||
      !hasTranslationIRMemoryAccess(Slot.Access, Required) ||
      Offset < Slot.Offset || Width > Slot.Size ||
      Offset - Slot.Offset > Slot.Size - Width)
    return failure(TranslationIRViolation::MemorySlotNotAllowed,
                   Instruction.getFunction());
  if (RequestedAlignment >
      effectiveAlignment(Slot.Alignment, Offset - Slot.Offset))
    return failure(TranslationIRViolation::UnprovenUndefinedBehavior,
                   Instruction.getFunction(),
                   "state/runtime alignment is not proven");
  return llvm::Error::success();
}

bool isIntegerAggregate(const llvm::Type *Type, unsigned MaxIntegerWidth) {
  if (Type->isIntegerTy())
    return Type->getIntegerBitWidth() <= MaxIntegerWidth;
  if (Type->isVoidTy())
    return true;
  const auto *Struct = llvm::dyn_cast<llvm::StructType>(Type);
  if (!Struct || Struct->isOpaque())
    return false;
  return llvm::all_of(Struct->elements(), [&](const llvm::Type *Element) {
    return isIntegerAggregate(Element, MaxIntegerWidth);
  });
}

bool containsUnsupportedScalarType(const llvm::Type *Type) {
  if (Type->isFloatingPointTy() || Type->isVectorTy())
    return true;
  if (const auto *Struct = llvm::dyn_cast<llvm::StructType>(Type))
    return llvm::any_of(Struct->elements(), [&](const llvm::Type *Element) {
      return containsUnsupportedScalarType(Element);
    });
  if (const auto *Array = llvm::dyn_cast<llvm::ArrayType>(Type))
    return containsUnsupportedScalarType(Array->getElementType());
  return false;
}

bool containsTooWideInteger(const llvm::Type *Type, unsigned MaxIntegerWidth) {
  if (Type->isIntegerTy())
    return Type->getIntegerBitWidth() > MaxIntegerWidth;
  if (const auto *Struct = llvm::dyn_cast<llvm::StructType>(Type))
    return llvm::any_of(Struct->elements(), [&](const llvm::Type *Element) {
      return containsTooWideInteger(Element, MaxIntegerWidth);
    });
  if (const auto *Array = llvm::dyn_cast<llvm::ArrayType>(Type))
    return containsTooWideInteger(Array->getElementType(), MaxIntegerWidth);
  return false;
}

bool isScalarIntegerStorageType(const llvm::Type *Type,
                                unsigned MaxIntegerWidth) {
  return Type->isIntegerTy() && Type->getIntegerBitWidth() <= MaxIntegerWidth;
}

class ConstantGraphFacts {
public:
  enum : uint8_t {
    None = 0,
    UndefOrPoison = 1u << 0,
    IntegerToPointer = 1u << 1,
    PointerToInteger = 1u << 2,
  };

  uint8_t get(const llvm::Constant *Root) {
    if (llvm::isa<llvm::GlobalValue>(Root))
      return None;
    if (const auto It = Cache.find(Root); It != Cache.end())
      return It->second;

    struct Frame {
      const llvm::Constant *Value;
      unsigned NextOperand = 0;
      uint8_t Facts = None;
    };
    llvm::SmallVector<Frame, 16> Worklist;
    llvm::SmallPtrSet<const llvm::Constant *, 16> Active;
    Active.insert(Root);
    Worklist.push_back({Root, 0, directFacts(Root)});

    while (!Worklist.empty()) {
      Frame &Current = Worklist.back();
      if (Current.NextOperand == Current.Value->getNumOperands()) {
        const llvm::Constant *Completed = Current.Value;
        const uint8_t Facts = Current.Facts;
        Cache[Completed] = Facts;
        Active.erase(Completed);
        Worklist.pop_back();
        if (!Worklist.empty())
          Worklist.back().Facts |= Facts;
        continue;
      }

      const llvm::Value *Operand =
          Current.Value->getOperand(Current.NextOperand++);
      const auto *Child = llvm::dyn_cast<llvm::Constant>(Operand);
      if (!Child || llvm::isa<llvm::GlobalValue>(Child))
        continue;
      if (const auto It = Cache.find(Child); It != Cache.end()) {
        Current.Facts |= It->second;
        continue;
      }
      if (!Active.insert(Child).second)
        continue;
      Worklist.push_back({Child, 0, directFacts(Child)});
    }
    return Cache.lookup(Root);
  }

private:
  static uint8_t directFacts(const llvm::Constant *Constant) {
    uint8_t Facts = None;
    if (llvm::isa<llvm::UndefValue, llvm::PoisonValue>(Constant))
      Facts |= UndefOrPoison;
    if (const auto *Expression = llvm::dyn_cast<llvm::ConstantExpr>(Constant)) {
      switch (Expression->getOpcode()) {
      case llvm::Instruction::IntToPtr:
        Facts |= IntegerToPointer;
        break;
      case llvm::Instruction::PtrToInt:
      case llvm::Instruction::PtrToAddr:
        Facts |= PointerToInteger;
        break;
      default:
        break;
      }
    }
    return Facts;
  }

  llvm::DenseMap<const llvm::Constant *, uint8_t> Cache;
};

bool isAllowedScalarIntrinsic(const llvm::Function &Function,
                              unsigned MaxIntegerWidth) {
  switch (Function.getIntrinsicID()) {
  case llvm::Intrinsic::bitreverse:
  case llvm::Intrinsic::bswap:
  case llvm::Intrinsic::ctlz:
  case llvm::Intrinsic::ctpop:
  case llvm::Intrinsic::cttz:
  case llvm::Intrinsic::fshl:
  case llvm::Intrinsic::fshr:
  case llvm::Intrinsic::sadd_sat:
  case llvm::Intrinsic::sadd_with_overflow:
  case llvm::Intrinsic::smax:
  case llvm::Intrinsic::smin:
  case llvm::Intrinsic::smul_with_overflow:
  case llvm::Intrinsic::ssub_sat:
  case llvm::Intrinsic::ssub_with_overflow:
  case llvm::Intrinsic::uadd_sat:
  case llvm::Intrinsic::uadd_with_overflow:
  case llvm::Intrinsic::umax:
  case llvm::Intrinsic::umin:
  case llvm::Intrinsic::umul_with_overflow:
  case llvm::Intrinsic::usub_sat:
  case llvm::Intrinsic::usub_with_overflow:
    break;
  default:
    return false;
  }

  const llvm::FunctionType *Type = Function.getFunctionType();
  if (!isIntegerAggregate(Type->getReturnType(), MaxIntegerWidth))
    return false;
  return llvm::all_of(Type->params(), [&](const llvm::Type *Parameter) {
    return Parameter->isIntegerTy() &&
           Parameter->getIntegerBitWidth() <= MaxIntegerWidth;
  });
}

using RuntimeHelperMap = llvm::StringMap<const TranslationRuntimeHelper *>;

llvm::Error
buildRuntimeHelperMap(const llvm::Module &Module,
                      llvm::ArrayRef<TranslationRuntimeHelper> RuntimeHelpers,
                      unsigned MaxIntegerWidth, RuntimeHelperMap &Map) {
  for (const TranslationRuntimeHelper &Helper : RuntimeHelpers) {
    if (Helper.Name.empty() || !Helper.Type)
      return failure(TranslationIRViolation::InvalidPolicy, nullptr,
                     "runtime helper requires a name and function type");
    if (&Helper.Type->getContext() != &Module.getContext())
      return failure(TranslationIRViolation::InvalidPolicy, nullptr,
                     "runtime helper type belongs to another LLVM context");
    if (Helper.Name.starts_with("llvm.") ||
        !Map.try_emplace(Helper.Name, &Helper).second)
      return failure(TranslationIRViolation::InvalidPolicy, nullptr,
                     "runtime helper name is reserved or duplicated");
    if (Helper.Type->isVarArg() ||
        Helper.Type->getNumParams() != Helper.Parameters.size() ||
        (!Helper.Type->getReturnType()->isVoidTy() &&
         (!Helper.Type->getReturnType()->isIntegerTy() ||
          Helper.Type->getReturnType()->getIntegerBitWidth() >
              MaxIntegerWidth)))
      return failure(TranslationIRViolation::InvalidPolicy, nullptr,
                     "runtime helper has a non-scalar ABI");

    for (unsigned Index = 0; Index != Helper.Parameters.size(); ++Index) {
      const llvm::Type *Parameter = Helper.Type->getParamType(Index);
      switch (Helper.Parameters[Index]) {
      case TranslationRuntimeParameterKind::ScalarInteger:
        if (!Parameter->isIntegerTy() ||
            Parameter->getIntegerBitWidth() > MaxIntegerWidth)
          return failure(TranslationIRViolation::InvalidPolicy, nullptr,
                         "scalar helper parameter is not an integer");
        break;
      case TranslationRuntimeParameterKind::StatePointer:
      case TranslationRuntimeParameterKind::RuntimePointer:
        if (!Parameter->isPointerTy() ||
            Parameter->getPointerAddressSpace() != 0)
          return failure(TranslationIRViolation::InvalidPolicy, nullptr,
                         "pointer helper parameter is not a pointer");
        break;
      default:
        return failure(TranslationIRViolation::InvalidPolicy, nullptr,
                       "runtime helper has an unknown parameter kind");
      }
    }
  }
  return llvm::Error::success();
}

llvm::Error
verifyRuntimeHelperDeclaration(const llvm::Function &Function,
                               const RuntimeHelperMap &RuntimeHelpers) {
  const auto It = RuntimeHelpers.find(Function.getName());
  if (It == RuntimeHelpers.end())
    return failure(TranslationIRViolation::ExternalSymbolNotAllowed, &Function);
  const TranslationRuntimeHelper &Helper = *It->second;
  if (Function.getFunctionType() != Helper.Type ||
      Function.getCallingConv() != llvm::CallingConv::C ||
      Function.getLinkage() != llvm::GlobalValue::ExternalLinkage ||
      !Function.hasDefaultVisibility() || !Function.doesNotThrow() ||
      Function.hasFnAttribute(llvm::Attribute::NoReturn) ||
      !hasOnlyNoUnwindAttributes(Function.getAttributes(),
                                 Function.arg_size()) ||
      hasForbiddenFunctionCodegenState(Function))
    return failure(TranslationIRViolation::RuntimeHelperABIMismatch, &Function);
  return llvm::Error::success();
}

llvm::Error verifyRuntimeHelperCall(const llvm::CallBase &Call,
                                    const TranslationRuntimeHelper &Helper,
                                    uint64_t StateSize, uint64_t RuntimeSize,
                                    const llvm::DataLayout &Layout) {
  if (Call.getFunctionType() != Helper.Type ||
      Call.getCallingConv() != llvm::CallingConv::C ||
      Call.arg_size() != Helper.Parameters.size() ||
      Call.hasFnAttr(llvm::Attribute::NoReturn) ||
      !hasOnlyNoUnwindAttributes(Call.getAttributes(), Call.arg_size()) ||
      Call.getNumOperandBundles() != 0)
    return failure(TranslationIRViolation::RuntimeHelperABIMismatch,
                   Call.getFunction());

  for (unsigned Index = 0; Index != Helper.Parameters.size(); ++Index) {
    const TranslationRuntimeParameterKind Kind = Helper.Parameters[Index];
    if (Kind == TranslationRuntimeParameterKind::ScalarInteger)
      continue;
    const PointerLocation Location =
        locatePointer(Call.getArgOperand(Index), *Call.getFunction(), Layout,
                      StateSize, RuntimeSize);
    const PointerRoot Expected =
        Kind == TranslationRuntimeParameterKind::StatePointer
            ? PointerRoot::State
            : PointerRoot::Runtime;
    if (Location.Root != Expected || Location.Offset != 0)
      return failure(TranslationIRViolation::RuntimeHelperABIMismatch,
                     Call.getFunction(),
                     "pointer argument has the wrong provenance");
  }
  return llvm::Error::success();
}

class ExplicitPoisonAnalysis {
public:
  ExplicitPoisonAnalysis(const llvm::Function &Function,
                         ConstantGraphFacts &ConstantFacts) {
    llvm::SmallVector<const llvm::Value *, 32> Worklist;
    auto Seed = [&](const llvm::Value *Value) {
      const auto *Constant = llvm::dyn_cast<llvm::Constant>(Value);
      const bool IsPoison =
          llvm::isa<llvm::UndefValue, llvm::PoisonValue>(Value) ||
          (Constant && (ConstantFacts.get(Constant) &
                        ConstantGraphFacts::UndefOrPoison) != 0);
      if (IsPoison && PoisonDependent.insert(Value).second)
        Worklist.push_back(Value);
    };

    for (const llvm::BasicBlock &Block : Function)
      for (const llvm::Instruction &Instruction : Block)
        for (const llvm::Use &Operand : Instruction.operands())
          Seed(Operand.get());

    while (!Worklist.empty()) {
      const llvm::Value *Value = Worklist.pop_back_val();
      for (const llvm::User *User : Value->users()) {
        const auto *Instruction = llvm::dyn_cast<llvm::Instruction>(User);
        if (!Instruction || Instruction->getFunction() != &Function ||
            llvm::isa<llvm::LoadInst, llvm::CallBase>(Instruction))
          continue;
        if (PoisonDependent.insert(Instruction).second)
          Worklist.push_back(Instruction);
      }
    }
  }

  bool contains(const llvm::Value *Value) const {
    return PoisonDependent.contains(Value);
  }

private:
  llvm::DenseSet<const llvm::Value *> PoisonDependent;
};

bool hasObservableUndefOrPoison(const llvm::Instruction &Instruction,
                                const ExplicitPoisonAnalysis &PoisonAnalysis) {
  auto Contains = [&](const llvm::Value *Value) {
    return PoisonAnalysis.contains(Value);
  };

  if (const auto *Return = llvm::dyn_cast<llvm::ReturnInst>(&Instruction))
    return Return->getReturnValue() && Contains(Return->getReturnValue());
  if (const auto *Branch = llvm::dyn_cast<llvm::CondBrInst>(&Instruction))
    return Contains(Branch->getCondition());
  if (const auto *Switch = llvm::dyn_cast<llvm::SwitchInst>(&Instruction))
    return Contains(Switch->getCondition());
  if (const auto *Indirect = llvm::dyn_cast<llvm::IndirectBrInst>(&Instruction))
    return Contains(Indirect->getAddress());
  if (const auto *Load = llvm::dyn_cast<llvm::LoadInst>(&Instruction))
    return Contains(Load->getPointerOperand());
  if (const auto *Store = llvm::dyn_cast<llvm::StoreInst>(&Instruction))
    return Contains(Store->getValueOperand()) ||
           Contains(Store->getPointerOperand());
  if (const auto *Call = llvm::dyn_cast<llvm::CallBase>(&Instruction)) {
    for (const llvm::Use &Argument : Call->args())
      if (Contains(Argument.get()))
        return true;
  }
  return false;
}

std::optional<TranslationIRViolation>
findForbiddenPointerConversion(const llvm::Value *Value,
                               ConstantGraphFacts &ConstantFacts) {
  const auto *Constant = llvm::dyn_cast<llvm::Constant>(Value);
  if (!Constant)
    return std::nullopt;
  const uint8_t Facts = ConstantFacts.get(Constant);
  if ((Facts & ConstantGraphFacts::IntegerToPointer) != 0)
    return TranslationIRViolation::GuestIntegerToPointer;
  if ((Facts & ConstantGraphFacts::PointerToInteger) != 0)
    return TranslationIRViolation::HostPointerExposure;
  return std::nullopt;
}

bool hasUnprovenSemanticMetadata(const llvm::Instruction &Instruction) {
  llvm::SmallVector<std::pair<unsigned, llvm::MDNode *>, 4> Metadata;
  Instruction.getAllMetadata(Metadata);
  return llvm::any_of(Metadata, [](const auto &Entry) {
    return Entry.first != llvm::LLVMContext::MD_dbg;
  });
}

bool hasUnsupportedScalarType(const llvm::Instruction &Instruction) {
  if (containsUnsupportedScalarType(Instruction.getType()))
    return true;
  for (const llvm::Use &Operand : Instruction.operands()) {
    if (llvm::isa<llvm::BasicBlock>(Operand.get()))
      continue;
    if (containsUnsupportedScalarType(Operand->getType()))
      return true;
  }
  return false;
}

bool hasTooWideInteger(const llvm::Instruction &Instruction,
                       unsigned MaxIntegerWidth) {
  if (containsTooWideInteger(Instruction.getType(), MaxIntegerWidth))
    return true;
  for (const llvm::Use &Operand : Instruction.operands()) {
    if (llvm::isa<llvm::BasicBlock>(Operand.get()))
      continue;
    if (containsTooWideInteger(Operand->getType(), MaxIntegerWidth))
      return true;
  }
  return false;
}

std::string renderInstruction(const llvm::Instruction &Instruction);

llvm::Error verifyTotalScalarOperation(const llvm::Instruction &Instruction,
                                       const llvm::DataLayout &Layout) {
  if (llvm::isa<llvm::FreezeInst>(Instruction))
    return failure(TranslationIRViolation::UnprovenUndefinedBehavior,
                   Instruction.getFunction(), renderInstruction(Instruction));
  const auto *Binary = llvm::dyn_cast<llvm::BinaryOperator>(&Instruction);
  if (!Binary)
    return llvm::Error::success();

  switch (Binary->getOpcode()) {
  case llvm::Instruction::Shl:
  case llvm::Instruction::LShr:
  case llvm::Instruction::AShr: {
    const llvm::KnownBits Amount = llvm::computeKnownBits(
        Binary->getOperand(1), Layout, nullptr, &Instruction);
    if (Amount.getMaxValue().getLimitedValue() >=
        Binary->getType()->getIntegerBitWidth())
      return failure(TranslationIRViolation::UnprovenUndefinedBehavior,
                     Instruction.getFunction(), renderInstruction(Instruction));
    break;
  }
  case llvm::Instruction::UDiv:
  case llvm::Instruction::URem:
  case llvm::Instruction::SDiv:
  case llvm::Instruction::SRem: {
    const llvm::KnownBits Divisor = llvm::computeKnownBits(
        Binary->getOperand(1), Layout, nullptr, &Instruction);
    if (Divisor.One.isZero())
      return failure(TranslationIRViolation::UnprovenUndefinedBehavior,
                     Instruction.getFunction(), renderInstruction(Instruction));
    if (Binary->getOpcode() != llvm::Instruction::SDiv &&
        Binary->getOpcode() != llvm::Instruction::SRem)
      break;

    const llvm::KnownBits Dividend = llvm::computeKnownBits(
        Binary->getOperand(0), Layout, nullptr, &Instruction);
    const unsigned Width = Binary->getType()->getIntegerBitWidth();
    const bool DivisorMayBeMinusOne = Divisor.Zero.isZero();
    const bool DividendMayBeSignedMinimum =
        !Dividend.Zero[Width - 1] &&
        Dividend.One.extractBits(Width - 1, 0).isZero();
    if (DivisorMayBeMinusOne && DividendMayBeSignedMinimum)
      return failure(TranslationIRViolation::UnprovenUndefinedBehavior,
                     Instruction.getFunction(), renderInstruction(Instruction));
    break;
  }
  default:
    break;
  }
  return llvm::Error::success();
}

bool isHostEHInstruction(const llvm::Instruction &Instruction) {
  return llvm::isa<llvm::InvokeInst, llvm::LandingPadInst, llvm::ResumeInst,
                   llvm::CatchSwitchInst, llvm::CatchPadInst,
                   llvm::CleanupPadInst, llvm::CatchReturnInst,
                   llvm::CleanupReturnInst>(Instruction);
}

std::string renderInstruction(const llvm::Instruction &Instruction) {
  std::string Text;
  llvm::raw_string_ostream Stream(Text);
  Instruction.print(Stream);
  Stream.flush();
  return Text;
}

} // namespace

llvm::Error verifyTranslationIR(
    const llvm::Module &Module, const llvm::Triple &ExpectedHostTriple,
    const llvm::DataLayout &ExpectedHostDataLayout, uint64_t StateSize,
    uint64_t RuntimeSize, llvm::ArrayRef<TranslationIRMemorySlot> MemorySlots,
    llvm::ArrayRef<TranslationRuntimeHelper> RuntimeHelpers) {
  if (StateSize == 0 || RuntimeSize == 0)
    return failure(TranslationIRViolation::InvalidPolicy, nullptr,
                   "state and runtime extents must be nonzero");
  const std::optional<unsigned> MaxIntegerWidth =
      maxLegalIntegerWidth(ExpectedHostTriple);
  if (!MaxIntegerWidth)
    return failure(TranslationIRViolation::InvalidPolicy, nullptr,
                   "unsupported host architecture");
  std::vector<TranslationIRMemorySlot> SortedMemorySlots;
  if (llvm::Error Error = validateMemorySlots(MemorySlots, StateSize,
                                              RuntimeSize, SortedMemorySlots))
    return Error;

  RuntimeHelperMap RuntimeHelperPolicies;
  if (llvm::Error Error = buildRuntimeHelperMap(
          Module, RuntimeHelpers, *MaxIntegerWidth, RuntimeHelperPolicies))
    return Error;

  const std::string ActualTriple = Module.getTargetTriple().normalize();
  const std::string ExpectedTriple = ExpectedHostTriple.normalize();
  if (ActualTriple != ExpectedTriple)
    return failure(TranslationIRViolation::HostTripleMismatch, nullptr,
                   Module.getTargetTriple().str());
  if (Module.getDataLayout() != ExpectedHostDataLayout)
    return failure(TranslationIRViolation::HostDataLayoutMismatch, nullptr,
                   Module.getDataLayoutStr());

  std::string LLVMVerification;
  llvm::raw_string_ostream VerificationStream(LLVMVerification);
  if (llvm::verifyModule(Module, &VerificationStream)) {
    VerificationStream.flush();
    return failure(TranslationIRViolation::InvalidLLVMIR, nullptr,
                   LLVMVerification);
  }
  if (!Module.getModuleInlineAsm().empty())
    return failure(TranslationIRViolation::InlineAssembly, nullptr,
                   "module-level assembly");

  if (!Module.alias_empty() || !Module.ifunc_empty())
    return failure(TranslationIRViolation::ExternalSymbolNotAllowed, nullptr,
                   "aliases and indirect functions are forbidden");
  if (!Module.named_metadata_empty())
    return failure(TranslationIRViolation::UnprovenSemanticMetadata, nullptr,
                   "named metadata and module flags are forbidden");

  ConstantGraphFacts ConstantFacts;
  for (const llvm::GlobalVariable &Global : Module.globals()) {
    if (!Global.isConstant())
      return failure(TranslationIRViolation::MutableGlobalState, nullptr,
                     Global.getName());
    if (Global.hasMetadataOtherThanDebugLoc())
      return failure(TranslationIRViolation::UnprovenSemanticMetadata, nullptr,
                     Global.getName());
    if (!Global.hasLocalLinkage() || Global.isDeclaration() ||
        !Global.hasInitializer() || Global.isThreadLocal() ||
        Global.getAddressSpace() != 0 || Global.hasSection() ||
        Global.hasComdat() || Global.hasPartition() || Global.hasAttributes() ||
        Global.getCodeModel().has_value() || Global.isExternallyInitialized() ||
        !isScalarIntegerStorageType(Global.getValueType(), *MaxIntegerWidth))
      return failure(TranslationIRViolation::ExternalSymbolNotAllowed, nullptr,
                     Global.getName());
    if ((ConstantFacts.get(Global.getInitializer()) &
         ConstantGraphFacts::UndefOrPoison) != 0)
      return failure(TranslationIRViolation::GuestObservableUndefOrPoison,
                     nullptr, Global.getName());
    if (std::optional<TranslationIRViolation> Reason =
            findForbiddenPointerConversion(Global.getInitializer(),
                                           ConstantFacts))
      return failure(*Reason, nullptr, Global.getName());
  }

  for (const llvm::Function &Function : Module) {
    if (Function.hasMetadataOtherThanDebugLoc())
      return failure(TranslationIRViolation::UnprovenSemanticMetadata,
                     &Function);
    if (Function.hasPersonalityFn())
      return failure(TranslationIRViolation::HostExceptionHandling, &Function,
                     "personality function");
  }

  for (const llvm::Function &Function : Module) {
    if (Function.isTargetIntrinsic())
      return failure(TranslationIRViolation::TargetSpecificIntrinsic,
                     &Function);
    if (Function.isIntrinsic()) {
      const llvm::AttributeList CanonicalAttributes =
          llvm::Intrinsic::getAttributes(Module.getContext(),
                                         Function.getIntrinsicID(),
                                         Function.getFunctionType());
      if (!Function.isDeclaration() ||
          Function.getLinkage() != llvm::GlobalValue::ExternalLinkage ||
          !Function.hasDefaultVisibility() ||
          Function.getCallingConv() != llvm::CallingConv::C ||
          Function.getAttributes() != CanonicalAttributes ||
          hasForbiddenFunctionCodegenState(Function) ||
          !isAllowedScalarIntrinsic(Function, *MaxIntegerWidth))
        return failure(TranslationIRViolation::IntrinsicNotAllowed, &Function);
      continue;
    }
    if (Function.isDeclaration()) {
      if (llvm::Error Error =
              verifyRuntimeHelperDeclaration(Function, RuntimeHelperPolicies))
        return Error;
      continue;
    }
    if (RuntimeHelperPolicies.contains(Function.getName()))
      return failure(TranslationIRViolation::RuntimeHelperABIMismatch,
                     &Function, "runtime helper must remain a declaration");
    if (!isCanonicalBlockABI(Function))
      return failure(TranslationIRViolation::NonStandardBlockABI, &Function);
    if (!Function.doesNotThrow())
      return failure(TranslationIRViolation::HostExceptionHandling, &Function,
                     "function is not nounwind");
    if (Function.hasFnAttribute(llvm::Attribute::NoReturn))
      return failure(TranslationIRViolation::NonStandardBlockABI, &Function,
                     "translated block is marked noreturn");
    if (!hasOnlyNoUnwindAttributes(Function.getAttributes(),
                                   Function.arg_size()) ||
        hasForbiddenFunctionCodegenState(Function))
      return failure(TranslationIRViolation::UnsupportedHostIROperation,
                     &Function,
                     "function attributes or codegen state are not allowed");
    if (hasControlFlowCycle(Function))
      return failure(TranslationIRViolation::DispatchCycle, &Function);

    ExplicitPoisonAnalysis PoisonAnalysis(Function, ConstantFacts);
    for (const llvm::BasicBlock &Block : Function) {
      for (const llvm::Instruction &Instruction : Block) {
        if (isHostEHInstruction(Instruction))
          return failure(TranslationIRViolation::HostExceptionHandling,
                         &Function, renderInstruction(Instruction));
        if (hasUnprovenSemanticMetadata(Instruction))
          return failure(TranslationIRViolation::UnprovenSemanticMetadata,
                         &Function, renderInstruction(Instruction));
        if (hasUnsupportedScalarType(Instruction))
          return failure(TranslationIRViolation::UnsupportedHostIROperation,
                         &Function, renderInstruction(Instruction));
        if (hasTooWideInteger(Instruction, *MaxIntegerWidth))
          return failure(TranslationIRViolation::BackendLibcallRisk, &Function,
                         renderInstruction(Instruction));
        if (llvm::isa<llvm::AllocaInst, llvm::AtomicRMWInst,
                      llvm::AtomicCmpXchgInst, llvm::FenceInst, llvm::VAArgInst,
                      llvm::UnreachableInst, llvm::IndirectBrInst,
                      llvm::CallBrInst>(Instruction))
          return failure(TranslationIRViolation::UnsupportedHostIROperation,
                         &Function, renderInstruction(Instruction));
        if (llvm::Error Error =
                verifyTotalScalarOperation(Instruction, ExpectedHostDataLayout))
          return Error;
        if (hasObservableUndefOrPoison(Instruction, PoisonAnalysis))
          return failure(TranslationIRViolation::GuestObservableUndefOrPoison,
                         &Function, renderInstruction(Instruction));
        if (Instruction.hasPoisonGeneratingAnnotations() &&
            !Instruction.use_empty() &&
            !llvm::isGuaranteedNotToBePoison(&Instruction, nullptr,
                                             &Instruction))
          return failure(TranslationIRViolation::UnprovenPoisonGeneratingFlag,
                         &Function, renderInstruction(Instruction));

        for (const llvm::Use &Operand : Instruction.operands()) {
          if (std::optional<TranslationIRViolation> Reason =
                  findForbiddenPointerConversion(Operand.get(), ConstantFacts))
            return failure(*Reason, &Function, renderInstruction(Instruction));
        }
        if (llvm::isa<llvm::PtrToIntInst, llvm::PtrToAddrInst>(Instruction)) {
          return failure(TranslationIRViolation::HostPointerExposure, &Function,
                         renderInstruction(Instruction));
        }
        if (const auto *IntToPtr =
                llvm::dyn_cast<llvm::IntToPtrInst>(&Instruction)) {
          return failure(TranslationIRViolation::GuestIntegerToPointer,
                         &Function, renderInstruction(*IntToPtr));
        }

        if (const auto *Load = llvm::dyn_cast<llvm::LoadInst>(&Instruction)) {
          if (Load->getType()->isPointerTy())
            return failure(TranslationIRViolation::HostPointerExposure,
                           &Function, renderInstruction(Instruction));
          if (!Load->getType()->isIntegerTy())
            return failure(TranslationIRViolation::UnsupportedHostIROperation,
                           &Function, renderInstruction(Instruction));
          if (Load->isAtomic() || Load->isVolatile())
            return failure(TranslationIRViolation::UnsupportedHostIROperation,
                           &Function, renderInstruction(Instruction));
        }
        if (const auto *Store = llvm::dyn_cast<llvm::StoreInst>(&Instruction)) {
          if (Store->getValueOperand()->getType()->isPointerTy())
            return failure(TranslationIRViolation::HostPointerExposure,
                           &Function, renderInstruction(Instruction));
          if (!Store->getValueOperand()->getType()->isIntegerTy())
            return failure(TranslationIRViolation::UnsupportedHostIROperation,
                           &Function, renderInstruction(Instruction));
          if (Store->isAtomic() || Store->isVolatile())
            return failure(TranslationIRViolation::UnsupportedHostIROperation,
                           &Function, renderInstruction(Instruction));
        }
        if (const auto *Compare =
                llvm::dyn_cast<llvm::ICmpInst>(&Instruction)) {
          if (Compare->getOperand(0)->getType()->isPointerTy())
            return failure(TranslationIRViolation::HostPointerExposure,
                           &Function, renderInstruction(Instruction));
        }
        if (Instruction.getType()->isPointerTy() &&
            !llvm::isa<llvm::GetElementPtrInst>(Instruction))
          return failure(TranslationIRViolation::HostPointerExposure, &Function,
                         renderInstruction(Instruction));

        if (const auto *GEP =
                llvm::dyn_cast<llvm::GetElementPtrInst>(&Instruction)) {
          const PointerLocation Location = locatePointer(
              GEP, Function, ExpectedHostDataLayout, StateSize, RuntimeSize);
          if (Location.Root == PointerRoot::Unknown)
            return failure(TranslationIRViolation::DirectGuestMemoryAccess,
                           &Function, renderInstruction(Instruction));
          if (Location.Offset < 0 ||
              static_cast<uint64_t>(Location.Offset) > Location.Extent) {
            const TranslationIRViolation Reason =
                Location.Root == PointerRoot::Private
                    ? TranslationIRViolation::UnboundedPrivateMemoryAccess
                    : TranslationIRViolation::UnboundedStateOrRuntimeAccess;
            return failure(Reason, &Function, renderInstruction(Instruction));
          }
        }

        if (const auto *Load = llvm::dyn_cast<llvm::LoadInst>(&Instruction)) {
          if (llvm::Error Error = verifyMemoryAccess(
                  Load->getPointerOperand(), Load->getType(), Instruction,
                  StateSize, RuntimeSize, ExpectedHostDataLayout,
                  SortedMemorySlots, Load->getAlign().value(), false))
            return Error;
        } else if (const auto *Store =
                       llvm::dyn_cast<llvm::StoreInst>(&Instruction)) {
          if (llvm::Error Error = verifyMemoryAccess(
                  Store->getPointerOperand(),
                  Store->getValueOperand()->getType(), Instruction, StateSize,
                  RuntimeSize, ExpectedHostDataLayout, SortedMemorySlots,
                  Store->getAlign().value(), true))
            return Error;
        }

        const auto *Call = llvm::dyn_cast<llvm::CallBase>(&Instruction);
        if (!Call)
          continue;
        if (Call->isInlineAsm())
          return failure(TranslationIRViolation::InlineAssembly, &Function,
                         renderInstruction(Instruction));
        if (Call->getCallingConv() != llvm::CallingConv::C ||
            Call->getNumOperandBundles() != 0 ||
            !hasOnlyNoUnwindAttributes(Call->getAttributes(), Call->arg_size()))
          return failure(TranslationIRViolation::UnsupportedHostIROperation,
                         &Function,
                         "call attributes or operand bundles are forbidden");
        const llvm::Function *Callee = Call->getCalledFunction();
        if (!Callee)
          return failure(TranslationIRViolation::ExternalSymbolNotAllowed,
                         &Function, "indirect call");
        if (Callee->isTargetIntrinsic())
          return failure(TranslationIRViolation::TargetSpecificIntrinsic,
                         &Function, Callee->getName());
        if (!Call->doesNotThrow())
          return failure(TranslationIRViolation::HostExceptionHandling,
                         &Function, "call is not nounwind");
        if (Callee->isIntrinsic()) {
          if (!isAllowedScalarIntrinsic(*Callee, *MaxIntegerWidth))
            return failure(TranslationIRViolation::IntrinsicNotAllowed,
                           &Function, Callee->getName());
          if ((Callee->getIntrinsicID() == llvm::Intrinsic::ctlz ||
               Callee->getIntrinsicID() == llvm::Intrinsic::cttz) &&
              (Call->arg_size() < 2 ||
               !llvm::PatternMatch::match(Call->getArgOperand(1),
                                          llvm::PatternMatch::m_Zero())))
            return failure(TranslationIRViolation::UnprovenUndefinedBehavior,
                           &Function, Callee->getName());
          if (const auto *CallInstruction =
                  llvm::dyn_cast<llvm::CallInst>(&Instruction);
              CallInstruction && CallInstruction->isTailCall())
            return failure(TranslationIRViolation::IntrinsicNotAllowed,
                           &Function, "intrinsic call cannot be tail");
          continue;
        }
        if (!Callee->isDeclaration())
          return failure(TranslationIRViolation::DirectBlockCall, &Function,
                         Callee->getName());
        const auto Helper = RuntimeHelperPolicies.find(Callee->getName());
        if (Helper == RuntimeHelperPolicies.end())
          return failure(TranslationIRViolation::ExternalSymbolNotAllowed,
                         &Function, Callee->getName());
        if (const auto *CallInstruction =
                llvm::dyn_cast<llvm::CallInst>(&Instruction);
            CallInstruction && CallInstruction->isTailCall())
          return failure(TranslationIRViolation::RuntimeHelperABIMismatch,
                         &Function, "runtime helper call cannot be tail");
        if (llvm::Error Error =
                verifyRuntimeHelperCall(*Call, *Helper->second, StateSize,
                                        RuntimeSize, ExpectedHostDataLayout))
          return Error;
      }
    }
  }

  return llvm::Error::success();
}

} // namespace neverd::translate
