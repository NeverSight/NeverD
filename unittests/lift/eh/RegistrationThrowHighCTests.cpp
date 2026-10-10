//===- RegistrationThrowHighCTests.cpp - PE32 scalar throw values --------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Check source rendering of scalar x86 C++ exception objects.
//===----------------------------------------------------------------------===//
#include "gtest/gtest.h"

#include "neverd/backend/c/HighC/HighCEmitter.h"
#include "neverd/ir/high/MsvcTypeName.h"
#include "neverd/loader/BinaryImage.h"
#include "neverd/support/BinaryEncoding.h"

using namespace neverd;

TEST(RegistrationThrowHighC, ReadsTheCurrentScalarObjectWithItsExactType) {
  for (const auto &Type : msvc_type_name::FundamentalTypes)
    for (unsigned Mutation = 0; Mutation != 5; ++Mutation) {
      SCOPED_TRACE(Type.Encoding.str());
      SCOPED_TRACE(Mutation);
      BinaryImage Image;
      Image.Arch = Arch::X86;
      Image.Bits = Bitness::Bits32;
      Image.Format = BinaryFormat::COFF;
      Image.Base = 0x400000;
      Segment Table;
      Table.VA = 0x403000;
      Table.Flags = SegmentFlags::Readable;
      Table.Data.resize(128);
      Table.Size = Table.Data.size();
      auto Word = [&](size_t Offset, uint32_t Value) {
        writeLE<uint32_t>(Table.Data.data() + Offset, Value);
      };
      Word(12, Table.VA + 16);
      Word(16, 1);
      Word(20, Table.VA + 24);
      Word(24, 1);
      Word(28, Table.VA + 64);
      Word(36, UINT32_MAX);
      Word(44, Mutation == 1 ? Type.Size + 1 : Type.Size);
      std::copy(Type.Encoding.begin(), Type.Encoding.end(),
                Table.Data.begin() + 72);
      if (Mutation == 2)
        Table.Data[73] = '?';
      if (Mutation == 3)
        Table.Flags = SegmentFlags::Readable | SegmentFlags::Writable;
      Image.Segments.push_back(Table);
      HighFunc Function;
      Function.Name = "typed_throw";
      Function.Entry = 0x401000;
      Function.ReturnType = NdType::makeVoid();
      auto Pointer = NdType::makePtr();
      Pointer->Size = 4;
      Function.Params.push_back({"object", Pointer, kNoParamReg, 0});
      MedVar Object;
      Object.Kind = MedVar::Param;
      Object.Id = 0;
      Object.Size = 4;
      HighStmt Call;
      Call.Kind = StmtKind::Call;
      Call.CallExpr = HighExpr::makeCall("_CxxThrowException", 0x402000,
                                         {HighExpr::makeVar(Object, Pointer),
                                          HighExpr::makeConst(Table.VA, 4)});
      Function.Body.push_back(Call);
      CEmitterOptions Options;
      Options.Image = &Image;
      Options.TheArch = Arch::X86;
      Options.Format = BinaryFormat::COFF;
      Options.StructuredExceptionSyntax = Mutation != 4;
      std::string Text;
      llvm::raw_string_ostream OS(Text);
      ASSERT_TRUE(HighCEmitter().emit({Function}, OS, Options));
      EXPECT_EQ(Text.find("throw (" + Type.Spelling.str() + ")") !=
                    std::string::npos,
                Mutation == 0)
          << Text;
      EXPECT_EQ(Text.find("throw object"), std::string::npos) << Text;
      EXPECT_EQ(Text.find("throw " + Type.Spelling.str() + "()"),
                std::string::npos)
          << Text;
      if (Mutation == 0)
        EXPECT_TRUE(Text.find("memcpy") != std::string::npos ||
                    Text.find("*)(object)") != std::string::npos ||
                    Text.find("*)object") != std::string::npos)
            << Text;
      else
        EXPECT_NE(Text.find("CxxThrowException("), std::string::npos) << Text;
    }
}
