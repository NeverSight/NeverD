#include "../../../lib/sdk/capi/ObjCSourceBindings.h"
#include "RuntimeFunctionAddressFixture.h"
#include "gtest/gtest.h"

#include "neverd/ir/SourceABI.h"
#include "neverd/ir/TargetRegInfo.h"
#include "neverd/lift/AArch64/AArch64Regs.h"
#include "neverd/lift/X86/X86Regs.h"
#include "neverd/loader/Swift/SwiftRuntimeCalls.h"

using namespace neverd;

TEST(ObjCCallHints, SwiftNSCoderDecodeKeepsIndirectResultAndContextDistinct) {
  using namespace runtime_function_address_test;
  for (const auto Architecture : {Arch::AArch64, Arch::X64}) {
    for (const bool Any : {false, true}) {
      const std::string Name =
          Any ? "$sSo7NSCoderC10FoundationE12decodeObject2of6forKeyypSgSayyXlXp"
                "GSg_SStF"
              : "$sSo7NSCoderC10FoundationE12decodeObject2of6forKeyxSgxm_"
                "SStSo8NSObjectCRbzSo8NSCodingRzlF";
      SCOPED_TRACE(Name + ":" + std::to_string(static_cast<int>(Architecture)));
      const auto &TRI = getTargetRegInfo(Architecture);
      for (const char *Provider :
           {"/System/Library/Frameworks/Foundation.framework/Foundation",
            "/System/Library/Frameworks/Foundation.framework/Versions/C/"
            "Foundation",
            "/usr/lib/swift/libswiftFoundation.dylib"}) {
        auto Image = image(Architecture);
        Image.ImportPtrSlots[Slot] = "_" + Name;
        Image.DyldBindSlots[Slot] = {"_" + Name, 0, Provider, false};
        const auto Hint = swiftRuntimeSourceCallHint(Image, Slot);
        ASSERT_TRUE(Hint);
        const auto &Signature = Hint->Signature;
        EXPECT_EQ(Signature.Origin,
                  SourceFunctionTypeHint::OriginKind::SwiftSDK);
        EXPECT_EQ(Signature.Convention,
                  SourceFunctionTypeHint::ConventionKind::Swift);
        EXPECT_EQ(Signature.ReturnType->Kind,
                  Any ? NdTypeKind::Void : NdTypeKind::Int);
        if (!Any) {
          EXPECT_EQ(Signature.ReturnType->Size, 8U);
          EXPECT_EQ(Signature.ReturnLocation.RegisterOffset, TRI.IntReturnReg);
        }
        const unsigned StringIndex = Any ? 2 : 1;
        EXPECT_EQ(Hint->SwiftStringInputs,
                  (std::vector<std::pair<unsigned, unsigned>>{
                      {StringIndex, StringIndex + 1}}));
        ASSERT_EQ(Signature.Parameters.size(), 5U);
        unsigned Ordinary = 0;
        std::vector<ExprPtr> Arguments;
        for (unsigned I = 0; I != 5; ++I) {
          const auto &Parameter = Signature.Parameters[I];
          const bool Indirect = Any && I == 0;
          const bool Context = I == 4;
          const bool Word = Any ? (I == 1 || I == 2) : I == 1;
          EXPECT_EQ(Parameter.Type->Kind,
                    Word ? NdTypeKind::Int : NdTypeKind::Ptr);
          EXPECT_EQ(Parameter.Type->Size, 8U);
          EXPECT_EQ(Parameter.TheRole,
                    Context ? SourceParameterTypeHint::Role::SwiftContext
                    : Indirect
                        ? SourceParameterTypeHint::Role::SwiftIndirectResult
                        : SourceParameterTypeHint::Role::Ordinary);
          const auto Register =
              Context
                  ? (Architecture == Arch::AArch64 ? a64reg::X20 : x86reg::R13)
              : Indirect ? TRI.indirectResultReg()
                         : TRI.IntParamRegs[Ordinary++];
          EXPECT_EQ(Parameter.Location.RegisterOffset, Register);
          Arguments.push_back(HighExpr::makeConst(0, 8));
        }
        std::string Diagnostic;
        EXPECT_TRUE(validateSourceABI(Signature, Diagnostic)) << Diagnostic;
        auto Call = HighExpr::makeCall("untrusted", Slot, Arguments);
        Call->Type = Signature.ReturnType;
        Call->SourceCallHint = std::make_shared<SourceCallTypeHint>(*Hint);
        ASSERT_TRUE(sdk::objcSourceCallBound(*Call, Image, {}));
        EXPECT_FALSE(runtimeCFunctionAddressHint(Image, Slot));

        for (unsigned Mutation = 0; Mutation != 7; ++Mutation) {
          auto Changed = std::make_shared<SourceCallTypeHint>(*Hint);
          if (Mutation == 0)
            Changed->Signature.Parameters[4].TheRole =
                SourceParameterTypeHint::Role::Ordinary;
          if (Mutation == 1)
            Changed->Signature.Parameters[4].Location.RegisterOffset =
                TRI.IntParamRegs[3];
          if (Mutation == 2)
            Changed->Signature.Parameters[0].Location.RegisterOffset =
                TRI.IntParamRegs[2];
          if (Mutation == 3)
            Changed->Signature.Parameters[StringIndex].Type =
                NdType::makeInt(4, false);
          if (Mutation == 4)
            Changed->SwiftStringInputs.clear();
          if (Mutation == 5)
            Changed->Signature.Parameters.pop_back();
          if (Mutation == 6)
            Changed->Signature.ReturnType = NdType::makePtr(NdType::makeVoid());
          Call->SourceCallHint = Changed;
          EXPECT_FALSE(sdk::objcSourceCallBound(*Call, Image, {})) << Mutation;
        }
        Call->SourceCallHint = std::make_shared<SourceCallTypeHint>(*Hint);
        for (unsigned Mutation = 0; Mutation != 6; ++Mutation) {
          auto Wrong = Image;
          if (Mutation == 0)
            Wrong.DyldBindSlots[Slot].Module =
                "/tmp/Foundation.framework/Foundation";
          if (Mutation == 1)
            Wrong.DyldBindSlots[Slot].WeakImport = true;
          if (Mutation == 2)
            Wrong.DyldBindSlots[Slot].Addend = 8;
          if (Mutation == 3)
            Wrong.DyldBindSlots[Slot].Name += "invalid";
          if (Mutation == 4)
            Wrong.DyldBindSlots.erase(Slot);
          if (Mutation == 5)
            Wrong.ConflictingImportStorageSlots.insert(Slot);
          EXPECT_FALSE(swiftRuntimeSourceCallHint(Wrong, Slot)) << Mutation;
          EXPECT_FALSE(sdk::objcSourceCallBound(*Call, Wrong, {})) << Mutation;
        }
      }
    }
  }
}
