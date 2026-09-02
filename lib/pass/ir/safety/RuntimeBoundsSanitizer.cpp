//===- RuntimeBoundsSanitizer.cpp - Exact write bounds guards -----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "neverd/pass/ir/safety/RuntimeBoundsSanitizer.h"

#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/DataLayout.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/InstIterator.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Intrinsics.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Verifier.h"
#include "llvm/Support/Error.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/TargetParser/Triple.h"
#include "llvm/Transforms/Utils/Cloning.h"

#include <algorithm>
#include <limits>
#include <utility>

namespace neverd::safety {

namespace {

using safety_callsite_md::SafetyCallsiteRecord;

struct GuardTarget {
  const RuntimeSanitizerGuard *Guard = nullptr;
  llvm::CallBase *Call = nullptr;
};

RuntimeBoundsSanitizerResult
failure(RuntimeBoundsSanitizerError Error, llvm::StringRef Detail,
        std::optional<safety_callsite_md::SafetyCallsiteOccurrence> Occurrence =
            std::nullopt) {
  RuntimeBoundsSanitizerResult Result;
  Result.Error = Error;
  Result.FailureOccurrence = Occurrence;
  Result.Detail = Detail.str();
  return Result;
}

bool validGuard(const RuntimeSanitizerGuard &Guard) {
  const auto &Occurrence = Guard.Occurrence;
  constexpr uint32_t MaxSignedIndex =
      static_cast<uint32_t>(std::numeric_limits<int32_t>::max());
  if (Guard.Version != kRuntimeSanitizerPlanVersion ||
      Occurrence.FuncEntry == std::numeric_limits<uint64_t>::max() ||
      Occurrence.CallVA == std::numeric_limits<uint64_t>::max() ||
      Occurrence.BlockId > MaxSignedIndex ||
      Occurrence.OpIdx > MaxSignedIndex ||
      Occurrence.OriginSeq > MaxSignedIndex || Occurrence.CallSiteId == 0 ||
      !counted_write::isValid({Guard.Kind, Guard.DestinationOperandIndex,
                               Guard.LengthOperandIndex, Guard.ElementBytes}))
    return false;
  return true;
}

SafetyCallsiteRecord expectedRecord(const RuntimeSanitizerGuard &Guard) {
  SafetyCallsiteRecord Record;
  Record.Occurrence = Guard.Occurrence;
  Record.Kind = Guard.Kind;
  Record.DestinationOperandIndex = Guard.DestinationOperandIndex;
  Record.LengthOperandIndex = Guard.LengthOperandIndex;
  Record.ElementBytes = Guard.ElementBytes;
  return Record;
}

std::optional<unsigned> targetPointerWidth(const llvm::Module &Module) {
  const llvm::DataLayout &Layout = Module.getDataLayout();
  if (!Layout.isDefault())
    return Layout.getPointerSizeInBits(/*AddressSpace=*/0);

  const unsigned Width = Module.getTargetTriple().getArchPointerBitWidth();
  if (Width == 32 || Width == 64)
    return Width;
  return std::nullopt;
}

bool validDestinationType(const llvm::Module &Module, llvm::Type *Type) {
  if (Type->isPointerTy())
    return true;
  const auto *Integer = llvm::dyn_cast<llvm::IntegerType>(Type);
  if (!Integer)
    return false;
  const std::optional<unsigned> PointerWidth = targetPointerWidth(Module);
  return PointerWidth && Integer->getBitWidth() == *PointerWidth;
}

RuntimeBoundsSanitizerResult
resolveTargets(llvm::Module &Module,
               llvm::ArrayRef<RuntimeSanitizerGuard> Guards,
               std::vector<GuardTarget> &Targets) {
  for (size_t Left = 0; Left < Guards.size(); ++Left) {
    if (Guards[Left].Version != kRuntimeSanitizerPlanVersion)
      return failure(RuntimeBoundsSanitizerError::UnsupportedPlanVersion,
                     "guard uses an unsupported plan version",
                     Guards[Left].Occurrence);
    if (!validGuard(Guards[Left]))
      return failure(RuntimeBoundsSanitizerError::InvalidGuard,
                     "guard has an invalid occurrence or write layout",
                     Guards[Left].Occurrence);
    for (size_t Right = 0; Right < Left; ++Right)
      if (Guards[Left].Occurrence == Guards[Right].Occurrence)
        return failure(RuntimeBoundsSanitizerError::DuplicateGuard,
                       "guard plan repeats one occurrence",
                       Guards[Left].Occurrence);
  }

  std::vector<std::pair<SafetyCallsiteRecord, llvm::CallBase *>> Records;
  for (llvm::Function &Function : Module) {
    for (llvm::Instruction &Instruction : llvm::instructions(Function)) {
      auto *Call = llvm::dyn_cast<llvm::CallBase>(&Instruction);
      if (!Call)
        continue;
      llvm::Expected<std::optional<SafetyCallsiteRecord>> Parsed =
          safety_callsite_md::parse(*Call);
      if (!Parsed)
        return failure(RuntimeBoundsSanitizerError::MalformedMetadata,
                       llvm::toString(Parsed.takeError()));
      if (*Parsed)
        Records.emplace_back(**Parsed, Call);
    }
  }

  Targets.reserve(Guards.size());
  for (const RuntimeSanitizerGuard &Guard : Guards) {
    llvm::CallBase *Target = nullptr;
    size_t Matches = 0;
    const SafetyCallsiteRecord Expected = expectedRecord(Guard);
    const SafetyCallsiteRecord *MatchedRecord = nullptr;
    for (const auto &[Record, Call] : Records) {
      if (Record.Occurrence != Guard.Occurrence)
        continue;
      ++Matches;
      Target = Call;
      MatchedRecord = &Record;
    }
    if (Matches == 0)
      return failure(RuntimeBoundsSanitizerError::MissingTarget,
                     "guard target metadata is missing", Guard.Occurrence);
    if (Matches != 1)
      return failure(RuntimeBoundsSanitizerError::DuplicateTarget,
                     "guard target metadata is duplicated", Guard.Occurrence);
    if (*MatchedRecord != Expected)
      return failure(RuntimeBoundsSanitizerError::MetadataMismatch,
                     "guard does not match the emitted callsite record",
                     Guard.Occurrence);

    if (Guard.DestinationOperandIndex >= Target->arg_size() ||
        Guard.LengthOperandIndex >= Target->arg_size())
      return failure(RuntimeBoundsSanitizerError::OperandIndexOutOfRange,
                     "guard operand index is outside the emitted call",
                     Guard.Occurrence);
    llvm::Value *Destination =
        Target->getArgOperand(Guard.DestinationOperandIndex);
    llvm::Value *Length = Target->getArgOperand(Guard.LengthOperandIndex);
    if (!validDestinationType(Module, Destination->getType()) ||
        !Length->getType()->isIntegerTy())
      return failure(
          RuntimeBoundsSanitizerError::OperandTypeMismatch,
          "guard destination must be a pointer or target-width integer and "
          "length an integer",
          Guard.Occurrence);
    Targets.push_back({&Guard, Target});
  }

  for (const auto &[Record, Call] : Records) {
    (void)Call;
    const bool Planned = std::any_of(
        Guards.begin(), Guards.end(), [&](const RuntimeSanitizerGuard &Guard) {
          return Guard.Occurrence == Record.Occurrence;
        });
    if (!Planned)
      return failure(RuntimeBoundsSanitizerError::UnexpectedTarget,
                     "emitted callsite metadata is absent from the guard plan",
                     Record.Occurrence);
  }
  RuntimeBoundsSanitizerResult Success;
  Success.Complete = true;
  return Success;
}

void installGuard(llvm::Module &Module, const GuardTarget &Target) {
  const RuntimeSanitizerGuard &Guard = *Target.Guard;
  llvm::CallBase *Call = Target.Call;
  llvm::BasicBlock *Before = Call->getParent();
  llvm::Function *Function = Before->getParent();
  llvm::LLVMContext &Context = Module.getContext();

  llvm::BasicBlock *Safe = Before->splitBasicBlock(Call, "neverd.bounds.safe");
  Before->getTerminator()->eraseFromParent();
  llvm::BasicBlock *Trap =
      llvm::BasicBlock::Create(Context, "neverd.bounds.trap", Function, Safe);

  llvm::IRBuilder<> Builder(Before);
  llvm::Value *Length = Call->getArgOperand(Guard.LengthOperandIndex);
  auto *LengthType = llvm::cast<llvm::IntegerType>(Length->getType());
  const unsigned CompareWidth = std::max(64u, LengthType->getBitWidth());
  llvm::IntegerType *CompareType =
      llvm::IntegerType::get(Context, CompareWidth);
  llvm::Value *WideLength = Length;
  if (LengthType != CompareType)
    WideLength =
        Builder.CreateZExt(Length, CompareType, "neverd.bounds.length");

  llvm::Value *ByteLength = WideLength;
  llvm::Value *NoMultiplyOverflow = Builder.getTrue();
  if (Guard.ElementBytes != 1) {
    llvm::Function *Multiply = llvm::Intrinsic::getOrInsertDeclaration(
        &Module, llvm::Intrinsic::umul_with_overflow, {CompareType});
    llvm::Value *Product = Builder.CreateCall(
        Multiply,
        {WideLength, llvm::ConstantInt::get(CompareType, Guard.ElementBytes)},
        "neverd.bounds.product");
    ByteLength = Builder.CreateExtractValue(Product, 0, "neverd.bounds.bytes");
    llvm::Value *Overflow =
        Builder.CreateExtractValue(Product, 1, "neverd.bounds.overflow");
    NoMultiplyOverflow =
        Builder.CreateNot(Overflow, "neverd.bounds.no-overflow");
  }
  llvm::Value *WithinCapacity = Builder.CreateICmpULE(
      ByteLength, llvm::ConstantInt::get(CompareType, Guard.RemainingCapacity),
      "neverd.bounds.within-capacity");
  llvm::Value *SafeWrite =
      Guard.ElementBytes == 1
          ? WithinCapacity
          : Builder.CreateAnd(NoMultiplyOverflow, WithinCapacity,
                              "neverd.bounds.safe-write");
  Builder.CreateCondBr(SafeWrite, Safe, Trap);

  llvm::IRBuilder<> TrapBuilder(Trap);
  llvm::Function *TrapIntrinsic =
      llvm::Intrinsic::getOrInsertDeclaration(&Module, llvm::Intrinsic::trap);
  TrapBuilder.CreateCall(TrapIntrinsic);
  TrapBuilder.CreateUnreachable();
}

} // namespace

const char *toString(RuntimeBoundsSanitizerError Error) {
  switch (Error) {
  case RuntimeBoundsSanitizerError::None:
    return "none";
  case RuntimeBoundsSanitizerError::NullModule:
    return "null-module";
  case RuntimeBoundsSanitizerError::UnsupportedPlanVersion:
    return "unsupported-plan-version";
  case RuntimeBoundsSanitizerError::InvalidGuard:
    return "invalid-guard";
  case RuntimeBoundsSanitizerError::DuplicateGuard:
    return "duplicate-guard";
  case RuntimeBoundsSanitizerError::MalformedMetadata:
    return "malformed-metadata";
  case RuntimeBoundsSanitizerError::MissingTarget:
    return "missing-target";
  case RuntimeBoundsSanitizerError::DuplicateTarget:
    return "duplicate-target";
  case RuntimeBoundsSanitizerError::MetadataMismatch:
    return "metadata-mismatch";
  case RuntimeBoundsSanitizerError::OperandIndexOutOfRange:
    return "operand-index-out-of-range";
  case RuntimeBoundsSanitizerError::OperandTypeMismatch:
    return "operand-type-mismatch";
  case RuntimeBoundsSanitizerError::VerificationFailed:
    return "verification-failed";
  case RuntimeBoundsSanitizerError::UnexpectedTarget:
    return "unexpected-target";
  }
  return "unknown";
}

RuntimeBoundsSanitizerResult
applyRuntimeBoundsSanitizer(std::unique_ptr<llvm::Module> &Module,
                            llvm::ArrayRef<RuntimeSanitizerGuard> Guards) {
  if (!Module)
    return failure(RuntimeBoundsSanitizerError::NullModule,
                   "runtime bounds sanitizer requires an owning module");
  std::unique_ptr<llvm::Module> Candidate = llvm::CloneModule(*Module);
  std::vector<GuardTarget> Targets;
  RuntimeBoundsSanitizerResult Resolution =
      resolveTargets(*Candidate, Guards, Targets);
  if (!Resolution.Complete)
    return Resolution;

  for (const GuardTarget &Target : Targets)
    installGuard(*Candidate, Target);

  std::string Verification;
  llvm::raw_string_ostream VerificationOS(Verification);
  if (llvm::verifyModule(*Candidate, &VerificationOS)) {
    VerificationOS.flush();
    return failure(RuntimeBoundsSanitizerError::VerificationFailed,
                   Verification);
  }

  RuntimeBoundsSanitizerResult Result;
  Result.Complete = true;
  for (const RuntimeSanitizerGuard &Guard : Guards)
    Result.GuardedOriginalEntries.push_back(Guard.Occurrence.FuncEntry);
  std::sort(Result.GuardedOriginalEntries.begin(),
            Result.GuardedOriginalEntries.end());
  Result.GuardedOriginalEntries.erase(
      std::unique(Result.GuardedOriginalEntries.begin(),
                  Result.GuardedOriginalEntries.end()),
      Result.GuardedOriginalEntries.end());
  Module = std::move(Candidate);
  return Result;
}

} // namespace neverd::safety
