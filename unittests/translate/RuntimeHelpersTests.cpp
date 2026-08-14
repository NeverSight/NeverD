//===- RuntimeHelpersTests.cpp - Native translation helper tests ---------===//

#include "gtest/gtest.h"

#include "neverd/translate/GuestMemoryRuntime.h"
#include "neverd/translate/RuntimeHelpers.h"

#include "llvm/ADT/Twine.h"
#include "llvm/AsmParser/Parser.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/Support/Error.h"
#include "llvm/Support/SourceMgr.h"
#include "llvm/TargetParser/Triple.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <type_traits>
#include <vector>

using namespace neverd::translate;

namespace {

RuntimeCodeCredentialV1 credential(uint64_t Epoch = 9) {
  return {/*SessionID=*/1, /*BlockID=*/2, /*EntryPC=*/0x4000,
          /*CacheGeneration=*/3, /*CodeEpoch=*/Epoch};
}

std::unique_ptr<GuestMemoryRuntime> makeMemory() {
  GuestState State =
      llvm::cantFail(createZeroedGuestState(GuestArchitecture::X86_64));
  State.Memory.push_back({0x4000,
                          MemoryPermission::Read | MemoryPermission::Write,
                          0,
                          {0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88}});
  return llvm::cantFail(GuestMemoryRuntime::create(State));
}

std::unique_ptr<llvm::Module> parseProtocolModule(llvm::LLVMContext &Context,
                                                  llvm::StringRef Body) {
  constexpr llvm::StringLiteral HostTriple("aarch64-unknown-linux-gnu");
  constexpr llvm::StringLiteral HostLayout("e-p:64:64-i64:64-n32:64-S128");
  const std::string IR = (llvm::Twine("target datalayout = \"") + HostLayout +
                          "\"\ntarget triple = \"" + HostTriple + "\"\n" + Body)
                             .str();
  llvm::SMDiagnostic Diagnostic;
  std::unique_ptr<llvm::Module> Module =
      llvm::parseAssemblyString(IR, Diagnostic, Context);
  EXPECT_NE(Module, nullptr);
  if (Module)
    for (llvm::Function &Function : *Module)
      if (!Function.isDeclaration())
        Function.setVisibility(llvm::GlobalValue::HiddenVisibility);
  return Module;
}

void expectProtocolViolation(llvm::Error Error) {
  ASSERT_TRUE(static_cast<bool>(Error));
  bool Seen = false;
  llvm::handleAllErrors(
      std::move(Error), [&](const TranslationIRVerificationError &Failure) {
        Seen = true;
        EXPECT_EQ(Failure.reason(),
                  TranslationIRViolation::RuntimeProtocolViolation);
      });
  EXPECT_TRUE(Seen);
}

llvm::Error verifyCompleteRuntimeModule(const llvm::Module &Module) {
  const llvm::Triple HostTriple(Module.getTargetTriple());
  return verifyRuntimeTranslationIRV1(Module, HostTriple,
                                      Module.getDataLayout(), 1);
}

} // namespace

TEST(RuntimeHelpers, CallFrameHasAClosedGeneratedPrefix) {
  static_assert(std::is_standard_layout_v<RuntimeCallFrameV1>);
  EXPECT_EQ(offsetof(RuntimeCallFrameV1, Control), 0u);
  EXPECT_EQ(offsetof(RuntimeCallFrameV1, Memory),
            sizeof(RuntimeControlBlockV1));
  EXPECT_GT(sizeof(RuntimeCallFrameV1), kRuntimeControlBlockSizeV1);

  std::unique_ptr<GuestMemoryRuntime> Memory = makeMemory();
  RuntimeCallFrameV1 Frame = llvm::cantFail(
      createRuntimeCallFrameV1(*Memory, credential(), credential()));
  EXPECT_EQ(Frame.Memory, Memory.get());
  EXPECT_EQ(Frame.Published, credential());
  EXPECT_EQ(Frame.Validated, credential());
  EXPECT_EQ(Frame.Control.Magic, kRuntimeABIMagicV1);
}

TEST(RuntimeHelpers, RejectsUnboundOrStaleCredentials) {
  std::unique_ptr<GuestMemoryRuntime> Memory = makeMemory();
  RuntimeCodeCredentialV1 Empty;
  EXPECT_TRUE(static_cast<bool>(
      createRuntimeCallFrameV1(*Memory, Empty, Empty).takeError()));
  EXPECT_TRUE(static_cast<bool>(
      createRuntimeCallFrameV1(*Memory, credential(7), credential(8))
          .takeError()));
  EXPECT_FALSE(static_cast<bool>(
      validateRuntimeCodeCredentialV1(credential(), credential())));
}

