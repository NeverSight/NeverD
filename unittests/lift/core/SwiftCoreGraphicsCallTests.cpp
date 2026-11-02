#include "../../../lib/sdk/capi/ObjCSourceBindings.h"
#include "RuntimeFunctionAddressFixture.h"
#include "gtest/gtest.h"

#include "neverd/ir/SourceABI.h"
#include "neverd/ir/TargetRegInfo.h"
#include "neverd/lift/AArch64/AArch64Regs.h"
#include "neverd/lift/X86/X86Regs.h"
#include "neverd/loader/Swift/SwiftRuntimeCalls.h"

using namespace neverd;

TEST(ObjCCallHints, CoreGraphicsPointActionsKeepSwiftFloatingCarriers) {
  using namespace runtime_function_address_test;
  for (const auto Architecture : {Arch::AArch64, Arch::X64}) {
    SCOPED_TRACE(static_cast<unsigned>(Architecture));
    for (const char *Name :
         {"$sSo12CGContextRefa12CoreGraphicsE4move2toySo7CGPointV_tF",
          "$sSo12CGContextRefa12CoreGraphicsE7addLine2toySo7CGPointV_tF"}) {
      for (const char *Provider :
           {"/System/Library/Frameworks/CoreGraphics.framework/CoreGraphics",
            "/System/Library/Frameworks/CoreGraphics.framework/Versions/A/"
            "CoreGraphics",
            "/usr/lib/swift/libswiftCoreGraphics.dylib"}) {
        SCOPED_TRACE(Provider);
        auto Image = image(Architecture);
        const std::string Import = std::string("_") + Name;
        Image.ImportPtrSlots[Slot] = Import;
        Image.DyldBindSlots[Slot] = {Import, 0, Provider, false};
        const auto Hint = swiftRuntimeSourceCallHint(Image, Slot);
        ASSERT_TRUE(Hint) << Name;
        const auto &S = Hint->Signature;
        const auto &TRI = getTargetRegInfo(Architecture);
        EXPECT_EQ(S.Origin, SourceFunctionTypeHint::OriginKind::SwiftSDK);
        EXPECT_EQ(S.Convention, SourceFunctionTypeHint::ConventionKind::Swift);
        ASSERT_TRUE(S.ReturnType);
        EXPECT_EQ(S.ReturnType->Kind, NdTypeKind::Void);
        EXPECT_FALSE(Hint->DoesNotReturn);
        EXPECT_TRUE(S.ReturnComponents.empty());
        ASSERT_EQ(S.Parameters.size(), 3U);
        for (unsigned I = 0; I != 2; ++I) {
          const auto &P = S.Parameters[I];
          EXPECT_EQ(P.Type->Kind, NdTypeKind::Float);
          EXPECT_EQ(P.Type->Size, 8U);
          EXPECT_EQ(P.TheRole, SourceParameterTypeHint::Role::Ordinary);
          EXPECT_EQ(P.Location.Kind, SourceABICarrierKind::FloatingRegister);
          EXPECT_EQ(P.Location.RegisterOffset, TRI.FPParamRegs[I]);
          EXPECT_EQ(P.Location.ValueBytes, 8U);
        }
        const auto &Context = S.Parameters.back();
        EXPECT_EQ(Context.Type->Kind, NdTypeKind::Ptr);
        EXPECT_EQ(Context.Type->Size, 8U);
        EXPECT_EQ(Context.TheRole, SourceParameterTypeHint::Role::SwiftContext);
        EXPECT_EQ(Context.Location.RegisterOffset,
                  Architecture == Arch::AArch64 ? a64reg::X20 : x86reg::R13);
        std::string Error;
        EXPECT_TRUE(validateSourceABI(S, Error)) << Error;
        auto Call = HighExpr::makeCall("untrusted", Slot,
                                       {HighExpr::makeConst(0, 8),
                                        HighExpr::makeConst(0, 8),
                                        HighExpr::makeConst(0, 8)});
        Call->Type = S.ReturnType;
        Call->SourceCallHint = std::make_shared<SourceCallTypeHint>(*Hint);
        ASSERT_TRUE(sdk::objcSourceCallBound(*Call, Image, {}));
        EXPECT_FALSE(runtimeCFunctionAddressHint(Image, Slot));
        for (unsigned Mutation = 0; Mutation != 8; ++Mutation) {
          auto Changed = std::make_shared<SourceCallTypeHint>(*Hint);
          auto &ABI = Changed->Signature;
          if (Mutation == 0)
            ABI.Parameters[0].Type = NdType::makeInt(8);
          if (Mutation == 1)
            ABI.Parameters[1].Location.RegisterOffset = TRI.FPParamRegs[0];
          if (Mutation == 2)
            ABI.Parameters[0].Location.ValueBytes = 4;
          if (Mutation == 3)
            ABI.Parameters[2].TheRole = SourceParameterTypeHint::Role::Ordinary;
          if (Mutation == 4)
            ABI.Parameters[2].Location.RegisterOffset = TRI.IntParamRegs[0];
          if (Mutation == 5)
            ABI.Parameters.pop_back();
          if (Mutation == 6)
            ABI.ReturnType = NdType::makeInt(8);
          if (Mutation == 7)
            ABI.Convention = SourceFunctionTypeHint::ConventionKind::C;
          Call->SourceCallHint = Changed;
          EXPECT_FALSE(sdk::objcSourceCallBound(*Call, Image, {})) << Mutation;
        }
        Call->SourceCallHint = std::make_shared<SourceCallTypeHint>(*Hint);
        for (unsigned Mutation = 0; Mutation != 8; ++Mutation) {
          auto Wrong = Image;
          if (Mutation == 0)
            Wrong.DyldBindSlots[Slot].Module =
                "/tmp/libswiftCoreGraphics.dylib";
          if (Mutation == 1)
            Wrong.DyldBindSlots[Slot].Module =
                "/System/Library/Frameworks/CoreGraphics.framework/Versions/B/"
                "CoreGraphics";
          if (Mutation == 2)
            Wrong.DyldBindSlots[Slot].Module =
                "/System/Library/Frameworks/Foundation.framework/Foundation";
          if (Mutation == 3)
            Wrong.DyldBindSlots[Slot].WeakImport = true;
          if (Mutation == 4)
            Wrong.DyldBindSlots[Slot].Addend = 8;
          if (Mutation == 5)
            Wrong.DyldBindSlots[Slot].Name += "invalid";
          if (Mutation == 6)
            Wrong.DyldBindSlots.erase(Slot);
          if (Mutation == 7)
            Wrong.ConflictingImportStorageSlots.insert(Slot);
          EXPECT_FALSE(swiftRuntimeSourceCallHint(Wrong, Slot)) << Mutation;
          EXPECT_FALSE(sdk::objcSourceCallBound(*Call, Wrong, {})) << Mutation;
        }
      }
    }
  }
}
