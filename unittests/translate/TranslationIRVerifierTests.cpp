//===- TranslationIRVerifierTests.cpp - Translation IR boundary tests ----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "neverd/translate/TranslationIRVerifier.h"

#include "llvm/ADT/Twine.h"
#include "llvm/AsmParser/Parser.h"
#include "llvm/IR/DataLayout.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/Support/Error.h"
#include "llvm/Support/SourceMgr.h"
#include "llvm/TargetParser/Triple.h"

#include <memory>
#include <string>

namespace {

using neverd::translate::TranslationIRMemoryAccess;
using neverd::translate::TranslationIRMemoryRegion;
using neverd::translate::TranslationIRMemorySlot;
using neverd::translate::TranslationIRVerificationError;
using neverd::translate::TranslationIRViolation;
using neverd::translate::TranslationRuntimeHelper;
using neverd::translate::TranslationRuntimeParameterKind;

constexpr llvm::StringLiteral HostTriple("aarch64-unknown-linux-gnu");
constexpr llvm::StringLiteral HostLayout("e-p:64:64-i64:64-n32:64-S128");

std::unique_ptr<llvm::Module> parseModule(llvm::LLVMContext &Context,
                                          llvm::StringRef Body) {
  std::string IR = (llvm::Twine("target datalayout = \"") + HostLayout +
                    "\"\ntarget triple = \"" + HostTriple + "\"\n" + Body)
                       .str();
  llvm::SMDiagnostic Diagnostic;
  std::unique_ptr<llvm::Module> Module =
      llvm::parseAssemblyString(IR, Diagnostic, Context);
  EXPECT_NE(Module, nullptr);
  if (Module)
    for (llvm::Function &Function : *Module)
      if (!Function.isDeclaration() && !Function.hasLocalLinkage())
        Function.setVisibility(llvm::GlobalValue::HiddenVisibility);
  return Module;
}

llvm::Error verify(const llvm::Module &Module) {
  llvm::LLVMContext &Context = Module.getContext();
  llvm::Type *Pointer = llvm::PointerType::getUnqual(Context);
  llvm::Type *I32 = llvm::Type::getInt32Ty(Context);
  llvm::Type *I64 = llvm::Type::getInt64Ty(Context);
  const TranslationRuntimeParameterKind DispatchParameters[] = {
      TranslationRuntimeParameterKind::RuntimePointer,
      TranslationRuntimeParameterKind::ScalarInteger};
  const TranslationRuntimeParameterKind ExplicitParameters[] = {
      TranslationRuntimeParameterKind::RuntimePointer};
  const TranslationRuntimeHelper Helpers[] = {
      {"nvd_rt_dispatch", llvm::FunctionType::get(I32, {Pointer, I64}, false),
       DispatchParameters},
      {"explicit_helper",
       llvm::FunctionType::get(llvm::Type::getVoidTy(Context), {Pointer},
                               false),
       ExplicitParameters}};
  const TranslationIRMemorySlot Slots[] = {
      {TranslationIRMemoryRegion::State, 0, 64,
       TranslationIRMemoryAccess::Read | TranslationIRMemoryAccess::Write, 1},
      {TranslationIRMemoryRegion::Runtime, 0, 64,
       TranslationIRMemoryAccess::Read | TranslationIRMemoryAccess::Write, 1}};
  return neverd::translate::verifyTranslationIR(
      Module, llvm::Triple(HostTriple), llvm::DataLayout(HostLayout), 64, 64,
      Slots, Helpers);
}

void expectViolation(llvm::Error Error, TranslationIRViolation Reason) {
  ASSERT_TRUE(static_cast<bool>(Error));
  bool Seen = false;
  llvm::handleAllErrors(std::move(Error),
                        [&](const TranslationIRVerificationError &Failure) {
                          Seen = true;
                          EXPECT_EQ(Failure.reason(), Reason);
                        });
  EXPECT_TRUE(Seen);
}

void expectSuccess(llvm::Error Error) {
  if (Error)
    ADD_FAILURE() << llvm::toString(std::move(Error));
}

TEST(TranslationIRVerifier, AcceptsCanonicalBlockAndAllowlistedHelpers) {
  llvm::LLVMContext Context;
  std::unique_ptr<llvm::Module> Module = parseModule(Context, R"(
declare i32 @nvd_rt_dispatch(ptr, i64) nounwind
declare void @explicit_helper(ptr) nounwind

define i32 @translated_block(ptr %state, ptr %runtime) nounwind {
entry:
  %slot = getelementptr i64, ptr %state, i64 1
  %old = load i64, ptr %slot, align 1
  %next = xor i64 %old, 1
  store i64 %next, ptr %slot, align 1
  call void @explicit_helper(ptr %runtime) nounwind
  %stop = call i32 @nvd_rt_dispatch(ptr %runtime, i64 %next) nounwind
  ret i32 %stop
}
)");
  ASSERT_NE(Module, nullptr);
  expectSuccess(verify(*Module));
}

TEST(TranslationIRVerifier, AcceptsEmptyMemoryAndHelperPolicies) {
  llvm::LLVMContext Context;
  std::unique_ptr<llvm::Module> Module = parseModule(Context, R"(
define i32 @translated_block(ptr %state, ptr %runtime) nounwind {
  ret i32 0
}
)");
  ASSERT_NE(Module, nullptr);
  EXPECT_FALSE(static_cast<bool>(neverd::translate::verifyTranslationIR(
      *Module, llvm::Triple(HostTriple), llvm::DataLayout(HostLayout), 64,
      64)));
}

TEST(TranslationIRVerifier, RejectsHostTripleAndDataLayoutMismatch) {
  llvm::LLVMContext Context;
  std::unique_ptr<llvm::Module> Module = parseModule(Context, R"(
define i32 @translated_block(ptr %state, ptr %runtime) nounwind {
  ret i32 0
}
)");
  ASSERT_NE(Module, nullptr);

  expectViolation(neverd::translate::verifyTranslationIR(
                      *Module, llvm::Triple("x86_64-unknown-linux-gnu"),
                      llvm::DataLayout(HostLayout), 64, 64),
                  TranslationIRViolation::HostTripleMismatch);
  expectViolation(neverd::translate::verifyTranslationIR(
                      *Module, llvm::Triple(HostTriple),
                      llvm::DataLayout("e-p:32:32-i64:64-n32-S128"), 64, 64),
                  TranslationIRViolation::HostDataLayoutMismatch);
}

TEST(TranslationIRVerifier, RejectsNonCanonicalBlockABI) {
  llvm::LLVMContext Context;
  std::unique_ptr<llvm::Module> Module = parseModule(Context, R"(
define void @translated_block(ptr %state) nounwind {
  ret void
}
)");
  ASSERT_NE(Module, nullptr);
  expectViolation(verify(*Module), TranslationIRViolation::NonStandardBlockABI);
}

TEST(TranslationIRVerifier, RejectsInlineAssembly) {
  llvm::LLVMContext Context;
  std::unique_ptr<llvm::Module> Module = parseModule(Context, R"(
define i32 @translated_block(ptr %state, ptr %runtime) nounwind {
  call void asm sideeffect "", ""() nounwind
  ret i32 0
}
)");
  ASSERT_NE(Module, nullptr);
  expectViolation(verify(*Module), TranslationIRViolation::InlineAssembly);
}

TEST(TranslationIRVerifier, RejectsTargetSpecificIntrinsic) {
  llvm::LLVMContext Context;
  std::unique_ptr<llvm::Module> Module = parseModule(Context, R"(
declare void @llvm.aarch64.hint(i32) nounwind

define i32 @translated_block(ptr %state, ptr %runtime) nounwind {
  call void @llvm.aarch64.hint(i32 0) nounwind
  ret i32 0
}
)");
  ASSERT_NE(Module, nullptr);
  expectViolation(verify(*Module),
                  TranslationIRViolation::TargetSpecificIntrinsic);
}

TEST(TranslationIRVerifier, RejectsGuestIntegerDerivedPointer) {
  llvm::LLVMContext Context;
  std::unique_ptr<llvm::Module> Module = parseModule(Context, R"(
define i32 @translated_block(ptr %state, ptr %runtime) nounwind {
  %slot = getelementptr i64, ptr %state, i64 1
  %guest_address = load i64, ptr %slot, align 1
  %host_pointer = inttoptr i64 %guest_address to ptr
  %value = load i32, ptr %host_pointer, align 1
  ret i32 %value
}
)");
  ASSERT_NE(Module, nullptr);
  expectViolation(verify(*Module),
                  TranslationIRViolation::GuestIntegerToPointer);
}

TEST(TranslationIRVerifier, RejectsAnExternalOutsideTheAllowlist) {
  llvm::LLVMContext Context;
  std::unique_ptr<llvm::Module> Module = parseModule(Context, R"(
declare i32 @host_escape(ptr) nounwind

define i32 @translated_block(ptr %state, ptr %runtime) nounwind {
  %stop = call i32 @host_escape(ptr %runtime) nounwind
  ret i32 %stop
}
)");
  ASSERT_NE(Module, nullptr);
  expectViolation(verify(*Module),
                  TranslationIRViolation::ExternalSymbolNotAllowed);
}

TEST(TranslationIRVerifier, RejectsHostExceptionHandling) {
  llvm::LLVMContext Context;
  std::unique_ptr<llvm::Module> Module = parseModule(Context, R"(
declare i32 @nvd_rt_dispatch(ptr, i64) nounwind
declare i32 @__gxx_personality_v0(...)

define i32 @translated_block(ptr %state, ptr %runtime)
    personality ptr @__gxx_personality_v0 {
entry:
  %stop = invoke i32 @nvd_rt_dispatch(ptr %runtime, i64 0)
      to label %done unwind label %fault
done:
  ret i32 %stop
fault:
  %lp = landingpad { ptr, i32 } cleanup
  resume { ptr, i32 } %lp
}
)");
  ASSERT_NE(Module, nullptr);
  expectViolation(verify(*Module),
                  TranslationIRViolation::HostExceptionHandling);
}

TEST(TranslationIRVerifier, RejectsObservablePoisonAndUnprovenFlags) {
  llvm::LLVMContext Context;
  std::unique_ptr<llvm::Module> Poison = parseModule(Context, R"(
define i32 @translated_block(ptr %state, ptr %runtime) nounwind {
  ret i32 poison
}
)");
  ASSERT_NE(Poison, nullptr);
  expectViolation(verify(*Poison),
                  TranslationIRViolation::GuestObservableUndefOrPoison);

  std::unique_ptr<llvm::Module> DerivedPoison = parseModule(Context, R"(
define i32 @translated_block(ptr %state, ptr %runtime) nounwind {
  %derived = xor i32 poison, 7
  ret i32 %derived
}
)");
  ASSERT_NE(DerivedPoison, nullptr);
  expectViolation(verify(*DerivedPoison),
                  TranslationIRViolation::GuestObservableUndefOrPoison);

  std::unique_ptr<llvm::Module> Flags = parseModule(Context, R"(
define i32 @translated_block(ptr %state, ptr %runtime) nounwind {
  %slot = getelementptr i32, ptr %state, i64 1
  %old = load i32, ptr %slot, align 1
  %next = add nsw i32 %old, 1
  ret i32 %next
}
)");
  ASSERT_NE(Flags, nullptr);
  expectViolation(verify(*Flags),
                  TranslationIRViolation::UnprovenPoisonGeneratingFlag);
}

TEST(TranslationIRVerifier, RejectsDirectGuestMemoryAndFrozenPoison) {
  llvm::LLVMContext Context;
  std::unique_ptr<llvm::Module> Direct = parseModule(Context, R"(
define i32 @translated_block(ptr %state, ptr %runtime) nounwind {
  %slot = getelementptr ptr, ptr %state, i64 1
  %guest_pointer = load ptr, ptr %slot, align 1
  %value = load i32, ptr %guest_pointer, align 1
  ret i32 %value
}
)");
  ASSERT_NE(Direct, nullptr);
  expectViolation(verify(*Direct), TranslationIRViolation::HostPointerExposure);

  std::unique_ptr<llvm::Module> Frozen = parseModule(Context, R"(
define i32 @translated_block(ptr %state, ptr %runtime) nounwind {
  %defined = freeze i32 poison
  ret i32 %defined
}
)");
  ASSERT_NE(Frozen, nullptr);
  expectViolation(verify(*Frozen),
                  TranslationIRViolation::UnprovenUndefinedBehavior);
}

TEST(TranslationIRVerifier, RejectsOutOfBoundsStateAccessAndIndirectCalls) {
  llvm::LLVMContext Context;
  std::unique_ptr<llvm::Module> OutOfBounds = parseModule(Context, R"(
define i32 @translated_block(ptr %state, ptr %runtime) nounwind {
  %slot = getelementptr i64, ptr %state, i64 8
  %value = load i32, ptr %slot, align 1
  ret i32 %value
}
)");
  ASSERT_NE(OutOfBounds, nullptr);
  expectViolation(verify(*OutOfBounds),
                  TranslationIRViolation::UnboundedStateOrRuntimeAccess);

  std::unique_ptr<llvm::Module> Indirect = parseModule(Context, R"(
define i32 @translated_block(ptr %state, ptr %runtime) nounwind {
  %slot = getelementptr ptr, ptr %runtime, i64 0
  %callee = load ptr, ptr %slot, align 1
  %stop = call i32 %callee(ptr %state, ptr %runtime) nounwind
  ret i32 %stop
}
)");
  ASSERT_NE(Indirect, nullptr);
  expectViolation(verify(*Indirect),
                  TranslationIRViolation::HostPointerExposure);
}

TEST(TranslationIRVerifier,
     RejectsMemoryIntrinsicsInternalDefinitionsAndPointerExposure) {
  llvm::LLVMContext Context;
  std::unique_ptr<llvm::Module> MemoryIntrinsic = parseModule(Context, R"(
declare void @llvm.memcpy.p0.p0.i64(ptr noalias nocapture writeonly,
                                    ptr noalias nocapture readonly,
                                    i64, i1 immarg) nounwind

define i32 @translated_block(ptr %state, ptr %runtime) nounwind {
  call void @llvm.memcpy.p0.p0.i64(ptr %state, ptr %runtime, i64 8, i1 false) nounwind
  ret i32 0
}
)");
  ASSERT_NE(MemoryIntrinsic, nullptr);
  expectViolation(verify(*MemoryIntrinsic),
                  TranslationIRViolation::IntrinsicNotAllowed);

  std::unique_ptr<llvm::Module> InternalDefinition = parseModule(Context, R"(
define internal i32 @helper(ptr %guest_pointer, ptr %unused) nounwind {
  %value = load i32, ptr %guest_pointer, align 1
  ret i32 %value
}

define i32 @translated_block(ptr %state, ptr %runtime) nounwind {
  %stop = call i32 @helper(ptr %state, ptr %runtime) nounwind
  ret i32 %stop
}
)");
  ASSERT_NE(InternalDefinition, nullptr);
  expectViolation(verify(*InternalDefinition),
                  TranslationIRViolation::NonStandardBlockABI);

  std::unique_ptr<llvm::Module> PointerExposure = parseModule(Context, R"(
define i32 @translated_block(ptr %state, ptr %runtime) nounwind {
  %host_address = ptrtoint ptr %state to i64
  %stop = trunc i64 %host_address to i32
  ret i32 %stop
}
)");
  ASSERT_NE(PointerExposure, nullptr);
  expectViolation(verify(*PointerExposure),
                  TranslationIRViolation::HostPointerExposure);
}

TEST(TranslationIRVerifier,
     RejectsDirectBlockCallsAndMismatchedRuntimeHelperTypes) {
  llvm::LLVMContext Context;
  std::unique_ptr<llvm::Module> DirectCall = parseModule(Context, R"(
define i32 @other_block(ptr %state, ptr %runtime) nounwind {
  ret i32 7
}

define i32 @translated_block(ptr %state, ptr %runtime) nounwind {
  %stop = call i32 @other_block(ptr %state, ptr %runtime) nounwind
  ret i32 %stop
}
)");
  ASSERT_NE(DirectCall, nullptr);
  expectViolation(verify(*DirectCall), TranslationIRViolation::DirectBlockCall);

  std::unique_ptr<llvm::Module> WrongHelperType = parseModule(Context, R"(
declare i64 @nvd_rt_dispatch(ptr, i64) nounwind

define i32 @translated_block(ptr %state, ptr %runtime) nounwind {
  %wide = call i64 @nvd_rt_dispatch(ptr %runtime, i64 0) nounwind
  %stop = trunc i64 %wide to i32
  ret i32 %stop
}
)");
  ASSERT_NE(WrongHelperType, nullptr);
  expectViolation(verify(*WrongHelperType),
                  TranslationIRViolation::RuntimeHelperABIMismatch);
}

TEST(TranslationIRVerifier,
     RejectsUnapprovedIntrinsicsAndHostAddressObservation) {
  llvm::LLVMContext Context;
  std::unique_ptr<llvm::Module> HostClock = parseModule(Context, R"(
declare i64 @llvm.readcyclecounter() nounwind

define i32 @translated_block(ptr %state, ptr %runtime) nounwind {
  %host_ticks = call i64 @llvm.readcyclecounter() nounwind
  %stop = trunc i64 %host_ticks to i32
  ret i32 %stop
}
)");
  ASSERT_NE(HostClock, nullptr);
  expectViolation(verify(*HostClock),
                  TranslationIRViolation::IntrinsicNotAllowed);

  std::unique_ptr<llvm::Module> PointerLoad = parseModule(Context, R"(
define i32 @translated_block(ptr %state, ptr %runtime) nounwind {
  %host_pointer = load ptr, ptr %runtime, align 1
  %is_null = icmp eq ptr %host_pointer, null
  %stop = zext i1 %is_null to i32
  ret i32 %stop
}
)");
  ASSERT_NE(PointerLoad, nullptr);
  expectViolation(verify(*PointerLoad),
                  TranslationIRViolation::HostPointerExposure);

  std::unique_ptr<llvm::Module> PointerAddress = parseModule(Context, R"(
define i32 @translated_block(ptr %state, ptr %runtime) nounwind {
  %host_address = ptrtoaddr ptr %state to i64
  %stop = trunc i64 %host_address to i32
  ret i32 %stop
}
)");
  ASSERT_NE(PointerAddress, nullptr);
  expectViolation(verify(*PointerAddress),
                  TranslationIRViolation::HostPointerExposure);
}

TEST(TranslationIRVerifier,
     RejectsPrivateOutOfBoundsAndPrivatePointerHelperArguments) {
  llvm::LLVMContext Context;
  std::unique_ptr<llvm::Module> OutOfBounds = parseModule(Context, R"(
@table = internal constant i8 0

define i32 @translated_block(ptr %state, ptr %runtime) nounwind {
  %slot = getelementptr i8, ptr @table, i64 8
  %value = load i8, ptr %slot, align 1
  %stop = zext i8 %value to i32
  ret i32 %stop
}
)");
  ASSERT_NE(OutOfBounds, nullptr);
  expectViolation(verify(*OutOfBounds),
                  TranslationIRViolation::UnboundedPrivateMemoryAccess);

  std::unique_ptr<llvm::Module> PointerArgument = parseModule(Context, R"(
@table = internal constant i8 0
declare void @explicit_helper(ptr) nounwind

define i32 @translated_block(ptr %state, ptr %runtime) nounwind {
  call void @explicit_helper(ptr @table) nounwind
  ret i32 0
}
)");
  ASSERT_NE(PointerArgument, nullptr);
  expectViolation(verify(*PointerArgument),
                  TranslationIRViolation::RuntimeHelperABIMismatch);
}

TEST(TranslationIRVerifier, RejectsAtomicsAndFloatingPointIR) {
  llvm::LLVMContext Context;
  std::unique_ptr<llvm::Module> Atomic = parseModule(Context, R"(
define i32 @translated_block(ptr %state, ptr %runtime) nounwind {
  %old = atomicrmw add ptr %state, i32 1 seq_cst
  ret i32 %old
}
)");
  ASSERT_NE(Atomic, nullptr);
  expectViolation(verify(*Atomic),
                  TranslationIRViolation::UnsupportedHostIROperation);

  std::unique_ptr<llvm::Module> AtomicLoad = parseModule(Context, R"(
define i32 @translated_block(ptr %state, ptr %runtime) nounwind {
  %old = load atomic i32, ptr %state seq_cst, align 4
  ret i32 %old
}
)");
  ASSERT_NE(AtomicLoad, nullptr);
  expectViolation(verify(*AtomicLoad),
                  TranslationIRViolation::UnsupportedHostIROperation);

  std::unique_ptr<llvm::Module> AtomicStore = parseModule(Context, R"(
define i32 @translated_block(ptr %state, ptr %runtime) nounwind {
  store atomic i32 1, ptr %state seq_cst, align 4
  ret i32 0
}
)");
  ASSERT_NE(AtomicStore, nullptr);
  expectViolation(verify(*AtomicStore),
                  TranslationIRViolation::UnsupportedHostIROperation);

  std::unique_ptr<llvm::Module> CompareExchange = parseModule(Context, R"(
define i32 @translated_block(ptr %state, ptr %runtime) nounwind {
  %pair = cmpxchg ptr %state, i32 0, i32 1 seq_cst seq_cst, align 4
  %old = extractvalue { i32, i1 } %pair, 0
  ret i32 %old
}
)");
  ASSERT_NE(CompareExchange, nullptr);
  expectViolation(verify(*CompareExchange),
                  TranslationIRViolation::UnsupportedHostIROperation);

  std::unique_ptr<llvm::Module> Fence = parseModule(Context, R"(
define i32 @translated_block(ptr %state, ptr %runtime) nounwind {
  fence seq_cst
  ret i32 0
}
)");
  ASSERT_NE(Fence, nullptr);
  expectViolation(verify(*Fence),
                  TranslationIRViolation::UnsupportedHostIROperation);

  std::unique_ptr<llvm::Module> FloatingPoint = parseModule(Context, R"(
define i32 @translated_block(ptr %state, ptr %runtime) nounwind {
  %sum = fadd float 1.0, 2.0
  %stop = bitcast float %sum to i32
  ret i32 %stop
}
)");
  ASSERT_NE(FloatingPoint, nullptr);
  expectViolation(verify(*FloatingPoint),
                  TranslationIRViolation::UnsupportedHostIROperation);
}

TEST(TranslationIRVerifier, RejectsVectorAndVariadicStateOperations) {
  llvm::LLVMContext Context;
  std::unique_ptr<llvm::Module> Vector = parseModule(Context, R"(
define i32 @translated_block(ptr %state, ptr %runtime) nounwind {
  %value = extractelement <4 x i32> zeroinitializer, i32 0
  ret i32 %value
}
)");
  ASSERT_NE(Vector, nullptr);
  expectViolation(verify(*Vector),
                  TranslationIRViolation::UnsupportedHostIROperation);

  std::unique_ptr<llvm::Module> Variadic = parseModule(Context, R"(
define i32 @translated_block(ptr %state, ptr %runtime) nounwind {
  %value = va_arg ptr %state, i32
  ret i32 %value
}
)");
  ASSERT_NE(Variadic, nullptr);
  expectViolation(verify(*Variadic),
                  TranslationIRViolation::UnsupportedHostIROperation);

  std::unique_ptr<llvm::Module> AggregateLoad = parseModule(Context, R"(
define i32 @translated_block(ptr %state, ptr %runtime) nounwind {
  %aggregate = load [8 x i8], ptr %state, align 1
  %value = extractvalue [8 x i8] %aggregate, 0
  %stop = zext i8 %value to i32
  ret i32 %stop
}
)");
  ASSERT_NE(AggregateLoad, nullptr);
  expectViolation(verify(*AggregateLoad),
                  TranslationIRViolation::UnsupportedHostIROperation);

  std::unique_ptr<llvm::Module> AggregateStore = parseModule(Context, R"(
define i32 @translated_block(ptr %state, ptr %runtime) nounwind {
  store [8 x i8] zeroinitializer, ptr %state, align 1
  ret i32 0
}
)");
  ASSERT_NE(AggregateStore, nullptr);
  expectViolation(verify(*AggregateStore),
                  TranslationIRViolation::UnsupportedHostIROperation);
}

TEST(TranslationIRVerifier,
     RejectsPrivateWritesPoisonConstantsAndUnprovenAlignment) {
  llvm::LLVMContext Context;
  std::unique_ptr<llvm::Module> PrivateWrite = parseModule(Context, R"(
@table = internal constant i32 0

define i32 @translated_block(ptr %state, ptr %runtime) nounwind {
  store i32 1, ptr @table, align 1
  ret i32 0
}
)");
  ASSERT_NE(PrivateWrite, nullptr);
  expectViolation(verify(*PrivateWrite),
                  TranslationIRViolation::UnsupportedHostIROperation);

  std::unique_ptr<llvm::Module> PoisonConstant = parseModule(Context, R"(
@table = internal constant i32 poison

define i32 @translated_block(ptr %state, ptr %runtime) nounwind {
  %value = load i32, ptr @table, align 1
  ret i32 %value
}
)");
  ASSERT_NE(PoisonConstant, nullptr);
  expectViolation(verify(*PoisonConstant),
                  TranslationIRViolation::GuestObservableUndefOrPoison);

  std::unique_ptr<llvm::Module> Alignment = parseModule(Context, R"(
define i32 @translated_block(ptr %state, ptr %runtime) nounwind {
  %value = load i32, ptr %state, align 8
  ret i32 %value
}
)");
  ASSERT_NE(Alignment, nullptr);
  expectViolation(verify(*Alignment),
                  TranslationIRViolation::UnprovenUndefinedBehavior);
}

TEST(TranslationIRVerifier, RejectsUntotalScalarOperations) {
  llvm::LLVMContext Context;
  std::unique_ptr<llvm::Module> Shift = parseModule(Context, R"(
define i32 @translated_block(ptr %state, ptr %runtime) nounwind {
  %amount = load i32, ptr %state, align 1
  %value = shl i32 1, %amount
  ret i32 %value
}
)");
  ASSERT_NE(Shift, nullptr);
  expectViolation(verify(*Shift),
                  TranslationIRViolation::UnprovenUndefinedBehavior);

  std::unique_ptr<llvm::Module> Division = parseModule(Context, R"(
define i32 @translated_block(ptr %state, ptr %runtime) nounwind {
  %divisor = load i32, ptr %state, align 1
  %value = udiv i32 7, %divisor
  ret i32 %value
}
)");
  ASSERT_NE(Division, nullptr);
  expectViolation(verify(*Division),
                  TranslationIRViolation::UnprovenUndefinedBehavior);

  std::unique_ptr<llvm::Module> Frozen = parseModule(Context, R"(
define i32 @translated_block(ptr %state, ptr %runtime) nounwind {
  %input = load i32, ptr %state, align 1
  %value = freeze i32 %input
  ret i32 %value
}
)");
  ASSERT_NE(Frozen, nullptr);
  expectViolation(verify(*Frozen),
                  TranslationIRViolation::UnprovenUndefinedBehavior);
}

TEST(TranslationIRVerifier, AcceptsProvenShiftAndNonzeroDivision) {
  llvm::LLVMContext Context;
  std::unique_ptr<llvm::Module> Module = parseModule(Context, R"(
define i32 @translated_block(ptr %state, ptr %runtime) nounwind {
  %input = load i32, ptr %state, align 1
  %amount = and i32 %input, 31
  %shifted = shl i32 1, %amount
  %divisor = or i32 %input, 1
  %value = udiv i32 %shifted, %divisor
  ret i32 %value
}
)");
  ASSERT_NE(Module, nullptr);
  expectSuccess(verify(*Module));
}

TEST(TranslationIRVerifier, RejectsHelperAttributesAndMissingMemorySlots) {
  llvm::LLVMContext Context;
  std::unique_ptr<llvm::Module> Attributes = parseModule(Context, R"(
declare i32 @nvd_rt_dispatch(ptr, i64) nounwind memory(none)

define i32 @translated_block(ptr %state, ptr %runtime) nounwind {
  %stop = call i32 @nvd_rt_dispatch(ptr %runtime, i64 0) nounwind
  ret i32 %stop
}
)");
  ASSERT_NE(Attributes, nullptr);
  expectViolation(verify(*Attributes),
                  TranslationIRViolation::RuntimeHelperABIMismatch);

  std::unique_ptr<llvm::Module> MissingSlot = parseModule(Context, R"(
define i32 @translated_block(ptr %state, ptr %runtime) nounwind {
  %value = load i32, ptr %state, align 1
  ret i32 %value
}
)");
  ASSERT_NE(MissingSlot, nullptr);
  expectViolation(neverd::translate::verifyTranslationIR(
                      *MissingSlot, llvm::Triple(HostTriple),
                      llvm::DataLayout(HostLayout), 64, 64),
                  TranslationIRViolation::MemorySlotNotAllowed);
}

TEST(TranslationIRVerifier, RejectsCyclesThatBypassRuntimePolling) {
  llvm::LLVMContext Context;
  std::unique_ptr<llvm::Module> Module = parseModule(Context, R"(
define i32 @translated_block(ptr %state, ptr %runtime) nounwind {
entry:
  br label %loop
loop:
  br label %loop
}
)");
  ASSERT_NE(Module, nullptr);
  expectViolation(verify(*Module), TranslationIRViolation::DispatchCycle);
}

TEST(TranslationIRVerifier,
     RejectsFunctionVisibilityAddressSpaceAttributesAndMetadata) {
  llvm::LLVMContext Context;
  std::unique_ptr<llvm::Module> Visibility = parseModule(Context, R"(
define i32 @translated_block(ptr %state, ptr %runtime) nounwind {
  ret i32 0
}
)");
  ASSERT_NE(Visibility, nullptr);
  Visibility->getFunction("translated_block")
      ->setVisibility(llvm::GlobalValue::DefaultVisibility);
  expectViolation(verify(*Visibility),
                  TranslationIRViolation::NonStandardBlockABI);

  std::unique_ptr<llvm::Module> AddressSpace = parseModule(Context, R"(
define i32 @translated_block(ptr %state, ptr %runtime) addrspace(1) nounwind {
  ret i32 0
}
)");
  ASSERT_NE(AddressSpace, nullptr);
  expectViolation(verify(*AddressSpace),
                  TranslationIRViolation::UnsupportedHostIROperation);

  std::unique_ptr<llvm::Module> ParameterAttribute = parseModule(Context, R"(
define i32 @translated_block(ptr nonnull %state, ptr %runtime) nounwind {
  ret i32 0
}
)");
  ASSERT_NE(ParameterAttribute, nullptr);
  expectViolation(verify(*ParameterAttribute),
                  TranslationIRViolation::UnsupportedHostIROperation);

  std::unique_ptr<llvm::Module> Metadata = parseModule(Context, R"(
define i32 @translated_block(ptr %state, ptr %runtime) nounwind !type !0 {
  ret i32 0
}
!0 = !{i64 0, !"translation.type"}
)");
  ASSERT_NE(Metadata, nullptr);
  expectViolation(verify(*Metadata),
                  TranslationIRViolation::UnprovenSemanticMetadata);

  std::unique_ptr<llvm::Module> Prologue = parseModule(Context, R"(
define i32 @translated_block(ptr %state, ptr %runtime) nounwind prologue i32 1 {
  ret i32 0
}
)");
  ASSERT_NE(Prologue, nullptr);
  expectViolation(verify(*Prologue),
                  TranslationIRViolation::UnsupportedHostIROperation);

  std::unique_ptr<llvm::Module> Partition = parseModule(Context, R"(
define i32 @translated_block(ptr %state, ptr %runtime) nounwind partition "translation.part" {
  ret i32 0
}
)");
  ASSERT_NE(Partition, nullptr);
  expectViolation(verify(*Partition),
                  TranslationIRViolation::UnsupportedHostIROperation);
}

TEST(TranslationIRVerifier,
     AcceptsCanonicalIntrinsicsAndRejectsIntrinsicAnnotations) {
  llvm::LLVMContext Context;
  std::unique_ptr<llvm::Module> Canonical = parseModule(Context, R"(
declare i32 @llvm.ctpop.i32(i32)
declare i32 @llvm.ctlz.i32(i32, i1 immarg)

define i32 @translated_block(ptr %state, ptr %runtime) nounwind {
  %bits = load i32, ptr %state, align 1
  %ones = call i32 @llvm.ctpop.i32(i32 %bits) nounwind
  %leading = call i32 @llvm.ctlz.i32(i32 %bits, i1 false) nounwind
  %sum = add i32 %ones, %leading
  ret i32 %sum
}
)");
  ASSERT_NE(Canonical, nullptr);
  expectSuccess(verify(*Canonical));

  std::unique_ptr<llvm::Module> DeclarationAttribute = parseModule(Context, R"(
declare i32 @llvm.ctpop.i32(i32)

define i32 @translated_block(ptr %state, ptr %runtime) nounwind {
  %value = call i32 @llvm.ctpop.i32(i32 7) nounwind
  ret i32 %value
}
)");
  ASSERT_NE(DeclarationAttribute, nullptr);
  DeclarationAttribute->getFunction("llvm.ctpop.i32")
      ->addFnAttr(llvm::Attribute::NoInline);
  expectViolation(verify(*DeclarationAttribute),
                  TranslationIRViolation::IntrinsicNotAllowed);

  std::unique_ptr<llvm::Module> ReturnRange = parseModule(Context, R"(
declare i32 @llvm.ctpop.i32(i32)

define i32 @translated_block(ptr %state, ptr %runtime) nounwind {
  %value = call range(i32 0, 2) i32 @llvm.ctpop.i32(i32 7) nounwind
  ret i32 %value
}
)");
  ASSERT_NE(ReturnRange, nullptr);
  expectViolation(verify(*ReturnRange),
                  TranslationIRViolation::UnprovenPoisonGeneratingFlag);

  std::unique_ptr<llvm::Module> OperandBundle = parseModule(Context, R"(
declare i32 @llvm.ctpop.i32(i32)

define i32 @translated_block(ptr %state, ptr %runtime) nounwind {
  %value = call i32 @llvm.ctpop.i32(i32 7) [ "translation.bundle"(i32 1) ]
  ret i32 %value
}
)");
  ASSERT_NE(OperandBundle, nullptr);
  expectViolation(verify(*OperandBundle),
                  TranslationIRViolation::UnsupportedHostIROperation);
}

TEST(TranslationIRVerifier, ValidatesMemorySlotPolicyWithIndexedLookup) {
  llvm::LLVMContext Context;
  std::unique_ptr<llvm::Module> Module = parseModule(Context, R"(
define i32 @translated_block(ptr %state, ptr %runtime) nounwind {
  %slot = getelementptr i8, ptr %state, i64 4
  %value = load i32, ptr %slot, align 8
  ret i32 %value
}
)");
  ASSERT_NE(Module, nullptr);

  const TranslationIRMemorySlot Overlap[] = {
      {TranslationIRMemoryRegion::State, 0, 8, TranslationIRMemoryAccess::Read,
       8},
      {TranslationIRMemoryRegion::State, 4, 8, TranslationIRMemoryAccess::Read,
       4}};
  expectViolation(neverd::translate::verifyTranslationIR(
                      *Module, llvm::Triple(HostTriple),
                      llvm::DataLayout(HostLayout), 64, 64, Overlap),
                  TranslationIRViolation::InvalidPolicy);

  const TranslationIRMemorySlot WriteOnly[] = {
      {TranslationIRMemoryRegion::State, 0, 8, TranslationIRMemoryAccess::Write,
       8}};
  expectViolation(neverd::translate::verifyTranslationIR(
                      *Module, llvm::Triple(HostTriple),
                      llvm::DataLayout(HostLayout), 64, 64, WriteOnly),
                  TranslationIRViolation::MemorySlotNotAllowed);

  const TranslationIRMemorySlot Misaligned[] = {
      {TranslationIRMemoryRegion::State, 0, 8, TranslationIRMemoryAccess::Read,
       8}};
  expectViolation(neverd::translate::verifyTranslationIR(
                      *Module, llvm::Triple(HostTriple),
                      llvm::DataLayout(HostLayout), 64, 64, Misaligned),
                  TranslationIRViolation::UnprovenUndefinedBehavior);
}

TEST(TranslationIRVerifier,
     AcceptsAdjacentMemorySlotsButRejectsGapsAndPermissionChanges) {
  llvm::LLVMContext Context;
  std::unique_ptr<llvm::Module> Module = parseModule(Context, R"(
define i32 @translated_block(ptr %state, ptr %runtime) nounwind {
  %slot = getelementptr i8, ptr %state, i64 8
  store i32 0, ptr %slot, align 8
  ret i32 0
}
)");
  ASSERT_NE(Module, nullptr);

  const TranslationIRMemorySlot Adjacent[] = {
      {TranslationIRMemoryRegion::State, 8, 1, TranslationIRMemoryAccess::Write,
       8},
      {TranslationIRMemoryRegion::State, 9, 1, TranslationIRMemoryAccess::Write,
       1},
      {TranslationIRMemoryRegion::State, 10, 1,
       TranslationIRMemoryAccess::Write, 2},
      {TranslationIRMemoryRegion::State, 11, 1,
       TranslationIRMemoryAccess::Write, 1}};
  EXPECT_FALSE(static_cast<bool>(neverd::translate::verifyTranslationIR(
      *Module, llvm::Triple(HostTriple), llvm::DataLayout(HostLayout), 64, 64,
      Adjacent)));

  const TranslationIRMemorySlot Gap[] = {
      {TranslationIRMemoryRegion::State, 8, 2, TranslationIRMemoryAccess::Write,
       8},
      {TranslationIRMemoryRegion::State, 11, 1,
       TranslationIRMemoryAccess::Write, 1}};
  expectViolation(neverd::translate::verifyTranslationIR(
                      *Module, llvm::Triple(HostTriple),
                      llvm::DataLayout(HostLayout), 64, 64, Gap),
                  TranslationIRViolation::MemorySlotNotAllowed);

  const TranslationIRMemorySlot PermissionChange[] = {
      {TranslationIRMemoryRegion::State, 8, 2, TranslationIRMemoryAccess::Write,
       8},
      {TranslationIRMemoryRegion::State, 10, 2, TranslationIRMemoryAccess::Read,
       2}};
  expectViolation(neverd::translate::verifyTranslationIR(
                      *Module, llvm::Triple(HostTriple),
                      llvm::DataLayout(HostLayout), 64, 64, PermissionChange),
                  TranslationIRViolation::MemorySlotNotAllowed);
}

TEST(TranslationIRVerifier,
     RejectsUnknownMemoryRootsAndBackendLibcallIntegerWidths) {
  llvm::LLVMContext Context;
  std::unique_ptr<llvm::Module> UnknownRoot = parseModule(Context, R"(
define i32 @translated_block(ptr %state, ptr %runtime) nounwind {
  %value = load i32, ptr null, align 1
  ret i32 %value
}
)");
  ASSERT_NE(UnknownRoot, nullptr);
  expectViolation(verify(*UnknownRoot),
                  TranslationIRViolation::DirectGuestMemoryAccess);

  std::unique_ptr<llvm::Module> WideDivision = parseModule(Context, R"(
define i32 @translated_block(ptr %state, ptr %runtime) nounwind {
  %wide = load i128, ptr %state, align 1
  %value = udiv i128 %wide, 3
  %result = trunc i128 %value to i32
  ret i32 %result
}
)");
  ASSERT_NE(WideDivision, nullptr);
  expectViolation(verify(*WideDivision),
                  TranslationIRViolation::BackendLibcallRisk);
}

TEST(TranslationIRVerifier, RejectsSignedDivisionOverflowAndAcceptsSafeCase) {
  llvm::LLVMContext Context;
  std::unique_ptr<llvm::Module> Overflow = parseModule(Context, R"(
define i32 @translated_block(ptr %state, ptr %runtime) nounwind {
  %value = sdiv i32 -2147483648, -1
  ret i32 %value
}
)");
  ASSERT_NE(Overflow, nullptr);
  expectViolation(verify(*Overflow),
                  TranslationIRViolation::UnprovenUndefinedBehavior);

  std::unique_ptr<llvm::Module> Safe = parseModule(Context, R"(
define i32 @translated_block(ptr %state, ptr %runtime) nounwind {
  %input = load i32, ptr %state, align 1
  %divisor = or i32 %input, 1
  %value = srem i32 7, %divisor
  ret i32 %value
}
)");
  ASSERT_NE(Safe, nullptr);
  expectSuccess(verify(*Safe));
}

TEST(TranslationIRVerifier, RejectsModuleAndGlobalCodegenMetadata) {
  llvm::LLVMContext Context;
  std::unique_ptr<llvm::Module> NamedMetadata = parseModule(Context, R"(
define i32 @translated_block(ptr %state, ptr %runtime) nounwind {
  ret i32 0
}
!translation.module = !{!0}
!0 = !{i32 1}
)");
  ASSERT_NE(NamedMetadata, nullptr);
  expectViolation(verify(*NamedMetadata),
                  TranslationIRViolation::UnprovenSemanticMetadata);

  std::unique_ptr<llvm::Module> GlobalSection = parseModule(Context, R"(
@table = internal constant i32 0, section ".translation.private"

define i32 @translated_block(ptr %state, ptr %runtime) nounwind {
  %value = load i32, ptr @table, align 1
  ret i32 %value
}
)");
  ASSERT_NE(GlobalSection, nullptr);
  expectViolation(verify(*GlobalSection),
                  TranslationIRViolation::ExternalSymbolNotAllowed);

  std::unique_ptr<llvm::Module> GlobalPartition = parseModule(Context, R"(
@table = internal constant i32 0, partition "translation.part"

define i32 @translated_block(ptr %state, ptr %runtime) nounwind {
  %value = load i32, ptr @table, align 1
  ret i32 %value
}
)");
  ASSERT_NE(GlobalPartition, nullptr);
  expectViolation(verify(*GlobalPartition),
                  TranslationIRViolation::ExternalSymbolNotAllowed);

  std::unique_ptr<llvm::Module> GlobalMetadata = parseModule(Context, R"(
@table = internal constant i32 0, !type !0

define i32 @translated_block(ptr %state, ptr %runtime) nounwind {
  %value = load i32, ptr @table, align 1
  ret i32 %value
}
!0 = !{i64 0, !"translation.type"}
)");
  ASSERT_NE(GlobalMetadata, nullptr);
  expectViolation(verify(*GlobalMetadata),
                  TranslationIRViolation::UnprovenSemanticMetadata);

  std::unique_ptr<llvm::Module> AggregateGlobal = parseModule(Context, R"(
@table = internal constant [1024 x i8] zeroinitializer

define i32 @translated_block(ptr %state, ptr %runtime) nounwind {
  ret i32 0
}
)");
  ASSERT_NE(AggregateGlobal, nullptr);
  expectViolation(verify(*AggregateGlobal),
                  TranslationIRViolation::ExternalSymbolNotAllowed);
}

} // namespace
