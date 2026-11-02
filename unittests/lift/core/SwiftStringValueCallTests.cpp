#include "../../../lib/sdk/capi/ObjCSourceBindings.h"
#include "RuntimeFunctionAddressFixture.h"
#include "gtest/gtest.h"

#include "neverd/ir/SourceABI.h"
#include "neverd/ir/TargetRegInfo.h"
#include "neverd/lift/AArch64/AArch64Regs.h"
#include "neverd/lift/X86/X86Regs.h"
#include "neverd/loader/Swift/SwiftRuntimeCalls.h"
using namespace neverd;

TEST(ObjCCallHints, SwiftUTF8CStringKeepsBothStringWordsAndBufferResult) {
  using namespace runtime_function_address_test;
  const std::string Name = "$sSS11utf8CStrings15ContiguousArrayVys4Int8VGvg";
  for (const auto Architecture : {Arch::AArch64, Arch::X64}) {
    auto Image = image(Architecture);
    Image.ImportPtrSlots[Slot] = "_" + Name;
    Image.DyldBindSlots[Slot] = {"_" + Name, 0,
                                 "/usr/lib/swift/libswiftCore.dylib", false};
    const auto Hint = swiftRuntimeSourceCallHint(Image, Slot);
    ASSERT_TRUE(Hint);
    const auto &Signature = Hint->Signature;
    const auto &TRI = getTargetRegInfo(Architecture);
    EXPECT_EQ(Signature.Convention,
              SourceFunctionTypeHint::ConventionKind::Swift);
    ASSERT_EQ(Signature.Parameters.size(), 2U);
    EXPECT_EQ(Signature.Parameters[0].Type->Kind, NdTypeKind::Int);
    EXPECT_EQ(Signature.Parameters[1].Type->Kind, NdTypeKind::Ptr);
    for (unsigned I = 0; I < 2; ++I) {
      EXPECT_EQ(Signature.Parameters[I].TheRole,
                SourceParameterTypeHint::Role::Ordinary);
      EXPECT_EQ(Signature.Parameters[I].Type->Size, 8U);
      EXPECT_EQ(Signature.Parameters[I].Location.RegisterOffset,
                TRI.IntParamRegs[I]);
    }
    EXPECT_EQ(Signature.ReturnType->Kind, NdTypeKind::Ptr);
    EXPECT_EQ(Signature.ReturnType->Size, 8U);
    EXPECT_EQ(Signature.ReturnLocation.RegisterOffset, TRI.IntReturnReg);
    EXPECT_TRUE(Signature.ReturnComponents.empty());
    auto Call =
        HighExpr::makeCall("untrusted", Slot,
                           {HighExpr::makeConst(0, 8),
                            HighExpr::makeConst(0xe000000000000000, 8)});
    Call->Type = Signature.ReturnType;
    Call->SourceCallHint = std::make_shared<SourceCallTypeHint>(*Hint);
    EXPECT_TRUE(sdk::objcSourceCallBound(*Call, Image, {}));
    for (unsigned Mutation = 0; Mutation < 6; ++Mutation) {
      SCOPED_TRACE(Mutation);
      auto Wrong = Image;
      if (Mutation == 0)
        Wrong.DyldBindSlots[Slot].Module = "/tmp/libswiftCore.dylib";
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
      EXPECT_FALSE(swiftRuntimeSourceCallHint(Wrong, Slot));
      EXPECT_FALSE(sdk::objcSourceCallBound(*Call, Wrong, {}));
    }
    auto Changed = *Hint;
    Changed.Signature.Parameters[0].Type = NdType::makeInt(4);
    Call->SourceCallHint = std::make_shared<SourceCallTypeHint>(Changed);
    EXPECT_FALSE(sdk::objcSourceCallBound(*Call, Image, {}));
  }
}

