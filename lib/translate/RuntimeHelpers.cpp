//===- RuntimeHelpers.cpp - Sealed translation helper boundary -----------===//

#include "neverd/translate/RuntimeHelpers.h"

#include "neverd/translate/GuestMemoryRuntime.h"
#include "neverd/translate/RuntimeGuestState.h"
#include "neverd/translate/TranslationIRVerifier.h"

#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/Analysis/ValueTracking.h"
#include "llvm/IR/CFG.h"
#include "llvm/IR/DataLayout.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/PatternMatch.h"
#include "llvm/Support/Errc.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/TargetParser/Triple.h"

#include <cstdint>
#include <limits>
#include <optional>

namespace neverd::translate {
namespace {

llvm::Error invalid(llvm::StringRef Message) {
  return llvm::createStringError(llvm::errc::invalid_argument, "%s",
                                 Message.str().c_str());
}

llvm::Error protocolFailure(const llvm::Function *Function,
                            llvm::StringRef Detail) {
  return llvm::make_error<TranslationIRVerificationError>(
      TranslationIRViolation::RuntimeProtocolViolation,
      Function ? Function->getName().str() : std::string{}, Detail.str());
}

bool isDebugInstruction(const llvm::Instruction &Instruction) {
  return llvm::isa<llvm::DbgInfoIntrinsic>(Instruction);
}

const llvm::Instruction *
nextProtocolInstruction(const llvm::Instruction &Instruction) {
  for (const llvm::Instruction *Next = Instruction.getNextNode(); Next;
       Next = Next->getNextNode())
    if (!isDebugInstruction(*Next))
      return Next;
  return nullptr;
}

bool hasUniquePredecessor(const llvm::BasicBlock &Block,
                          const llvm::BasicBlock &Predecessor) {
  auto Begin = llvm::pred_begin(&Block);
  const auto End = llvm::pred_end(&Block);
  return Begin != End && *Begin == &Predecessor && ++Begin == End;
}

struct ExactFailureEdge {
  const llvm::ReturnInst *Return = nullptr;
  const llvm::Value *StatusUse = nullptr;
};

std::optional<ExactFailureEdge>
matchExactFailureEdge(const llvm::BasicBlock &Block,
                      const llvm::BasicBlock &Predecessor,
                      const llvm::CallInst &Status) {
  const llvm::ReturnInst *Return = nullptr;
  const llvm::PHINode *ReturnPhi = nullptr;
  for (const llvm::Instruction &Instruction : Block) {
    if (isDebugInstruction(Instruction))
      continue;
    if (const auto *Phi = llvm::dyn_cast<llvm::PHINode>(&Instruction)) {
      if (Return || ReturnPhi)
        return std::nullopt;
      ReturnPhi = Phi;
      continue;
    }
    if (Return || !(Return = llvm::dyn_cast<llvm::ReturnInst>(&Instruction)))
      return std::nullopt;
  }
  if (!Return)
    return std::nullopt;
  if (!ReturnPhi)
    return Return->getReturnValue() == &Status &&
                   hasUniquePredecessor(Block, Predecessor)
               ? std::optional<ExactFailureEdge>{{Return, Return}}
               : std::nullopt;
  if (Return->getReturnValue() != ReturnPhi || !ReturnPhi->hasOneUse() ||
      ReturnPhi->user_back() != Return)
    return std::nullopt;

  unsigned MatchingEdges = 0;
  for (unsigned Index = 0; Index != ReturnPhi->getNumIncomingValues(); ++Index)
    if (ReturnPhi->getIncomingBlock(Index) == &Predecessor) {
      ++MatchingEdges;
      if (ReturnPhi->getIncomingValue(Index) != &Status)
        return std::nullopt;
    }
  if (MatchingEdges != 1)
    return std::nullopt;
  return ExactFailureEdge{Return, ReturnPhi};
}

struct RuntimeLoadLocation {
  bool IsRuntime = false;
  int64_t Offset = 0;
  uint64_t Size = 0;
};

RuntimeLoadLocation locateRuntimeLoad(const llvm::LoadInst &Load,
                                      const llvm::Argument &Runtime,
                                      const llvm::DataLayout &Layout) {
  RuntimeLoadLocation Location;
  const llvm::Value *Base = llvm::GetPointerBaseWithConstantOffset(
      Load.getPointerOperand(), Location.Offset, Layout);
  Location.IsRuntime = Base == &Runtime;
  const llvm::TypeSize Size = Layout.getTypeStoreSize(Load.getType());
  if (!Size.isScalable())
    Location.Size = Size.getFixedValue();
  return Location;
}

bool overlapsScalarResult(const RuntimeLoadLocation &Location) {
  if (!Location.IsRuntime || Location.Size == 0)
    return false;
  constexpr int64_t ResultBegin =
      static_cast<int64_t>(offsetof(RuntimeControlBlockV1, ScalarResult));
  constexpr int64_t ResultEnd = ResultBegin + sizeof(uint64_t);
  if (Location.Offset >= ResultEnd)
    return false;
  if (Location.Size >
          static_cast<uint64_t>(std::numeric_limits<int64_t>::max()) ||
      Location.Offset > std::numeric_limits<int64_t>::max() -
                            static_cast<int64_t>(Location.Size))
    return true;
  return Location.Offset + static_cast<int64_t>(Location.Size) > ResultBegin;
}

bool isCanonicalScalarResultLoad(const llvm::LoadInst &Load,
                                 const llvm::Argument &Runtime,
                                 const llvm::DataLayout &Layout) {
  const RuntimeLoadLocation Location = locateRuntimeLoad(Load, Runtime, Layout);
  return Location.IsRuntime &&
         Location.Offset == static_cast<int64_t>(offsetof(RuntimeControlBlockV1,
                                                          ScalarResult)) &&
         Location.Size == sizeof(uint64_t) && Load.getType()->isIntegerTy(64) &&
         !Load.isAtomic() && !Load.isVolatile() &&
         Load.getAlign() >= llvm::Align(alignof(uint64_t));
}

const llvm::LoadInst *
findImmediateScalarResultLoad(const llvm::BasicBlock &Success,
                              const llvm::Argument &Runtime,
                              const llvm::DataLayout &Layout) {
  for (const llvm::Instruction &Instruction : Success) {
    if (isDebugInstruction(Instruction) ||
        llvm::isa<llvm::GetElementPtrInst>(Instruction))
      continue;
    auto *Load = llvm::dyn_cast<llvm::LoadInst>(&Instruction);
    if (Load && isCanonicalScalarResultLoad(*Load, Runtime, Layout))
      return Load;
    return nullptr;
  }
  return nullptr;
}

uint32_t failInvalidFrame(RuntimeCallFrameV1 *Frame,
                          RuntimeMemoryAccessKindV1 Access,
                          GuestScalarWidth Width,
                          uint32_t RequiredAlignment) noexcept {
  if (Frame) {
    Frame->Control = makeRuntimeControlBlockV1(
        CodeInvalidationPolicy::InvalidateOnExecutableWrite);
    Frame->Control.Exit.Kind = RuntimeABIExitKindV1::MemoryFault;
    Frame->Control.Exit.Fault = RuntimeMemoryFaultKindV1::InvalidRuntimeFrame;
    Frame->Control.Exit.Size = static_cast<uint64_t>(Width);
    llvm::Expected<RuntimeMemoryFaultDetailEncodingV1> Details =
        packRuntimeMemoryFaultDetailsV1(Frame->Control.Exit.Fault,
                                        {Access, RequiredAlignment,
                                         /*ExpectedGeneration=*/0,
                                         /*ObservedGeneration=*/0});
    if (Details) {
      Frame->Control.Exit.Detail0 = Details->Detail0;
      Frame->Control.Exit.Detail1 = Details->Detail1;
    } else {
      llvm::consumeError(Details.takeError());
      Frame->Control.Exit.Detail0 = static_cast<uint64_t>(Access);
    }
  }
  return static_cast<uint32_t>(RuntimeABIExitKindV1::MemoryFault);
}

uint32_t finish(RuntimeCallFrameV1 &Frame) noexcept {
  Frame.Control = Frame.Memory->snapshotControlBlock();
  if (Frame.Control.Exit.Kind == RuntimeABIExitKindV1::SelfModification)
    Frame.Validated = {};
  return static_cast<uint32_t>(Frame.Control.Exit.Kind);
}

uint32_t load(void *Opaque, uint64_t Address, uint32_t RequiredAlignment,
              GuestScalarWidth Width) noexcept {
  auto *Frame = static_cast<RuntimeCallFrameV1 *>(Opaque);
  if (!Frame || !Frame->Memory)
    return failInvalidFrame(Frame, RuntimeMemoryAccessKindV1::Read, Width,
                            RequiredAlignment);
  Frame->Memory->loadScalar(Address, Width, RequiredAlignment);
  return finish(*Frame);
}

uint32_t store(void *Opaque, uint64_t Address, uint64_t Value,
               uint32_t RequiredAlignment, GuestScalarWidth Width) noexcept {
  auto *Frame = static_cast<RuntimeCallFrameV1 *>(Opaque);
  if (!Frame || !Frame->Memory)
    return failInvalidFrame(Frame, RuntimeMemoryAccessKindV1::Write, Width,
                            RequiredAlignment);
  Frame->Memory->storeScalar(Address, Width, Value, RequiredAlignment);
  return finish(*Frame);
}

const RuntimeABIHelperBindingV1 HelperBindings[] = {
    {"nvd_rt_v1_load8_le", RuntimeABIHelperClassV1::Load, &nvd_rt_v1_load8_le,
     nullptr},
    {"nvd_rt_v1_load16_le", RuntimeABIHelperClassV1::Load, &nvd_rt_v1_load16_le,
     nullptr},
    {"nvd_rt_v1_load32_le", RuntimeABIHelperClassV1::Load, &nvd_rt_v1_load32_le,
     nullptr},
    {"nvd_rt_v1_load64_le", RuntimeABIHelperClassV1::Load, &nvd_rt_v1_load64_le,
     nullptr},
    {"nvd_rt_v1_store8_le", RuntimeABIHelperClassV1::Store, nullptr,
     &nvd_rt_v1_store8_le},
    {"nvd_rt_v1_store16_le", RuntimeABIHelperClassV1::Store, nullptr,
     &nvd_rt_v1_store16_le},
    {"nvd_rt_v1_store32_le", RuntimeABIHelperClassV1::Store, nullptr,
     &nvd_rt_v1_store32_le},
    {"nvd_rt_v1_store64_le", RuntimeABIHelperClassV1::Store, nullptr,
     &nvd_rt_v1_store64_le},
};

} // namespace

llvm::Error
validateRuntimeCodeCredentialV1(const RuntimeCodeCredentialV1 &Published,
                                const RuntimeCodeCredentialV1 &Validated) {
  if (Published.SessionID == 0 || Published.BlockID == 0 ||
      Published.CacheGeneration == 0 || Published.CodeEpoch == 0)
    return invalid("runtime code credential is not bound");
  if (Published != Validated)
    return invalid("runtime code credential is stale");
  return llvm::Error::success();
}

llvm::Error
validateRuntimeControlBlockAfterInvocationV1(const RuntimeControlBlockV1 &Block,
                                             uint32_t Status) {
  if (Status < kBlockExitKindBaseV1) {
    if (Status == static_cast<uint32_t>(RuntimeABIExitKindV1::None) ||
        Status > static_cast<uint32_t>(RuntimeABIExitKindV1::Cancelled))
      return invalid("translated block returned an unknown runtime status");
    if (llvm::Error Error = validateRuntimeControlBlockV1(Block))
      return Error;
    if (Status != static_cast<uint32_t>(Block.Exit.Kind))
      return invalid(
          "translated runtime status disagrees with the control exit");
    return llvm::Error::success();
  }

  if (Status > static_cast<uint32_t>(BlockExitKindV1::Trap))
    return invalid("translated block returned an unknown block-exit status");
  if (Block.Exit.Kind != RuntimeABIExitKindV1::None)
    return invalid("block-exit status carries a runtime-service exit");

  if (llvm::Error Strict = validateRuntimeControlBlockV1(Block)) {
    llvm::consumeError(std::move(Strict));
  } else {
    return llvm::Error::success();
  }

  if (Block.CodeInvalidation !=
          static_cast<uint32_t>(
              CodeInvalidationPolicy::ValidateBeforeDispatch) ||
      Block.Flags != 0 || Block.CurrentPC != 0 ||
      Block.ExpectedGeneration != 0 || Block.ObservedGeneration != 0)
    return invalid(
        "block exit has neither a valid nor a consumed dispatch proof");

  RuntimeControlBlockV1 ConsumedProof = Block;
  ConsumedProof.CodeInvalidation = static_cast<uint32_t>(
      CodeInvalidationPolicy::InvalidateOnExecutableWrite);
  return validateRuntimeControlBlockV1(ConsumedProof);
}

llvm::Expected<RuntimeCallFrameV1>
createRuntimeCallFrameV1(GuestMemoryRuntime &Memory,
                         const RuntimeCodeCredentialV1 &Published,
                         const RuntimeCodeCredentialV1 &Validated,
                         uint64_t CurrentPC, uint64_t ExpectedGeneration) {
  if (llvm::Error Error = validateRuntimeCodeCredentialV1(Published, Validated))
    return std::move(Error);
  if (Memory.codeInvalidationPolicy() !=
          CodeInvalidationPolicy::ValidateBeforeDispatch &&
      (CurrentPC != 0 || ExpectedGeneration != 0))
    return invalid("runtime call frame carries generation context for a "
                   "non-validating policy");
  RuntimeCallFrameV1 Frame;
  Frame.Control = Memory.snapshotControlBlock(CurrentPC, ExpectedGeneration);
  if (llvm::Error Error = validateRuntimeControlBlockV1(Frame.Control))
    return std::move(Error);
  Frame.Memory = &Memory;
  Frame.Published = Published;
  Frame.Validated = Validated;
  return Frame;
}

llvm::ArrayRef<RuntimeABIHelperBindingV1> runtimeABIHelperBindingsV1() {
  return HelperBindings;
}

const RuntimeABIHelperBindingV1 *
findRuntimeABIHelperBindingV1(llvm::StringRef Name) {
  for (const RuntimeABIHelperBindingV1 &Binding : HelperBindings)
    if (Binding.Name == Name)
      return &Binding;
  return nullptr;
}

llvm::Error verifyRuntimeABIHelperProtocolV1(const llvm::Module &Module) {
  llvm::SmallPtrSet<const llvm::LoadInst *, 16> AuthorizedResultLoads;
  const llvm::DataLayout &Layout = Module.getDataLayout();

  for (const llvm::Function &ConstFunction : Module) {
    if (ConstFunction.isDeclaration())
      continue;
    const llvm::Function &Function = ConstFunction;
    const llvm::Argument *Runtime =
        Function.arg_size() >= 2 ? Function.getArg(1) : nullptr;

    for (const llvm::BasicBlock &Block : Function) {
      for (const llvm::Instruction &Instruction : Block) {
        const auto *CallBase = llvm::dyn_cast<llvm::CallBase>(&Instruction);
        if (!CallBase)
          continue;
        const llvm::Value *Called =
            CallBase->getCalledOperand()->stripPointerCasts();
        const auto *Callee = llvm::dyn_cast<llvm::Function>(Called);
        if (!Callee)
          continue;
        const RuntimeABIHelperBindingV1 *Binding =
            findRuntimeABIHelperBindingV1(Callee->getName());
        if (!Binding)
          continue;

        const auto *Call = llvm::dyn_cast<llvm::CallInst>(CallBase);
        if (!Call || !Runtime || !Call->getType()->isIntegerTy(32) ||
            Call->arg_empty() ||
            Call->getArgOperand(0)->stripPointerCasts() != Runtime)
          return protocolFailure(
              &Function,
              "runtime helper is not a direct i32 call on the runtime frame");

        const auto *Compare = llvm::dyn_cast_or_null<llvm::ICmpInst>(
            nextProtocolInstruction(*Call));
        if (!Compare || Compare->getPredicate() != llvm::ICmpInst::ICMP_EQ)
          return protocolFailure(
              &Function,
              "runtime helper status is not immediately compared with zero");
        const bool CanonicalCompare =
            (Compare->getOperand(0) == Call &&
             llvm::PatternMatch::match(Compare->getOperand(1),
                                       llvm::PatternMatch::m_Zero())) ||
            (Compare->getOperand(1) == Call &&
             llvm::PatternMatch::match(Compare->getOperand(0),
                                       llvm::PatternMatch::m_Zero()));
        if (!CanonicalCompare)
          return protocolFailure(
              &Function, "runtime helper status comparison is not canonical");

        const auto *Branch = llvm::dyn_cast_or_null<llvm::CondBrInst>(
            nextProtocolInstruction(*Compare));
        if (!Branch || Branch->getCondition() != Compare ||
            Branch->getSuccessor(0) == Branch->getSuccessor(1))
          return protocolFailure(
              &Function,
              "runtime helper status does not immediately control dispatch");
        for (const llvm::User *User : Compare->users())
          if (User != Branch)
            return protocolFailure(
                &Function,
                "runtime helper status predicate has an additional consumer");

        const llvm::BasicBlock &Success = *Branch->getSuccessor(0);
        const llvm::BasicBlock &Failure = *Branch->getSuccessor(1);
        const std::optional<ExactFailureEdge> FailureEdge =
            matchExactFailureEdge(Failure, Block, *Call);
        if (!hasUniquePredecessor(Success, Block) || !FailureEdge) {
          std::string Detail;
          llvm::raw_string_ostream Stream(Detail);
          Stream << "runtime helper failure edge is not an exact status "
                    "return; dispatch block: ";
          Block.print(Stream);
          Stream << "; failure block: ";
          Failure.print(Stream);
          return protocolFailure(&Function, Stream.str());
        }

        for (const llvm::User *User : Call->users())
          if (User != Compare && User != FailureEdge->StatusUse)
            return protocolFailure(
                &Function, "runtime helper status has an additional consumer");

        if (Binding->Class == RuntimeABIHelperClassV1::Load) {
          const llvm::LoadInst *Result =
              findImmediateScalarResultLoad(Success, *Runtime, Layout);
          if (!Result)
            return protocolFailure(
                &Function,
                "runtime load result is not read first on the success edge");
          AuthorizedResultLoads.insert(Result);
        }
      }
    }

    if (!Runtime)
      continue;
    for (const llvm::BasicBlock &Block : Function)
      for (const llvm::Instruction &Instruction : Block)
        if (const auto *Load = llvm::dyn_cast<llvm::LoadInst>(&Instruction)) {
          const RuntimeLoadLocation Location =
              locateRuntimeLoad(*Load, *Runtime, Layout);
          if (overlapsScalarResult(Location) &&
              !AuthorizedResultLoads.contains(Load))
            return protocolFailure(
                &Function,
                "runtime scalar result is read without a checked load edge");
        }
  }
  return llvm::Error::success();
}

llvm::Error verifyRuntimeTranslationIRV1(
    const llvm::Module &Module, const llvm::Triple &ExpectedHostTriple,
    const llvm::DataLayout &ExpectedHostDataLayout, uint64_t StateSize,
    llvm::ArrayRef<TranslationIRMemorySlot> StateSlots) {
  llvm::SmallVector<TranslationIRMemorySlot, 32> Slots;
  Slots.reserve(StateSlots.size() + runtimeABIMemorySlotsV1().size());
  for (const TranslationIRMemorySlot &Slot : StateSlots) {
    if (Slot.Region != TranslationIRMemoryRegion::State)
      return invalid("v1 state policy contains a runtime memory slot");
    Slots.push_back(Slot);
  }
  Slots.append(runtimeABIMemorySlotsV1().begin(),
               runtimeABIMemorySlotsV1().end());

  const std::vector<TranslationRuntimeHelper> Helpers =
      createRuntimeABIHelperPolicyV1(Module.getContext());
  if (llvm::Error Error = verifyTranslationIR(
          Module, ExpectedHostTriple, ExpectedHostDataLayout, StateSize,
          kRuntimeControlBlockSizeV1, Slots, Helpers))
    return Error;
  return verifyRuntimeABIHelperProtocolV1(Module);
}

} // namespace neverd::translate