TEST(RuntimeHelpers, RegistryAndVerifierPolicyShareOneExactSurface) {
  const llvm::ArrayRef<RuntimeABIHelperBindingV1> Bindings =
      runtimeABIHelperBindingsV1();
  const llvm::ArrayRef<RuntimeABIHelperSignatureV1> Signatures =
      runtimeABIHelperSignaturesV1();
  ASSERT_EQ(Bindings.size(), 8u);
  ASSERT_EQ(Bindings.size(), Signatures.size());

  for (std::size_t Index = 0; Index != Bindings.size(); ++Index) {
    const RuntimeABIHelperBindingV1 &Binding = Bindings[Index];
    EXPECT_EQ(Binding.Name, Signatures[Index].Name);
    EXPECT_EQ(findRuntimeABIHelperBindingV1(Binding.Name), &Binding);
    if (Binding.Class == RuntimeABIHelperClassV1::Load) {
      EXPECT_NE(Binding.Load, nullptr);
      EXPECT_EQ(Binding.Store, nullptr);
    } else {
      EXPECT_EQ(Binding.Load, nullptr);
      EXPECT_NE(Binding.Store, nullptr);
    }
  }
  EXPECT_EQ(findRuntimeABIHelperBindingV1("nvd_rt_v1_poll"), nullptr);
  EXPECT_EQ(findRuntimeABIHelperBindingV1("nvd_rt_v1_validate_generation"),
            nullptr);
}

TEST(RuntimeHelpers, LoadsStoresAndFaultsUseTheFixedControlRecord) {
  std::unique_ptr<GuestMemoryRuntime> Memory = makeMemory();
  RuntimeCallFrameV1 Frame = llvm::cantFail(
      createRuntimeCallFrameV1(*Memory, credential(), credential()));

  EXPECT_EQ(nvd_rt_v1_load32_le(&Frame, 0x4000, 1),
            static_cast<uint32_t>(RuntimeABIExitKindV1::None));
  EXPECT_EQ(Frame.Control.ScalarResult, 0x44332211u);
  EXPECT_EQ(Frame.Control.Exit.Kind, RuntimeABIExitKindV1::None);

  EXPECT_EQ(nvd_rt_v1_store16_le(&Frame, 0x4002, 0xa1b2, 1),
            static_cast<uint32_t>(RuntimeABIExitKindV1::None));
  EXPECT_EQ(nvd_rt_v1_load32_le(&Frame, 0x4000, 1),
            static_cast<uint32_t>(RuntimeABIExitKindV1::None));
  EXPECT_EQ(Frame.Control.ScalarResult, 0xa1b22211u);

  EXPECT_EQ(nvd_rt_v1_load64_le(&Frame, 0x4fff, 1),
            static_cast<uint32_t>(RuntimeABIExitKindV1::MemoryFault));
  EXPECT_EQ(Frame.Control.Exit.Kind, RuntimeABIExitKindV1::MemoryFault);
  EXPECT_EQ(Frame.Control.Exit.Fault, RuntimeMemoryFaultKindV1::Unmapped);
  EXPECT_EQ(Frame.Control.Exit.Address, 0x4fffu);
  EXPECT_EQ(Frame.Control.Exit.Size, 8u);
  EXPECT_EQ(Frame.Control.ScalarResult, 0u);
}

TEST(RuntimeHelpers, InvalidHostFramesFailClosedWithoutAmbientFallback) {
  RuntimeCallFrameV1 Frame;
  Frame.Control = makeRuntimeControlBlockV1(
      CodeInvalidationPolicy::InvalidateOnExecutableWrite);
  EXPECT_EQ(nvd_rt_v1_load8_le(&Frame, 0x4000, 1),
            static_cast<uint32_t>(RuntimeABIExitKindV1::MemoryFault));
  EXPECT_EQ(Frame.Control.Exit.Kind, RuntimeABIExitKindV1::MemoryFault);
  EXPECT_EQ(Frame.Control.Exit.Fault,
            RuntimeMemoryFaultKindV1::InvalidRuntimeFrame);
  EXPECT_FALSE(static_cast<bool>(validateRuntimeControlBlockV1(Frame.Control)));
  llvm::Expected<RuntimeMemoryFaultDetailsV1> LoadDetails =
      unpackRuntimeMemoryFaultDetailsV1(
          Frame.Control.Exit.Fault,
          {Frame.Control.Exit.Detail0, Frame.Control.Exit.Detail1});
  ASSERT_TRUE(static_cast<bool>(LoadDetails));
  EXPECT_EQ(LoadDetails->Access, RuntimeMemoryAccessKindV1::Read);
  EXPECT_EQ(LoadDetails->RequiredAlignment, 1u);

  EXPECT_EQ(nvd_rt_v1_store32_le(&Frame, 0x4000, 7, 4),
            static_cast<uint32_t>(RuntimeABIExitKindV1::MemoryFault));
  EXPECT_EQ(Frame.Control.Exit.Fault,
            RuntimeMemoryFaultKindV1::InvalidRuntimeFrame);
  EXPECT_EQ(Frame.Control.Exit.Size, 4u);
  EXPECT_FALSE(static_cast<bool>(validateRuntimeControlBlockV1(Frame.Control)));
  llvm::Expected<RuntimeMemoryFaultDetailsV1> StoreDetails =
      unpackRuntimeMemoryFaultDetailsV1(
          Frame.Control.Exit.Fault,
          {Frame.Control.Exit.Detail0, Frame.Control.Exit.Detail1});
  ASSERT_TRUE(static_cast<bool>(StoreDetails));
  EXPECT_EQ(StoreDetails->Access, RuntimeMemoryAccessKindV1::Write);
  EXPECT_EQ(StoreDetails->RequiredAlignment, 4u);
}

