#include "../../../lib/sdk/capi/ObjCSourceBindings.h"
#include "RuntimeFunctionAddressFixture.h"
#include "gtest/gtest.h"

#include "neverd/ir/SourceABI.h"
#include "neverd/ir/TargetRegInfo.h"
#include "neverd/lift/AArch64/AArch64Regs.h"
#include "neverd/lift/X86/X86Regs.h"
#include "neverd/loader/Swift/SwiftRuntimeCalls.h"

using namespace neverd;

TEST(ObjCCallHints, SwiftSubjectInitializerKeepsValueAndInstanceContext) {
  using namespace runtime_function_address_test;
  const std::string Name = "$s7Combine19CurrentValueSubjectCyACyxq_Gxcfc";
  for (const auto Architecture : {Arch::AArch64, Arch::X64}) {
    for (const char *Provider :
         {"/System/Library/Frameworks/Combine.framework/Combine",
          "/System/Library/Frameworks/Combine.framework/Versions/A/Combine"}) {
      auto Image = image(Architecture);
      Image.ImportPtrSlots[Slot] = "_" + Name;
      Image.DyldBindSlots[Slot] = {"_" + Name, 0, Provider, false};
      const auto Hint = swiftRuntimeSourceCallHint(Image, Slot);
      ASSERT_TRUE(Hint);
      const auto &S = Hint->Signature;
      const auto &TRI = getTargetRegInfo(Architecture);
      EXPECT_EQ(S.Origin, SourceFunctionTypeHint::OriginKind::SwiftSDK);
      EXPECT_EQ(S.Convention, SourceFunctionTypeHint::ConventionKind::Swift);
      EXPECT_EQ(S.ReturnType->Kind, NdTypeKind::Ptr);
      EXPECT_EQ(S.ReturnType->Size, 8U);
      EXPECT_EQ(S.ReturnLocation.Kind, SourceABICarrierKind::IntegerRegister);
      EXPECT_EQ(S.ReturnLocation.RegisterOffset, TRI.IntReturnReg);
      EXPECT_EQ(S.ReturnLocation.ValueBytes, 8U);
      ASSERT_EQ(S.Parameters.size(), 2U);
      for (unsigned I = 0; I < 2; ++I) {
        EXPECT_EQ(S.Parameters[I].Type->Kind, NdTypeKind::Ptr);
        EXPECT_EQ(S.Parameters[I].Type->Size, 8U);
        EXPECT_EQ(S.Parameters[I].Location.ValueBytes, 8U);
        EXPECT_EQ(S.Parameters[I].TheRole,
                  I == 0 ? SourceParameterTypeHint::Role::Ordinary
                         : SourceParameterTypeHint::Role::SwiftContext);
      }
      EXPECT_EQ(S.Parameters[0].Location.RegisterOffset, TRI.IntParamRegs[0]);
      EXPECT_EQ(S.Parameters[1].Location.RegisterOffset,
                Architecture == Arch::AArch64 ? a64reg::X20 : x86reg::R13);
      std::string Error;
      EXPECT_TRUE(validateSourceABI(S, Error)) << Error;
      auto Call = HighExpr::makeCall(
          "untrusted", Slot,
          {HighExpr::makeConst(0, 8), HighExpr::makeConst(0, 8)});
      Call->Type = S.ReturnType;
      Call->SourceCallHint = std::make_shared<SourceCallTypeHint>(*Hint);
      ASSERT_TRUE(sdk::objcSourceCallBound(*Call, Image, {}));
      EXPECT_FALSE(runtimeCFunctionAddressHint(Image, Slot));
      for (unsigned Mutation = 0; Mutation != 8; ++Mutation) {
        auto Changed = std::make_shared<SourceCallTypeHint>(*Hint);
        auto &ABI = Changed->Signature;
        if (Mutation == 0)
          ABI.Parameters[1].TheRole = SourceParameterTypeHint::Role::Ordinary;
        if (Mutation == 1)
          ABI.Parameters[1].Location.RegisterOffset = TRI.IntParamRegs[1];
        if (Mutation == 2)
          ABI.Parameters[0].Location.ValueBytes = 4;
        if (Mutation == 3)
          ABI.Parameters.pop_back();
        if (Mutation == 4)
          ABI.Parameters.push_back(ABI.Parameters[0]);
        if (Mutation == 5)
          ABI.ReturnLocation.ValueBytes = 4;
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
          Wrong.DyldBindSlots[Slot].Module = "/tmp/Combine.framework/Combine";
        if (Mutation == 1)
          Wrong.DyldBindSlots[Slot].Module =
              "/System/Library/Frameworks/Combine.framework/Versions/B/Combine";
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

TEST(ObjCCallHints, SwiftPublishedInitializerKeepsOpaqueResultAndTypeMetadata) {
  using namespace runtime_function_address_test;
  const std::string Name = "$s7Combine9PublishedV12initialValueACyxGx_tcfC";
  for (const auto Architecture : {Arch::AArch64, Arch::X64}) {
    for (const char *Provider :
         {"/System/Library/Frameworks/Combine.framework/Combine",
          "/System/Library/Frameworks/Combine.framework/Versions/A/Combine"}) {
      auto Image = image(Architecture);
      Image.ImportPtrSlots[Slot] = "_" + Name;
      Image.DyldBindSlots[Slot] = {"_" + Name, 0, Provider, false};
      const auto Hint = swiftRuntimeSourceCallHint(Image, Slot);
      ASSERT_TRUE(Hint);
      const auto &Signature = Hint->Signature;
      const auto &TRI = getTargetRegInfo(Architecture);
      EXPECT_EQ(Signature.Origin, SourceFunctionTypeHint::OriginKind::SwiftSDK);
      EXPECT_EQ(Signature.Convention,
                SourceFunctionTypeHint::ConventionKind::Swift);
      EXPECT_EQ(Signature.ReturnType->Kind, NdTypeKind::Void);
      ASSERT_EQ(Signature.Parameters.size(), 3U);
      std::vector<ExprPtr> Arguments;
      for (unsigned I = 0; I < 3; ++I) {
        const auto &Parameter = Signature.Parameters[I];
        EXPECT_EQ(Parameter.Type->Kind, NdTypeKind::Ptr);
        EXPECT_EQ(Parameter.Type->Size, 8U);
        EXPECT_EQ(Parameter.TheRole,
                  I == 0 ? SourceParameterTypeHint::Role::SwiftIndirectResult
                         : SourceParameterTypeHint::Role::Ordinary);
        EXPECT_EQ(Parameter.Location.RegisterOffset,
                  I == 0 ? TRI.indirectResultReg() : TRI.IntParamRegs[I - 1]);
        Arguments.push_back(HighExpr::makeConst(0, 8));
      }
      std::string Diagnostic;
      EXPECT_TRUE(validateSourceABI(Signature, Diagnostic)) << Diagnostic;
      auto Call = HighExpr::makeCall("untrusted", Slot, Arguments);
      Call->Type = Signature.ReturnType;
      Call->SourceCallHint = std::make_shared<SourceCallTypeHint>(*Hint);
      ASSERT_TRUE(sdk::objcSourceCallBound(*Call, Image, {}));
      EXPECT_FALSE(runtimeCFunctionAddressHint(Image, Slot));
      for (unsigned Mutation = 0; Mutation < 5; ++Mutation) {
        auto Changed = std::make_shared<SourceCallTypeHint>(*Hint);
        if (Mutation == 0)
          Changed->Signature.Parameters[0].TheRole =
              SourceParameterTypeHint::Role::Ordinary;
        if (Mutation == 1)
          Changed->Signature.Parameters[0].Location.RegisterOffset =
              TRI.IntParamRegs[0];
        if (Mutation == 2)
          Changed->Signature.Parameters[2].TheRole =
              SourceParameterTypeHint::Role::SwiftContext;
        if (Mutation == 3)
          Changed->Signature.Parameters.pop_back();
        if (Mutation == 4)
          Changed->Signature.ReturnType = NdType::makePtr(NdType::makeVoid());
        Call->SourceCallHint = Changed;
        EXPECT_FALSE(sdk::objcSourceCallBound(*Call, Image, {})) << Mutation;
      }
      Call->SourceCallHint = std::make_shared<SourceCallTypeHint>(*Hint);
      for (unsigned Mutation = 0; Mutation < 6; ++Mutation) {
        auto Wrong = Image;
        if (Mutation == 0)
          Wrong.DyldBindSlots[Slot].Module = "/tmp/Combine.framework/Combine";
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

TEST(ObjCCallHints, SwiftPublishedAccessorsKeepOpaqueValueAndAllKeyPaths) {
  using namespace runtime_function_address_test;
  for (const bool Setter : {false, true}) {
    const std::string Name =
        "$s7Combine9PublishedV18_enclosingInstance7wrapped7storagexqd___s24"
        "ReferenceWritableKeyPathCyqd__xGAHyqd__ACyxGGtcRld__Clui" +
        std::string(Setter ? "sZ" : "gZ");
    for (const auto Architecture : {Arch::AArch64, Arch::X64}) {
      for (const char *Provider :
           {"/System/Library/Frameworks/Combine.framework/Combine",
            "/System/Library/Frameworks/Combine.framework/Versions/A/"
            "Combine"}) {
        SCOPED_TRACE(Name);
        auto Image = image(Architecture);
        Image.ImportPtrSlots[Slot] = "_" + Name;
        Image.DyldBindSlots[Slot] = {"_" + Name, 0, Provider, false};
        const auto Hint = swiftRuntimeSourceCallHint(Image, Slot);
        ASSERT_TRUE(Hint);
        const auto &S = Hint->Signature;
        const auto &TRI = getTargetRegInfo(Architecture);
        EXPECT_EQ(S.Origin, SourceFunctionTypeHint::OriginKind::SwiftSDK);
        EXPECT_EQ(S.Convention, SourceFunctionTypeHint::ConventionKind::Swift);
        EXPECT_EQ(S.ReturnType->Kind, NdTypeKind::Void);
        EXPECT_FALSE(Hint->DoesNotReturn);
        ASSERT_EQ(S.Parameters.size(), 4U);
        std::vector<ExprPtr> Args;
        for (unsigned I = 0; I != 4; ++I) {
          const auto &P = S.Parameters[I];
          const bool Result = !Setter && I == 0;
          EXPECT_EQ(P.Type->Kind, NdTypeKind::Ptr);
          EXPECT_EQ(P.Type->Size, 8U);
          EXPECT_EQ(P.Location.ValueBytes, 8U);
          EXPECT_EQ(P.TheRole,
                    Result ? SourceParameterTypeHint::Role::SwiftIndirectResult
                           : SourceParameterTypeHint::Role::Ordinary);
          EXPECT_EQ(P.Location.RegisterOffset,
                    Result ? TRI.indirectResultReg()
                           : TRI.IntParamRegs[I - !Setter]);
          Args.push_back(HighExpr::makeConst(0, 8));
        }
        std::string Error;
        EXPECT_TRUE(validateSourceABI(S, Error)) << Error;
        auto Call = HighExpr::makeCall("untrusted", Slot, Args);
        Call->Type = S.ReturnType;
        Call->SourceCallHint = std::make_shared<SourceCallTypeHint>(*Hint);
        ASSERT_TRUE(sdk::objcSourceCallBound(*Call, Image, {}));
        EXPECT_FALSE(runtimeCFunctionAddressHint(Image, Slot));
        for (unsigned Mutation = 0; Mutation != 9; ++Mutation) {
          auto Changed = std::make_shared<SourceCallTypeHint>(*Hint);
          auto &ABI = Changed->Signature;
          if (Mutation == 0)
            ABI.Parameters[0].TheRole =
                Setter ? SourceParameterTypeHint::Role::SwiftIndirectResult
                       : SourceParameterTypeHint::Role::Ordinary;
          if (Mutation == 1)
            ABI.Parameters[0].Location.RegisterOffset =
                Architecture == Arch::AArch64 ? a64reg::X20 : x86reg::R13;
          if (Mutation == 2)
            ABI.Parameters.pop_back();
          if (Mutation == 3)
            ABI.Parameters.push_back(ABI.Parameters.back());
          if (Mutation == 4)
            ABI.Parameters[1].TheRole =
                SourceParameterTypeHint::Role::SwiftContext;
          if (Mutation == 5)
            ABI.Parameters[2].Location.ValueBytes = 4;
          if (Mutation == 6)
            ABI.Parameters[0].Type = NdType::makeInt(8);
          if (Mutation == 7)
            ABI.Convention = SourceFunctionTypeHint::ConventionKind::C;
          if (Mutation == 8)
            ABI.ReturnType = NdType::makePtr(NdType::makeVoid());
          Call->SourceCallHint = Changed;
          EXPECT_FALSE(sdk::objcSourceCallBound(*Call, Image, {})) << Mutation;
        }
        Call->SourceCallHint = std::make_shared<SourceCallTypeHint>(*Hint);
        for (unsigned Mutation = 0; Mutation != 8; ++Mutation) {
          auto Wrong = Image;
          if (Mutation == 0)
            Wrong.DyldBindSlots[Slot].Module = "/tmp/Combine.framework/Combine";
          if (Mutation == 1)
            Wrong.DyldBindSlots[Slot].Module =
                "/System/Library/Frameworks/Combine.framework/Versions/B/"
                "Combine";
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
          if (Mutation == 7) {
            Wrong.ImportPtrSlots[Slot].back() = 'Y';
            Wrong.DyldBindSlots[Slot].Name = Wrong.ImportPtrSlots[Slot];
          }
          EXPECT_FALSE(swiftRuntimeSourceCallHint(Wrong, Slot)) << Mutation;
          EXPECT_FALSE(sdk::objcSourceCallBound(*Call, Wrong, {})) << Mutation;
        }
      }
    }
  }
}

TEST(ObjCCallHints, SwiftCancellableCallsKeepAllGenericAndContextCarriers) {
  using namespace runtime_function_address_test;
  for (const bool Sink : {false, true}) {
    const std::string Name =
        Sink ? "$s7Combine9PublisherPAAs5NeverO7FailureRtzrlE4sink12receiveValu"
               "eAA14AnyCancellableCy6OutputQzc_tF"
             : "$s7Combine14AnyCancellableC5store2inyShyACGz_tF";
    for (const auto Architecture : {Arch::AArch64, Arch::X64}) {
      for (const char *Provider :
           {"/System/Library/Frameworks/Combine.framework/Combine",
            "/System/Library/Frameworks/Combine.framework/Versions/A/"
            "Combine"}) {
        SCOPED_TRACE(Name);
        auto Image = image(Architecture);
        Image.ImportPtrSlots[Slot] = "_" + Name;
        Image.DyldBindSlots[Slot] = {"_" + Name, 0, Provider, false};
        const auto Hint = swiftRuntimeSourceCallHint(Image, Slot);
        ASSERT_TRUE(Hint);
        const auto &S = Hint->Signature;
        const auto &TRI = getTargetRegInfo(Architecture);
        const unsigned Ordinary = Sink ? 4 : 1;
        EXPECT_EQ(S.Origin, SourceFunctionTypeHint::OriginKind::SwiftSDK);
        EXPECT_EQ(S.Convention, SourceFunctionTypeHint::ConventionKind::Swift);
        EXPECT_EQ(S.ReturnType->Kind,
                  Sink ? NdTypeKind::Ptr : NdTypeKind::Void);
        EXPECT_FALSE(Hint->DoesNotReturn);
        ASSERT_EQ(S.Parameters.size(), Ordinary + 1);
        std::vector<ExprPtr> Args;
        for (unsigned I = 0; I <= Ordinary; ++I) {
          const auto &P = S.Parameters[I];
          EXPECT_EQ(P.Type->Kind, NdTypeKind::Ptr);
          EXPECT_EQ(P.Type->Size, 8U);
          EXPECT_EQ(P.Location.ValueBytes, 8U);
          EXPECT_EQ(P.TheRole, I == Ordinary
                                   ? SourceParameterTypeHint::Role::SwiftContext
                                   : SourceParameterTypeHint::Role::Ordinary);
          EXPECT_EQ(
              P.Location.RegisterOffset,
              I == Ordinary
                  ? (Architecture == Arch::AArch64 ? a64reg::X20 : x86reg::R13)
                  : TRI.IntParamRegs[I]);
          Args.push_back(HighExpr::makeConst(0, 8));
        }
        std::string Error;
        EXPECT_TRUE(validateSourceABI(S, Error)) << Error;
        auto Call = HighExpr::makeCall("untrusted", Slot, Args);
        Call->Type = S.ReturnType;
        Call->SourceCallHint = std::make_shared<SourceCallTypeHint>(*Hint);
        ASSERT_TRUE(sdk::objcSourceCallBound(*Call, Image, {}));
        EXPECT_FALSE(runtimeCFunctionAddressHint(Image, Slot));
        for (unsigned Mutation = 0; Mutation != 10; ++Mutation) {
          auto Changed = std::make_shared<SourceCallTypeHint>(*Hint);
          auto &ABI = Changed->Signature;
          if (Mutation == 0)
            ABI.Parameters.back().TheRole =
                SourceParameterTypeHint::Role::Ordinary;
          if (Mutation == 1)
            ABI.Parameters.back().Location.RegisterOffset =
                TRI.IntParamRegs[Ordinary];
          if (Mutation == 2)
            ABI.Parameters[0].Location.ValueBytes = 4;
          if (Mutation == 3)
            ABI.Parameters.pop_back();
          if (Mutation == 4)
            ABI.Parameters.push_back(ABI.Parameters[0]);
          if (Mutation == 5)
            ABI.ReturnType =
                Sink ? NdType::makeVoid() : NdType::makePtr(NdType::makeVoid());
          if (Mutation == 6)
            ABI.Parameters[0].Type = NdType::makeInt(8);
          if (Mutation == 7)
            ABI.Convention = SourceFunctionTypeHint::ConventionKind::C;
          if (Mutation == 8)
            ABI.Parameters[0].TheRole =
                SourceParameterTypeHint::Role::SwiftIndirectResult;
          if (Mutation == 9)
            ABI.Parameters[0].Location.RegisterOffset = TRI.IntParamRegs[1];
          Call->SourceCallHint = Changed;
          EXPECT_FALSE(sdk::objcSourceCallBound(*Call, Image, {})) << Mutation;
        }
        Call->SourceCallHint = std::make_shared<SourceCallTypeHint>(*Hint);
        for (unsigned Mutation = 0; Mutation != 10; ++Mutation) {
          auto Wrong = Image;
          if (Mutation == 0)
            Wrong.DyldBindSlots[Slot].Module = "/tmp/Combine.framework/Combine";
          if (Mutation == 1)
            Wrong.DyldBindSlots[Slot].Module =
                "/System/Library/Frameworks/Combine.framework/Versions/B/"
                "Combine";
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
          if (Mutation == 8)
            Wrong.Format = BinaryFormat::ELF;
          if (Mutation == 9)
            Wrong.Arch = Arch::ARM;
          EXPECT_FALSE(swiftRuntimeSourceCallHint(Wrong, Slot)) << Mutation;
          EXPECT_FALSE(sdk::objcSourceCallBound(*Call, Wrong, {})) << Mutation;
        }
      }
    }
  }
}
