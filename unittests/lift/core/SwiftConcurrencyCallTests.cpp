#include "../../../lib/sdk/capi/ObjCSourceBindings.h"
#include "RuntimeFunctionAddressFixture.h"
#include "gtest/gtest.h"

#include "neverd/ir/SourceABI.h"
#include "neverd/ir/TargetRegInfo.h"
#include "neverd/lift/AArch64/AArch64Regs.h"
#include "neverd/lift/X86/X86Regs.h"
#include "neverd/loader/Swift/SwiftRuntimeCalls.h"

using namespace neverd;

TEST(ObjCCallHints, SwiftMainActorSharedKeepsObjectAndMetatypeContext) {
  using namespace runtime_function_address_test;
  constexpr llvm::StringLiteral Name = "_$sScM6sharedScMvgZ";
  constexpr llvm::StringLiteral Provider =
      "/usr/lib/swift/libswift_Concurrency.dylib";
  for (const auto Architecture : {Arch::AArch64, Arch::X64}) {
    SCOPED_TRACE(static_cast<unsigned>(Architecture));
    auto Image = image(Architecture);
    Image.ImportPtrSlots[Slot] = Name.str();
    Image.DyldBindSlots[Slot] = {Name.str(), 0, Provider.str(), false};
    const auto Hint = swiftRuntimeSourceCallHint(Image, Slot);
    ASSERT_TRUE(Hint);
    const auto &Signature = Hint->Signature;
    const auto &TRI = getTargetRegInfo(Architecture);
    EXPECT_EQ(Signature.Origin, SourceFunctionTypeHint::OriginKind::SwiftSDK);
    EXPECT_EQ(Signature.Convention,
              SourceFunctionTypeHint::ConventionKind::Swift);
    ASSERT_TRUE(Signature.ReturnType);
    EXPECT_EQ(Signature.ReturnType->Kind, NdTypeKind::Ptr);
    EXPECT_EQ(Signature.ReturnType->Size, 8U);
    EXPECT_EQ(Signature.ReturnLocation.Kind,
              SourceABICarrierKind::IntegerRegister);
    EXPECT_EQ(Signature.ReturnLocation.RegisterOffset, TRI.IntReturnReg);
    EXPECT_EQ(Signature.ReturnLocation.ValueBytes, 8U);
    EXPECT_TRUE(Signature.ReturnComponents.empty());
    ASSERT_EQ(Signature.Parameters.size(), 1U);
    const auto &Context = Signature.Parameters.front();
    EXPECT_EQ(Context.Type->Kind, NdTypeKind::Ptr);
    EXPECT_EQ(Context.Type->Size, 8U);
    EXPECT_EQ(Context.TheRole, SourceParameterTypeHint::Role::SwiftContext);
    EXPECT_EQ(Context.Location.Kind, SourceABICarrierKind::IntegerRegister);
    EXPECT_EQ(Context.Location.RegisterOffset,
              Architecture == Arch::AArch64 ? a64reg::X20 : x86reg::R13);
    EXPECT_EQ(Context.Location.ValueBytes, 8U);
    std::string Diagnostic;
    EXPECT_TRUE(validateSourceABI(Signature, Diagnostic)) << Diagnostic;
    auto Call = HighExpr::makeCall("untrusted_name", Slot,
                                   {HighExpr::makeConst(0x4560, 8)});
    Call->Type = Signature.ReturnType;
    Call->SourceCallHint = std::make_shared<SourceCallTypeHint>(*Hint);
    ASSERT_TRUE(sdk::objcSourceCallBound(*Call, Image, {}));
    EXPECT_FALSE(runtimeCFunctionAddressHint(Image, Slot));
    for (unsigned Mutation = 0; Mutation != 10; ++Mutation) {
      auto Changed = std::make_shared<SourceCallTypeHint>(*Hint);
      auto &ABI = Changed->Signature;
      if (Mutation == 0)
        ABI.Parameters[0].TheRole = SourceParameterTypeHint::Role::Ordinary;
      if (Mutation == 1)
        ABI.Parameters[0].Location.RegisterOffset = TRI.IntParamRegs[0];
      if (Mutation == 2)
        ABI.Parameters[0].Location.ValueBytes = 4;
      if (Mutation == 3)
        ABI.Parameters.clear();
      if (Mutation == 4)
        ABI.Parameters.push_back(ABI.Parameters[0]);
      if (Mutation == 5)
        ABI.ReturnLocation.ValueBytes = 4;
      if (Mutation == 6)
        ABI.ReturnType = NdType::makeInt(8);
      if (Mutation == 7)
        ABI.Convention = SourceFunctionTypeHint::ConventionKind::C;
      if (Mutation == 8)
        ABI.ReturnLocation.RegisterOffset = TRI.IntParamRegs[1];
      if (Mutation == 9)
        ABI.Parameters[0].Type = NdType::makeInt(8);
      Call->SourceCallHint = Changed;
      EXPECT_FALSE(sdk::objcSourceCallBound(*Call, Image, {})) << Mutation;
    }
    Call->SourceCallHint = std::make_shared<SourceCallTypeHint>(*Hint);
    for (unsigned Mutation = 0; Mutation != 10; ++Mutation) {
      auto Wrong = Image;
      if (Mutation == 0)
        Wrong.DyldBindSlots[Slot].Module = "/tmp/libswift_Concurrency.dylib";
      if (Mutation == 1)
        Wrong.DyldBindSlots[Slot].Module = "/usr/lib/swift/libswiftCore.dylib";
      if (Mutation == 2)
        Wrong.DyldBindSlots[Slot].WeakImport = true;
      if (Mutation == 3)
        Wrong.DyldBindSlots[Slot].Addend = 8;
      if (Mutation == 4)
        Wrong.DyldBindSlots[Slot].Name += "invalid";
      if (Mutation == 5)
        Wrong.DyldBindSlots.erase(Slot);
      if (Mutation == 6)
        Wrong.ConflictingImportStorageSlots.insert(Slot);
      if (Mutation == 7)
        Wrong.ImportPtrSlots[Slot] += "invalid";
      if (Mutation == 8)
        Wrong.Format = BinaryFormat::COFF;
      if (Mutation == 9)
        Wrong.Bits = Bitness::Bits32;
      EXPECT_FALSE(swiftRuntimeSourceCallHint(Wrong, Slot)) << Mutation;
      EXPECT_FALSE(sdk::objcSourceCallBound(*Call, Wrong, {})) << Mutation;
    }
  }
}