TEST(RuntimeHelpers, AcceptsCheckedLoadAndStoreControlFlow) {
  llvm::LLVMContext Context;
  std::unique_ptr<llvm::Module> Module = parseProtocolModule(Context, R"(
declare i32 @nvd_rt_v1_load64_le(ptr, i64, i32) nounwind
declare i32 @nvd_rt_v1_store64_le(ptr, i64, i64, i32) nounwind

define i32 @translated_block(ptr %state, ptr %runtime) nounwind {
entry:
  %load_status = call i32 @nvd_rt_v1_load64_le(
      ptr %runtime, i64 4096, i32 1) nounwind
  %load_ok = icmp eq i32 %load_status, 0
  br i1 %load_ok, label %loaded, label %load_failed
load_failed:
  ret i32 %load_status
loaded:
  %result_slot = getelementptr i8, ptr %runtime, i64 72
  %value = load i64, ptr %result_slot, align 8
  %store_status = call i32 @nvd_rt_v1_store64_le(
      ptr %runtime, i64 8192, i64 %value, i32 1) nounwind
  %store_ok = icmp eq i32 %store_status, 0
  br i1 %store_ok, label %done, label %store_failed
store_failed:
  ret i32 %store_status
done:
  ret i32 0
}
)");
  ASSERT_NE(Module, nullptr);
  EXPECT_FALSE(static_cast<bool>(verifyRuntimeABIHelperProtocolV1(*Module)));
  EXPECT_FALSE(static_cast<bool>(verifyCompleteRuntimeModule(*Module)));
}

TEST(RuntimeHelpers, RejectsIgnoredStatusAndStaleResultReads) {
  llvm::LLVMContext Context;
  std::unique_ptr<llvm::Module> Ignored = parseProtocolModule(Context, R"(
declare i32 @nvd_rt_v1_load64_le(ptr, i64, i32) nounwind

define i32 @translated_block(ptr %state, ptr %runtime) nounwind {
  %status = call i32 @nvd_rt_v1_load64_le(
      ptr %runtime, i64 4096, i32 1) nounwind
  ret i32 0
}
)");
  ASSERT_NE(Ignored, nullptr);
  expectProtocolViolation(verifyRuntimeABIHelperProtocolV1(*Ignored));
  expectProtocolViolation(verifyCompleteRuntimeModule(*Ignored));

  std::unique_ptr<llvm::Module> Stale = parseProtocolModule(Context, R"(
define i32 @translated_block(ptr %state, ptr %runtime) nounwind {
  %result_slot = getelementptr i8, ptr %runtime, i64 72
  %value = load i64, ptr %result_slot, align 8
  %status = trunc i64 %value to i32
  ret i32 %status
}
)");
  ASSERT_NE(Stale, nullptr);
  expectProtocolViolation(verifyRuntimeABIHelperProtocolV1(*Stale));
  expectProtocolViolation(verifyCompleteRuntimeModule(*Stale));
}

TEST(RuntimeHelpers, RejectsResultReadsBeforeSuccessAndForgedFailureExits) {
  llvm::LLVMContext Context;
  std::unique_ptr<llvm::Module> EarlyRead = parseProtocolModule(Context, R"(
declare i32 @nvd_rt_v1_load64_le(ptr, i64, i32) nounwind

define i32 @translated_block(ptr %state, ptr %runtime) nounwind {
entry:
  %status = call i32 @nvd_rt_v1_load64_le(
      ptr %runtime, i64 4096, i32 1) nounwind
  %result_slot = getelementptr i8, ptr %runtime, i64 72
  %value = load i64, ptr %result_slot, align 8
  %ok = icmp eq i32 %status, 0
  br i1 %ok, label %done, label %failed
failed:
  ret i32 %status
done:
  %low = trunc i64 %value to i32
  ret i32 %low
}
)");
  ASSERT_NE(EarlyRead, nullptr);
  expectProtocolViolation(verifyRuntimeABIHelperProtocolV1(*EarlyRead));

  std::unique_ptr<llvm::Module> ForgedExit = parseProtocolModule(Context, R"(
declare i32 @nvd_rt_v1_store64_le(ptr, i64, i64, i32) nounwind

define i32 @translated_block(ptr %state, ptr %runtime) nounwind {
entry:
  %status = call i32 @nvd_rt_v1_store64_le(
      ptr %runtime, i64 4096, i64 7, i32 1) nounwind
  %ok = icmp eq i32 %status, 0
  br i1 %ok, label %done, label %failed
failed:
  ret i32 0
done:
  ret i32 0
}
)");
  ASSERT_NE(ForgedExit, nullptr);
  expectProtocolViolation(verifyRuntimeABIHelperProtocolV1(*ForgedExit));
}