extern "C" uint32_t nvd_rt_v1_load8_le(void *Runtime, uint64_t Address,
                                       uint32_t RequiredAlignment) noexcept {
  return neverd::translate::load(Runtime, Address, RequiredAlignment,
                                 neverd::translate::GuestScalarWidth::I8);
}

extern "C" uint32_t nvd_rt_v1_load16_le(void *Runtime, uint64_t Address,
                                        uint32_t RequiredAlignment) noexcept {
  return neverd::translate::load(Runtime, Address, RequiredAlignment,
                                 neverd::translate::GuestScalarWidth::I16);
}

extern "C" uint32_t nvd_rt_v1_load32_le(void *Runtime, uint64_t Address,
                                        uint32_t RequiredAlignment) noexcept {
  return neverd::translate::load(Runtime, Address, RequiredAlignment,
                                 neverd::translate::GuestScalarWidth::I32);
}

extern "C" uint32_t nvd_rt_v1_load64_le(void *Runtime, uint64_t Address,
                                        uint32_t RequiredAlignment) noexcept {
  return neverd::translate::load(Runtime, Address, RequiredAlignment,
                                 neverd::translate::GuestScalarWidth::I64);
}

extern "C" uint32_t nvd_rt_v1_store8_le(void *Runtime, uint64_t Address,
                                        uint64_t Value,
                                        uint32_t RequiredAlignment) noexcept {
  return neverd::translate::store(Runtime, Address, Value, RequiredAlignment,
                                  neverd::translate::GuestScalarWidth::I8);
}

extern "C" uint32_t nvd_rt_v1_store16_le(void *Runtime, uint64_t Address,
                                         uint64_t Value,
                                         uint32_t RequiredAlignment) noexcept {
  return neverd::translate::store(Runtime, Address, Value, RequiredAlignment,
                                  neverd::translate::GuestScalarWidth::I16);
}

extern "C" uint32_t nvd_rt_v1_store32_le(void *Runtime, uint64_t Address,
                                         uint64_t Value,
                                         uint32_t RequiredAlignment) noexcept {
  return neverd::translate::store(Runtime, Address, Value, RequiredAlignment,
                                  neverd::translate::GuestScalarWidth::I32);
}

extern "C" uint32_t nvd_rt_v1_store64_le(void *Runtime, uint64_t Address,
                                         uint64_t Value,
                                         uint32_t RequiredAlignment) noexcept {
  return neverd::translate::store(Runtime, Address, Value, RequiredAlignment,
                                  neverd::translate::GuestScalarWidth::I64);
}
