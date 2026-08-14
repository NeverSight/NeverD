//===- RuntimeABI.cpp - Backend-private translation runtime ABI ----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "neverd/translate/RuntimeABI.h"

#include "neverd/translate/TranslationResult.h"

#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Type.h"
#include "llvm/Support/Errc.h"
#include "llvm/Support/ErrorHandling.h"

#include <cassert>
#include <cstdint>
#include <iterator>
#include <limits>
#include <vector>

namespace neverd::translate {
namespace {

llvm::Error invalid(llvm::StringRef Message) {
  return llvm::createStringError(llvm::errc::invalid_argument, "%s",
                                 Message.str().c_str());
}

bool isKnownPolicy(uint32_t Value) {
  switch (Value) {
  case static_cast<uint32_t>(CodeInvalidationPolicy::RejectExecutableWrites):
  case static_cast<uint32_t>(
      CodeInvalidationPolicy::InvalidateOnExecutableWrite):
  case static_cast<uint32_t>(CodeInvalidationPolicy::ValidateBeforeDispatch):
    return true;
  }
  return false;
}

bool isKnownExitKind(RuntimeABIExitKindV1 Kind) {
  switch (Kind) {
  case RuntimeABIExitKindV1::None:
  case RuntimeABIExitKindV1::MemoryFault:
  case RuntimeABIExitKindV1::SelfModification:
  case RuntimeABIExitKindV1::BudgetExhausted:
  case RuntimeABIExitKindV1::Cancelled:
    return true;
  }
  return false;
}

bool isKnownFault(RuntimeMemoryFaultKindV1 Kind) {
  switch (Kind) {
  case RuntimeMemoryFaultKindV1::None:
  case RuntimeMemoryFaultKindV1::InvalidAccessWidth:
  case RuntimeMemoryFaultKindV1::InvalidAlignment:
  case RuntimeMemoryFaultKindV1::Misaligned:
  case RuntimeMemoryFaultKindV1::AddressOverflow:
  case RuntimeMemoryFaultKindV1::Unmapped:
  case RuntimeMemoryFaultKindV1::CrossRegion:
  case RuntimeMemoryFaultKindV1::PermissionDenied:
  case RuntimeMemoryFaultKindV1::ExecutableWriteRejected:
  case RuntimeMemoryFaultKindV1::ExecutableGenerationOverflow:
  case RuntimeMemoryFaultKindV1::ExecutableGenerationMismatch:
  case RuntimeMemoryFaultKindV1::PolicyViolation:
  case RuntimeMemoryFaultKindV1::InvalidRuntimeFrame:
    return true;
  }
  return false;
}

bool isScalarAccessSize(uint64_t Size) {
  return Size == 1 || Size == 2 || Size == 4 || Size == 8;
}

bool isKnownAccess(RuntimeMemoryAccessKindV1 Access) {
  switch (Access) {
  case RuntimeMemoryAccessKindV1::Read:
  case RuntimeMemoryAccessKindV1::Write:
  case RuntimeMemoryAccessKindV1::Execute:
    return true;
  }
  return false;
}

bool isPowerOfTwo(uint32_t Value) {
  return Value != 0 && (Value & (Value - 1)) == 0;
}

constexpr uint64_t AccessMask = 0xff;
constexpr unsigned AlignmentShift = 32;
constexpr uint64_t OrdinaryKnownMask =
    AccessMask |
    (uint64_t{std::numeric_limits<uint32_t>::max()} << AlignmentShift);

RuntimeMemoryFaultDetailEncodingV1
packOrdinaryMemoryFaultDetails(const RuntimeMemoryFaultDetailsV1 &Details) {
  return {static_cast<uint64_t>(Details.Access) |
              (uint64_t{Details.RequiredAlignment} << AlignmentShift),
          0};
}

llvm::Expected<RuntimeMemoryFaultDetailsV1> unpackOrdinaryMemoryFaultDetails(
    const RuntimeMemoryFaultDetailEncodingV1 &Encoding) {
  if ((Encoding.Detail0 & ~OrdinaryKnownMask) != 0 || Encoding.Detail1 != 0)
    return invalid("runtime memory-fault detail contains reserved bits");
  RuntimeMemoryFaultDetailsV1 Details;
  Details.Access =
      static_cast<RuntimeMemoryAccessKindV1>(Encoding.Detail0 & AccessMask);
  Details.RequiredAlignment =
      static_cast<uint32_t>(Encoding.Detail0 >> AlignmentShift);
  if (!isKnownAccess(Details.Access))
    return invalid("runtime memory-fault detail has an unknown access kind");
  return Details;
}

bool isReadOrWrite(RuntimeMemoryAccessKindV1 Access) {
  return Access == RuntimeMemoryAccessKindV1::Read ||
         Access == RuntimeMemoryAccessKindV1::Write;
}

llvm::Error
validateMemoryFaultDetails(RuntimeMemoryFaultKindV1 Fault,
                           const RuntimeMemoryFaultDetailsV1 &Details) {
  if (!isKnownAccess(Details.Access))
    return invalid("runtime memory-fault detail has an unknown access kind");

  switch (Fault) {
  case RuntimeMemoryFaultKindV1::ExecutableGenerationMismatch:
    if (Details.Access != RuntimeMemoryAccessKindV1::Execute ||
        Details.RequiredAlignment != 0)
      return invalid(
          "generation-mismatch fault has inconsistent access fields");
    if (Details.ExpectedGeneration == Details.ObservedGeneration)
      return invalid("generation-mismatch fault has equal generations");
    return llvm::Error::success();

  case RuntimeMemoryFaultKindV1::ExecutableGenerationOverflow:
    if (Details.Access != RuntimeMemoryAccessKindV1::Write)
      return invalid(
          "generation-overflow fault has an inconsistent access kind");
    if (Details.RequiredAlignment != 0 &&
        !isPowerOfTwo(Details.RequiredAlignment))
      return invalid("runtime memory-fault detail has an invalid alignment");
    if (Details.ExpectedGeneration != 0 ||
        Details.ObservedGeneration != std::numeric_limits<uint64_t>::max())
      return invalid("generation-overflow fault has incoherent generations");
    return llvm::Error::success();

  case RuntimeMemoryFaultKindV1::None:
    return invalid("memory-fault detail has no fault kind");

  case RuntimeMemoryFaultKindV1::InvalidAccessWidth:
  case RuntimeMemoryFaultKindV1::InvalidAlignment:
  case RuntimeMemoryFaultKindV1::Misaligned:
  case RuntimeMemoryFaultKindV1::AddressOverflow:
  case RuntimeMemoryFaultKindV1::Unmapped:
  case RuntimeMemoryFaultKindV1::CrossRegion:
  case RuntimeMemoryFaultKindV1::PermissionDenied:
  case RuntimeMemoryFaultKindV1::ExecutableWriteRejected:
  case RuntimeMemoryFaultKindV1::PolicyViolation:
  case RuntimeMemoryFaultKindV1::InvalidRuntimeFrame:
    if (Details.ExpectedGeneration != 0 || Details.ObservedGeneration != 0)
      return invalid("ordinary memory-fault detail carries generation state");
    break;
  }

  switch (Fault) {
  case RuntimeMemoryFaultKindV1::InvalidAccessWidth:
    if (Details.RequiredAlignment != 0)
      return invalid("invalid-access-width fault carries an alignment");
    return llvm::Error::success();

  case RuntimeMemoryFaultKindV1::Unmapped:
  case RuntimeMemoryFaultKindV1::AddressOverflow:
  case RuntimeMemoryFaultKindV1::PermissionDenied:
    if (Details.Access == RuntimeMemoryAccessKindV1::Execute)
      return Details.RequiredAlignment == 0
                 ? llvm::Error::success()
                 : invalid("execute memory fault carries an alignment");
    if (Details.RequiredAlignment != 0 &&
        !isPowerOfTwo(Details.RequiredAlignment))
      return invalid("runtime memory-fault detail has an invalid alignment");
    return llvm::Error::success();

  case RuntimeMemoryFaultKindV1::CrossRegion:
    if (!isReadOrWrite(Details.Access))
      return invalid("cross-region fault has an inconsistent access kind");
    if (Details.RequiredAlignment != 0 &&
        !isPowerOfTwo(Details.RequiredAlignment))
      return invalid("runtime memory-fault detail has an invalid alignment");
    return llvm::Error::success();

  case RuntimeMemoryFaultKindV1::Misaligned:
    if (!isReadOrWrite(Details.Access))
      return invalid("misaligned fault has an inconsistent access kind");
    if (!isPowerOfTwo(Details.RequiredAlignment))
      return invalid("runtime memory-fault detail has an invalid alignment");
    return llvm::Error::success();

  case RuntimeMemoryFaultKindV1::InvalidAlignment:
    if (!isReadOrWrite(Details.Access))
      return invalid("invalid-alignment fault has an inconsistent access kind");
    if (Details.RequiredAlignment == 0 ||
        isPowerOfTwo(Details.RequiredAlignment))
      return invalid(
          "runtime memory-fault alignment contradicts its fault kind");
    return llvm::Error::success();

  case RuntimeMemoryFaultKindV1::ExecutableWriteRejected:
    if (Details.Access != RuntimeMemoryAccessKindV1::Write)
      return invalid(
          "executable-write-rejected fault has an inconsistent access kind");
    if (Details.RequiredAlignment != 0 &&
        !isPowerOfTwo(Details.RequiredAlignment))
      return invalid("runtime memory-fault detail has an invalid alignment");
    return llvm::Error::success();

  case RuntimeMemoryFaultKindV1::PolicyViolation:
    if (Details.Access != RuntimeMemoryAccessKindV1::Execute ||
        Details.RequiredAlignment != 0)
      return invalid("policy-violation fault has inconsistent access fields");
    return llvm::Error::success();

  case RuntimeMemoryFaultKindV1::InvalidRuntimeFrame:
    if (!isReadOrWrite(Details.Access))
      return invalid(
          "invalid-runtime-frame fault has an inconsistent access kind");
    return llvm::Error::success();

  case RuntimeMemoryFaultKindV1::None:
    return invalid("memory-fault detail kind is not supported");
  case RuntimeMemoryFaultKindV1::ExecutableGenerationOverflow:
  case RuntimeMemoryFaultKindV1::ExecutableGenerationMismatch:
    llvm_unreachable("handled generation memory-fault kind");
  }
  return invalid("unknown runtime memory-fault detail kind");
}

bool hasZeroPayload(const RuntimeABIExitV1 &Exit) {
  return Exit.Address == 0 && Exit.Size == 0 && Exit.Detail0 == 0 &&
         Exit.Detail1 == 0;
}

bool hasZeroGeneration(const RuntimeControlBlockV1 &Block) {
  return Block.ExpectedGeneration == 0 && Block.ObservedGeneration == 0;
}

bool usesPolicy(const RuntimeControlBlockV1 &Block,
                CodeInvalidationPolicy Policy) {
  return Block.CodeInvalidation == static_cast<uint32_t>(Policy);
}

bool hasFlag(const RuntimeControlBlockV1 &Block, RuntimeABIFlagV1 Flag) {
  return (Block.Flags & static_cast<uint32_t>(Flag)) != 0;
}

constexpr RuntimeABIValueKind LoadParameters[] = {
    RuntimeABIValueKind::RuntimePointer, RuntimeABIValueKind::I64,
    RuntimeABIValueKind::I32};
constexpr TranslationRuntimeParameterKind LoadVerifierParameters[] = {
    TranslationRuntimeParameterKind::RuntimePointer,
    TranslationRuntimeParameterKind::ScalarInteger,
    TranslationRuntimeParameterKind::ScalarInteger};

constexpr RuntimeABIValueKind StoreParameters[] = {
    RuntimeABIValueKind::RuntimePointer, RuntimeABIValueKind::I64,
    RuntimeABIValueKind::I64, RuntimeABIValueKind::I32};
constexpr TranslationRuntimeParameterKind StoreVerifierParameters[] = {
    TranslationRuntimeParameterKind::RuntimePointer,
    TranslationRuntimeParameterKind::ScalarInteger,
    TranslationRuntimeParameterKind::ScalarInteger,
    TranslationRuntimeParameterKind::ScalarInteger};

const RuntimeABIHelperSignatureV1 HelperSignatures[] = {
    {"nvd_rt_v1_load8_le", RuntimeABIValueKind::I32, LoadParameters,
     LoadVerifierParameters},
    {"nvd_rt_v1_load16_le", RuntimeABIValueKind::I32, LoadParameters,
     LoadVerifierParameters},
    {"nvd_rt_v1_load32_le", RuntimeABIValueKind::I32, LoadParameters,
     LoadVerifierParameters},
    {"nvd_rt_v1_load64_le", RuntimeABIValueKind::I32, LoadParameters,
     LoadVerifierParameters},
    {"nvd_rt_v1_store8_le", RuntimeABIValueKind::I32, StoreParameters,
     StoreVerifierParameters},
    {"nvd_rt_v1_store16_le", RuntimeABIValueKind::I32, StoreParameters,
     StoreVerifierParameters},
    {"nvd_rt_v1_store32_le", RuntimeABIValueKind::I32, StoreParameters,
     StoreVerifierParameters},
    {"nvd_rt_v1_store64_le", RuntimeABIValueKind::I32, StoreParameters,
     StoreVerifierParameters},
};

constexpr TranslationIRMemorySlot MemorySlots[] = {
    {TranslationIRMemoryRegion::Runtime,
     offsetof(RuntimeControlBlockV1, ScalarResult), sizeof(uint64_t),
     TranslationIRMemoryAccess::Read, alignof(uint64_t)},
};

llvm::Type *typeFor(RuntimeABIValueKind Kind, llvm::LLVMContext &Context) {
  switch (Kind) {
  case RuntimeABIValueKind::Void:
    return llvm::Type::getVoidTy(Context);
  case RuntimeABIValueKind::I32:
    return llvm::Type::getInt32Ty(Context);
  case RuntimeABIValueKind::I64:
    return llvm::Type::getInt64Ty(Context);
  case RuntimeABIValueKind::StatePointer:
  case RuntimeABIValueKind::RuntimePointer:
    return llvm::PointerType::getUnqual(Context);
  }
  llvm_unreachable("unknown runtime ABI value kind");
}

} // namespace

RuntimeControlBlockV1
makeRuntimeControlBlockV1(CodeInvalidationPolicy CodeInvalidation,
                          uint64_t InstructionBudget, uint64_t BlockBudget) {
  RuntimeControlBlockV1 Block;
  Block.CodeInvalidation = static_cast<uint32_t>(CodeInvalidation);
  Block.InstructionBudget = InstructionBudget;
  Block.BlockBudget = BlockBudget;
  return Block;
}

llvm::Expected<RuntimeMemoryFaultDetailEncodingV1>
packRuntimeMemoryFaultDetailsV1(RuntimeMemoryFaultKindV1 Fault,
                                const RuntimeMemoryFaultDetailsV1 &Details) {
  if (llvm::Error Error = validateMemoryFaultDetails(Fault, Details))
    return std::move(Error);
  if (Fault == RuntimeMemoryFaultKindV1::ExecutableGenerationMismatch)
    return RuntimeMemoryFaultDetailEncodingV1{Details.ExpectedGeneration,
                                              Details.ObservedGeneration};
  if (Fault == RuntimeMemoryFaultKindV1::ExecutableGenerationOverflow) {
    RuntimeMemoryFaultDetailEncodingV1 Encoding =
        packOrdinaryMemoryFaultDetails(Details);
    Encoding.Detail1 = Details.ObservedGeneration;
    return Encoding;
  }
  return packOrdinaryMemoryFaultDetails(Details);
}

llvm::Expected<RuntimeMemoryFaultDetailsV1> unpackRuntimeMemoryFaultDetailsV1(
    RuntimeMemoryFaultKindV1 Fault,
    const RuntimeMemoryFaultDetailEncodingV1 &Encoding) {
  if (Fault == RuntimeMemoryFaultKindV1::ExecutableGenerationMismatch) {
    RuntimeMemoryFaultDetailsV1 Details;
    Details.Access = RuntimeMemoryAccessKindV1::Execute;
    Details.ExpectedGeneration = Encoding.Detail0;
    Details.ObservedGeneration = Encoding.Detail1;
    if (llvm::Error Error = validateMemoryFaultDetails(Fault, Details))
      return std::move(Error);
    return Details;
  }

  if (Fault == RuntimeMemoryFaultKindV1::ExecutableGenerationOverflow) {
    llvm::Expected<RuntimeMemoryFaultDetailsV1> Details =
        unpackOrdinaryMemoryFaultDetails({Encoding.Detail0, 0});
    if (!Details)
      return Details.takeError();
    Details->ObservedGeneration = Encoding.Detail1;
    if (llvm::Error Error = validateMemoryFaultDetails(Fault, *Details))
      return std::move(Error);
    return *Details;
  }

  llvm::Expected<RuntimeMemoryFaultDetailsV1> Details =
      unpackOrdinaryMemoryFaultDetails(Encoding);
  if (!Details)
    return Details.takeError();
  if (llvm::Error Error = validateMemoryFaultDetails(Fault, *Details))
    return std::move(Error);
  return *Details;
}

llvm::Error validateRuntimeControlBlockV1(const RuntimeControlBlockV1 &Block) {
  if (Block.Magic != kRuntimeABIMagicV1)
    return invalid("invalid runtime ABI magic");
  if (Block.Version != kRuntimeABIVersionV1)
    return invalid("unsupported runtime ABI version");
  if (Block.Size != kRuntimeControlBlockSizeV1)
    return invalid("invalid runtime ABI size");
  if (!isKnownPolicy(Block.CodeInvalidation))
    return invalid("invalid runtime ABI code-invalidation policy");
  constexpr uint32_t KnownFlags =
      static_cast<uint32_t>(RuntimeABIFlagV1::GenerationValidated);
  if ((Block.Flags & ~KnownFlags) != 0)
    return invalid("runtime ABI contains an unknown flag");
  if (Block.Reserved1 != 0)
    return invalid("runtime ABI reserved field is non-zero");
  if (Block.CancellationRequested > 1)
    return invalid("invalid runtime ABI cancellation value");
  if (!isKnownExitKind(Block.Exit.Kind))
    return invalid("unknown runtime ABI exit kind");
  if (!isKnownFault(Block.Exit.Fault))
    return invalid("unknown runtime ABI memory fault");

  const bool IsMemoryFault =
      Block.Exit.Kind == RuntimeABIExitKindV1::MemoryFault;
  if (IsMemoryFault && Block.Exit.Fault == RuntimeMemoryFaultKindV1::None)
    return invalid("runtime memory-fault exit has no fault kind");
  if (!IsMemoryFault && Block.Exit.Fault != RuntimeMemoryFaultKindV1::None)
    return invalid("non-memory runtime exit carries a memory fault");
  if (Block.Exit.Kind != RuntimeABIExitKindV1::None && Block.Flags != 0)
    return invalid("non-success runtime exit carries runtime ABI flags");

  const bool IsCancelled = Block.Exit.Kind == RuntimeABIExitKindV1::Cancelled;
  if (Block.CancellationRequested != (IsCancelled ? 1u : 0u))
    return invalid(IsCancelled
                       ? "cancelled runtime exit has no cancellation request"
                       : "runtime cancellation request has no cancelled exit");

  const bool InstructionBudgetExhausted =
      Block.InstructionBudget != 0 &&
      Block.InstructionCount >= Block.InstructionBudget;
  const bool BlockBudgetExhausted =
      Block.BlockBudget != 0 && Block.BlockCount >= Block.BlockBudget;
  if (Block.Exit.Kind != RuntimeABIExitKindV1::BudgetExhausted &&
      !IsCancelled && (InstructionBudgetExhausted || BlockBudgetExhausted))
    return invalid("runtime ABI exhausted budget has no budget exit");

  switch (Block.Exit.Kind) {
  case RuntimeABIExitKindV1::None:
    if (!hasZeroPayload(Block.Exit))
      return invalid("empty runtime exit carries a payload");
    if (usesPolicy(Block, CodeInvalidationPolicy::ValidateBeforeDispatch)) {
      if (!hasFlag(Block, RuntimeABIFlagV1::GenerationValidated))
        return invalid("runtime ABI dispatch generation validation is missing");
      if (Block.ExpectedGeneration != Block.ObservedGeneration)
        return invalid("runtime ABI dispatch generation was not validated");
      if (Block.ScalarResult != Block.ObservedGeneration)
        return invalid("runtime ABI validated generation has a stale scalar");
    } else {
      if (Block.Flags != 0)
        return invalid("runtime ABI generation flag is invalid for the policy");
      if (!hasZeroGeneration(Block))
        return invalid(
            "runtime ABI generation mirror is invalid for the policy");
    }
    return llvm::Error::success();

  case RuntimeABIExitKindV1::Cancelled:
    if (!hasZeroPayload(Block.Exit))
      return invalid("cancelled runtime exit carries a payload");
    if (!hasZeroGeneration(Block))
      return invalid("cancelled runtime exit carries generation state");
    if (Block.ScalarResult != 0)
      return invalid("cancelled runtime exit carries a stale scalar");
    return llvm::Error::success();

  case RuntimeABIExitKindV1::BudgetExhausted: {
    if (Block.Exit.Address != 0)
      return invalid("runtime budget exit carries an address");
    if (!hasZeroGeneration(Block))
      return invalid("runtime budget exit carries generation state");
    if (Block.ScalarResult != 0)
      return invalid("runtime budget exit carries a stale scalar");

    switch (Block.Exit.Size) {
    case static_cast<uint64_t>(TranslationBudgetKind::GuestInstructions):
      if (!InstructionBudgetExhausted || Block.Exit.Detail0 == 0 ||
          Block.Exit.Detail0 != Block.InstructionBudget ||
          Block.Exit.Detail1 != Block.InstructionCount)
        return invalid("runtime instruction-budget exit is incoherent");
      return llvm::Error::success();
    case static_cast<uint64_t>(TranslationBudgetKind::Blocks):
      if (InstructionBudgetExhausted || !BlockBudgetExhausted ||
          Block.Exit.Detail0 == 0 || Block.Exit.Detail0 != Block.BlockBudget ||
          Block.Exit.Detail1 != Block.BlockCount)
        return invalid("runtime block-budget exit is incoherent");
      return llvm::Error::success();
    case static_cast<uint64_t>(TranslationBudgetKind::GeneratedCodeBytes):
      break;
    }
    return invalid("runtime budget exit has an invalid budget kind");
  }

  case RuntimeABIExitKindV1::SelfModification:
    if (!usesPolicy(Block, CodeInvalidationPolicy::InvalidateOnExecutableWrite))
      return invalid("runtime self-modification exit violates its policy");
    if (!isScalarAccessSize(Block.Exit.Size) ||
        Block.Exit.Size - 1 >
            std::numeric_limits<uint64_t>::max() - Block.Exit.Address ||
        Block.Exit.Detail0 == std::numeric_limits<uint64_t>::max() ||
        Block.Exit.Detail1 != Block.Exit.Detail0 + 1)
      return invalid("invalid runtime self-modification exit");
    if (Block.ExpectedGeneration != 0 ||
        Block.ObservedGeneration != Block.Exit.Detail1)
      return invalid("runtime self-modification generation mirror is invalid");
    if (Block.ScalarResult != 0)
      return invalid("runtime self-modification exit carries a stale scalar");
    return llvm::Error::success();

  case RuntimeABIExitKindV1::MemoryFault: {
    if (Block.ScalarResult != 0)
      return invalid("runtime memory-fault exit carries a stale scalar");
    llvm::Expected<RuntimeMemoryFaultDetailsV1> Details =
        unpackRuntimeMemoryFaultDetailsV1(
            Block.Exit.Fault, {Block.Exit.Detail0, Block.Exit.Detail1});
    if (!Details)
      return Details.takeError();
    if (Block.ExpectedGeneration != Details->ExpectedGeneration ||
        Block.ObservedGeneration != Details->ObservedGeneration)
      return invalid("runtime memory-fault generation mirror is incoherent");

    if (Block.Exit.Fault == RuntimeMemoryFaultKindV1::InvalidAccessWidth) {
      if (Block.Exit.Size != 0)
        return invalid("invalid-access-width memory-fault exit is incoherent");
    } else if (Details->Access == RuntimeMemoryAccessKindV1::Execute) {
      if (Block.Exit.Size == 0)
        return invalid("runtime execute memory fault has an empty access");
    } else if (!isScalarAccessSize(Block.Exit.Size)) {
      return invalid("runtime memory-fault exit has an invalid access size");
    }

    switch (Block.Exit.Fault) {
    case RuntimeMemoryFaultKindV1::ExecutableGenerationMismatch:
      if (!usesPolicy(Block, CodeInvalidationPolicy::ValidateBeforeDispatch) ||
          Block.Exit.Size != 1)
        return invalid("runtime executable generation mismatch is incoherent");
      return llvm::Error::success();

    case RuntimeMemoryFaultKindV1::ExecutableGenerationOverflow:
      if (usesPolicy(Block, CodeInvalidationPolicy::RejectExecutableWrites))
        return invalid("runtime executable generation overflow is incoherent");
      return llvm::Error::success();

    case RuntimeMemoryFaultKindV1::ExecutableWriteRejected:
      if (!usesPolicy(Block, CodeInvalidationPolicy::RejectExecutableWrites))
        return invalid(
            "runtime executable-write rejection violates its policy");
      break;

    case RuntimeMemoryFaultKindV1::PolicyViolation:
      if (usesPolicy(Block, CodeInvalidationPolicy::ValidateBeforeDispatch) ||
          Block.Exit.Size != 1)
        return invalid("runtime policy-violation fault contradicts its policy");
      break;

    case RuntimeMemoryFaultKindV1::Misaligned:
      if (Block.Exit.Address % Details->RequiredAlignment == 0)
        return invalid("runtime misaligned fault has an aligned address");
      break;

    case RuntimeMemoryFaultKindV1::None:
      llvm_unreachable("validated runtime memory-fault kind");

    case RuntimeMemoryFaultKindV1::InvalidAccessWidth:
    case RuntimeMemoryFaultKindV1::InvalidAlignment:
    case RuntimeMemoryFaultKindV1::AddressOverflow:
    case RuntimeMemoryFaultKindV1::Unmapped:
    case RuntimeMemoryFaultKindV1::CrossRegion:
    case RuntimeMemoryFaultKindV1::PermissionDenied:
    case RuntimeMemoryFaultKindV1::InvalidRuntimeFrame:
      break;
    }
    return llvm::Error::success();
  }
  }
  llvm_unreachable("validated runtime ABI exit kind");
}

llvm::ArrayRef<RuntimeABIHelperSignatureV1> runtimeABIHelperSignaturesV1() {
  return HelperSignatures;
}

llvm::ArrayRef<TranslationIRMemorySlot> runtimeABIMemorySlotsV1() {
  return MemorySlots;
}

const RuntimeABIHelperSignatureV1 *
findRuntimeABIHelperSignatureV1(llvm::StringRef Name) {
  for (const RuntimeABIHelperSignatureV1 &Signature : HelperSignatures)
    if (Signature.Name == Name)
      return &Signature;
  return nullptr;
}

std::vector<TranslationRuntimeHelper>
createRuntimeABIHelperPolicyV1(llvm::LLVMContext &Context) {
  std::vector<TranslationRuntimeHelper> Helpers;
  Helpers.reserve(std::size(HelperSignatures));
  for (const RuntimeABIHelperSignatureV1 &Signature : HelperSignatures) {
    assert(Signature.Parameters.size() == Signature.VerifierParameters.size() &&
           "runtime helper type/provenance arity mismatch");
    std::vector<llvm::Type *> Parameters;
    Parameters.reserve(Signature.Parameters.size());
    for (RuntimeABIValueKind Kind : Signature.Parameters) {
      assert(Kind != RuntimeABIValueKind::Void &&
             "void is not a helper parameter type");
      Parameters.push_back(typeFor(Kind, Context));
    }
    Helpers.push_back(
        {Signature.Name,
         llvm::FunctionType::get(typeFor(Signature.Result, Context), Parameters,
                                 false),
         Signature.VerifierParameters});
  }
  return Helpers;
}

} // namespace neverd::translate