TEST(ObjCCallHints, SwiftStringValueOperationsKeepContextAndResultCarriers) {
  using namespace runtime_function_address_test;
  for (const auto Architecture : {Arch::AArch64, Arch::X64})
    for (bool Reserve : {false, true}) {
      const std::string Name = Reserve
                                   ? "$sSS15reserveCapacityyySiF"
                                   : "$s10Foundation3URLV14absoluteStringSSvg";
      const std::vector<std::string> Providers =
          Reserve
              ? std::vector<std::string>{"/usr/lib/swift/libswiftCore.dylib"}
              : std::vector<std::string>{
                    "/System/Library/Frameworks/Foundation.framework/"
                    "Foundation",
                    "/System/Library/Frameworks/Foundation.framework/Versions/"
                    "C/Foundation",
                    "/usr/lib/swift/libswiftFoundation.dylib"};
      for (const auto &Provider : Providers) {
        auto Image = image(Architecture);
        Image.ImportPtrSlots[Slot] = "_" + Name;
        Image.DyldBindSlots[Slot] = {"_" + Name, 0, Provider, false};
        const auto Hint = swiftRuntimeSourceCallHint(Image, Slot);
        ASSERT_TRUE(Hint);
        const auto &Signature = Hint->Signature;
        const auto &TRI = getTargetRegInfo(Architecture);
        EXPECT_EQ(Signature.Origin,
                  SourceFunctionTypeHint::OriginKind::SwiftSDK);
        EXPECT_EQ(Signature.Convention,
                  SourceFunctionTypeHint::ConventionKind::Swift);
        EXPECT_EQ(Signature.ReturnType->Kind,
                  Reserve ? NdTypeKind::Void : NdTypeKind::Int);
        if (!Reserve) {
          EXPECT_EQ(Signature.ReturnType->Size, 16U);
          EXPECT_EQ(Signature.ReturnLocation.Kind, SourceABICarrierKind::None);
          ASSERT_EQ(Signature.ReturnComponents.size(), 2U);
          EXPECT_EQ(Signature.ReturnComponents[0].RegisterOffset,
                    TRI.IntReturnReg);
          EXPECT_EQ(Signature.ReturnComponents[1].RegisterOffset,
                    Architecture == Arch::AArch64 ? a64reg::X1 : x86reg::RDX);
        }
        ASSERT_EQ(Signature.Parameters.size(), Reserve ? 2U : 1U);
        const auto &Context = Signature.Parameters.back();
        EXPECT_EQ(Context.TheRole, SourceParameterTypeHint::Role::SwiftContext);
        EXPECT_EQ(Context.Type->Kind, NdTypeKind::Ptr);
        EXPECT_EQ(Context.Type->Size, 8U);
        EXPECT_EQ(Context.Location.RegisterOffset,
                  Architecture == Arch::AArch64 ? a64reg::X20 : x86reg::R13);
        if (Reserve) {
          EXPECT_EQ(Signature.Parameters[0].Type->Kind, NdTypeKind::Int);
          EXPECT_EQ(Signature.Parameters[0].Type->Size, 8U);
          EXPECT_EQ(Signature.Parameters[0].TheRole,
                    SourceParameterTypeHint::Role::Ordinary);
          EXPECT_EQ(Signature.Parameters[0].Location.RegisterOffset,
                    TRI.IntParamRegs[0]);
        }
        std::string Diagnostic;
        EXPECT_TRUE(validateSourceABI(Signature, Diagnostic)) << Diagnostic;
        auto Call =
            HighExpr::makeCall("untrusted", Slot,
                               std::vector<ExprPtr>(Signature.Parameters.size(),
                                                    HighExpr::makeConst(0, 8)));
        Call->Type = Signature.ReturnType;
        Call->SourceCallHint = std::make_shared<SourceCallTypeHint>(*Hint);
        ASSERT_TRUE(sdk::objcSourceCallBound(*Call, Image, {}));
        EXPECT_FALSE(runtimeCFunctionAddressHint(Image, Slot));
        for (unsigned Mutation = 0; Mutation < 5; ++Mutation) {
          auto Changed = std::make_shared<SourceCallTypeHint>(*Hint);
          if (Mutation == 0)
            Changed->Signature.Parameters.back().TheRole =
                SourceParameterTypeHint::Role::Ordinary;
          if (Mutation == 1)
            Changed->Signature.Parameters.back().Location.RegisterOffset =
                TRI.IntParamRegs[0];
          if (Mutation == 2)
            Changed->Signature.ReturnType = NdType::makePtr(NdType::makeVoid());
          if (Mutation == 3)
            Changed->Signature.Parameters.pop_back();
          if (Mutation == 4)
            Changed->SwiftStringInputs = {{0, 1}};
          Call->SourceCallHint = Changed;
          EXPECT_FALSE(sdk::objcSourceCallBound(*Call, Image, {})) << Mutation;
        }
        Call->SourceCallHint = std::make_shared<SourceCallTypeHint>(*Hint);
        for (unsigned Mutation = 0; Mutation < 6; ++Mutation) {
          auto Wrong = Image;
          if (Mutation == 0)
            Wrong.DyldBindSlots[Slot].Module = "/tmp/Foundation";
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

TEST(ObjCCallHints, SwiftCStringConstructionKeepsCompleteSDKDeclarations) {
  using namespace runtime_function_address_test;
  for (const auto Architecture : {Arch::AArch64, Arch::X64})
    for (bool Repair : {false, true}) {
      const std::string Name =
          Repair ? "$sSS18_fromUTF8RepairingySS6result_"
                   "Sb11repairsMadetSRys5UInt8VGFZ"
                 : "$sSa28_allocateBufferUninitialized15minimumCapacitys06_"
                   "ArrayB0VyxGSi_tFZ";
      auto Image = image(Architecture);
      Image.ImportPtrSlots[Slot] = "_" + Name;
      Image.DyldBindSlots[Slot] = {"_" + Name, 0,
                                   "/usr/lib/swift/libswiftCore.dylib", false};
      const auto Hint = swiftRuntimeSourceCallHint(Image, Slot);
      ASSERT_TRUE(Hint) << Name;
      const auto &Signature = Hint->Signature;
      const auto &TRI = getTargetRegInfo(Architecture);
      EXPECT_EQ(Signature.Origin, SourceFunctionTypeHint::OriginKind::SwiftSDK);
      EXPECT_EQ(Signature.Convention,
                SourceFunctionTypeHint::ConventionKind::Swift);
      ASSERT_EQ(Signature.Parameters.size(), 2U);
      for (unsigned I = 0; I != 2; ++I) {
        EXPECT_EQ(Signature.Parameters[I].TheRole,
                  SourceParameterTypeHint::Role::Ordinary);
        EXPECT_EQ(Signature.Parameters[I].Type->Kind,
                  Repair || I == 0 ? NdTypeKind::Int : NdTypeKind::Ptr);
        EXPECT_EQ(Signature.Parameters[I].Type->Size, 8U);
        EXPECT_EQ(Signature.Parameters[I].Location.RegisterOffset,
                  TRI.IntParamRegs[I]);
      }
      if (Repair) {
        EXPECT_EQ(Signature.ReturnType->Kind, NdTypeKind::Struct);
        const auto Members = sourceAggregateMembers(Signature.ReturnType);
        ASSERT_EQ(Members.size(), 3U);
        EXPECT_TRUE(isSourceBooleanType(Members[2].Type));
        ASSERT_EQ(Signature.ReturnComponents.size(), 3U);
        EXPECT_EQ(Signature.ReturnComponents[0].RegisterOffset,
                  TRI.IntReturnRegs[0]);
        EXPECT_EQ(Signature.ReturnComponents[1].RegisterOffset,
                  TRI.IntReturnRegs[1]);
        EXPECT_EQ(Signature.ReturnComponents[2].Kind,
                  SourceABICarrierKind::BooleanRegister);
        EXPECT_EQ(Signature.ReturnComponents[2].RegisterOffset,
                  Architecture == Arch::AArch64 ? a64reg::X2 : x86reg::RCX);
        EXPECT_EQ(Signature.ReturnComponents[2].ValueBytes, 1U);
      } else {
        EXPECT_EQ(Signature.ReturnType->Kind, NdTypeKind::Ptr);
        EXPECT_EQ(Signature.ReturnLocation.RegisterOffset, TRI.IntReturnReg);
        EXPECT_TRUE(Signature.ReturnComponents.empty());
      }
      auto Call = HighExpr::makeCall(
          "untrusted", Slot,
          {HighExpr::makeConst(0, 8), HighExpr::makeConst(0, 8)});
      Call->Type = Signature.ReturnType;
      Call->SourceCallHint = std::make_shared<SourceCallTypeHint>(*Hint);
      ASSERT_TRUE(sdk::objcSourceCallBound(*Call, Image, {}));
      EXPECT_FALSE(runtimeCFunctionAddressHint(Image, Slot));
      for (unsigned Mutation = 0; Mutation != 6; ++Mutation) {
        auto Wrong = Image;
        if (Mutation == 0)
          Wrong.DyldBindSlots[Slot].Module = "/tmp/libswiftCore.dylib";
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
      for (unsigned Mutation = 0; Mutation != 5; ++Mutation) {
        auto Changed = std::make_shared<SourceCallTypeHint>(*Hint);
        auto &S = Changed->Signature;
        if (Mutation == 0)
          S.Parameters.back().TheRole =
              SourceParameterTypeHint::Role::SwiftContext;
        if (Mutation == 1)
          S.Parameters.front().Type = NdType::makeInt(4, false);
        if (Mutation == 2) {
          S.ReturnType = NdType::makeInt(16, false);
          // A valid two-word ABI still omits part of the actual SDK result.
          std::string Error;
          ASSERT_TRUE(assignDarwinSwiftSourceABI(S, Architecture, Error));
        }
        if (Mutation == 3 && Repair)
          S.ReturnComponents[2].Kind = SourceABICarrierKind::IntegerRegister;
        if (Mutation == 3 && !Repair)
          S.ReturnLocation.ValueBytes = 4;
        if (Mutation == 4)
          Changed->CanonicalBooleanInputs = {1};
        Call->SourceCallHint = Changed;
        EXPECT_FALSE(sdk::objcSourceCallBound(*Call, Image, {})) << Mutation;
      }
    }
}

TEST(ObjCCallHints, SwiftComponentsAndSetAllocationKeepEveryGenericCarrier) {
  using namespace runtime_function_address_test;
  for (const auto Architecture : {Arch::AArch64, Arch::X64})
    for (bool Allocate : {false, true}) {
      const std::string Name =
          Allocate ? "$ss11_SetStorageC8allocate8capacityAByxGSi_tFZ"
                   : "$sSy10FoundationE10components11separatedBySaySSGqd___"
                     "tSyRd__lF";
      const std::vector<std::string> Providers =
          Allocate
              ? std::vector<std::string>{"/usr/lib/swift/libswiftCore.dylib"}
              : std::vector<std::string>{
                    "/System/Library/Frameworks/Foundation.framework/"
                    "Foundation",
                    "/System/Library/Frameworks/Foundation.framework/Versions/"
                    "C/Foundation",
                    "/usr/lib/swift/libswiftFoundation.dylib"};
      for (const auto &Provider : Providers) {
        auto Image = image(Architecture);
        Image.ImportPtrSlots[Slot] = "_" + Name;
        Image.DyldBindSlots[Slot] = {"_" + Name, 0, Provider, false};
        const auto Hint = swiftRuntimeSourceCallHint(Image, Slot);
        ASSERT_TRUE(Hint);
        const auto &Signature = Hint->Signature;
        const auto &TRI = getTargetRegInfo(Architecture);
        ASSERT_EQ(Signature.Parameters.size(), Allocate ? 2U : 6U);
        EXPECT_EQ(Signature.Convention,
                  SourceFunctionTypeHint::ConventionKind::Swift);
        EXPECT_EQ(Signature.ReturnType->Kind, NdTypeKind::Ptr);
        EXPECT_EQ(Signature.ReturnType->Size, 8U);
        EXPECT_TRUE(Signature.ReturnComponents.empty());
        EXPECT_EQ(Signature.ReturnLocation.RegisterOffset, TRI.IntReturnReg);
        for (size_t I = 0; I + 1 < Signature.Parameters.size(); ++I) {
          const auto &P = Signature.Parameters[I];
          EXPECT_EQ(P.Type->Kind, Allocate ? NdTypeKind::Int : NdTypeKind::Ptr);
          EXPECT_EQ(P.Type->Size, 8U);
          EXPECT_EQ(P.TheRole, SourceParameterTypeHint::Role::Ordinary);
          EXPECT_EQ(P.Location.RegisterOffset, TRI.IntParamRegs[I]);
        }
        const auto &Context = Signature.Parameters.back();
        EXPECT_EQ(Context.Type->Kind, NdTypeKind::Ptr);
        EXPECT_EQ(Context.TheRole, SourceParameterTypeHint::Role::SwiftContext);
        EXPECT_EQ(Context.Location.RegisterOffset,
                  Architecture == Arch::AArch64 ? a64reg::X20 : x86reg::R13);
        auto Call =
            HighExpr::makeCall("untrusted", Slot,
                               std::vector<ExprPtr>(Signature.Parameters.size(),
                                                    HighExpr::makeConst(0, 8)));
        Call->Type = Signature.ReturnType;
        Call->SourceCallHint = std::make_shared<SourceCallTypeHint>(*Hint);
        ASSERT_TRUE(sdk::objcSourceCallBound(*Call, Image, {}));
        EXPECT_FALSE(runtimeCFunctionAddressHint(Image, Slot));
        for (unsigned Mutation = 0; Mutation < 4; ++Mutation) {
          auto Changed = *Hint;
          if (Mutation == 0)
            Changed.Signature.Parameters.back().TheRole =
                SourceParameterTypeHint::Role::Ordinary;
          if (Mutation == 1)
            Changed.Signature.Parameters.erase(
                Changed.Signature.Parameters.begin());
          if (Mutation == 2)
            Changed.Signature.ReturnType = NdType::makeInt(8, false);
          if (Mutation == 3)
            Changed.Signature.Parameters.front().Type =
                NdType::makeInt(4, false);
          std::string Error;
          if (assignDarwinSwiftSourceABI(Changed.Signature, Architecture,
                                         Error)) {
            Call->SourceCallHint =
                std::make_shared<SourceCallTypeHint>(Changed);
            EXPECT_FALSE(sdk::objcSourceCallBound(*Call, Image, {}))
                << Mutation;
          }
        }
        Call->SourceCallHint = std::make_shared<SourceCallTypeHint>(*Hint);
        for (unsigned Mutation = 0; Mutation < 6; ++Mutation) {
          auto Wrong = Image;
          if (Mutation == 0)
            Wrong.DyldBindSlots[Slot].Module = "/tmp/Foundation";
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
          EXPECT_FALSE(swiftRuntimeSourceCallHint(Wrong, Slot));
          EXPECT_FALSE(sdk::objcSourceCallBound(*Call, Wrong, {}));
        }
      }
    }
}
