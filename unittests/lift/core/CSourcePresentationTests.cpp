//===- CSourcePresentationTests.cpp - Source text and variable metadata ---===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Source presentation retains storage widths and decodes image text.
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "neverd/backend/LLVMOriginalImageStorage.h"
#include "neverd/backend/RewriteSourceIdentity.h"
#include "neverd/backend/c/CSourceMap.h"
#include "neverd/backend/c/HighC/HighCEmitter.h"
#include "neverd/backend/c/LLVMC/LLVMCEmitter.h"
#include "neverd/backend/c/render/CTypeFormat.h"
#include "neverd/backend/llvm/SourceFrameBytes.h"
#include "neverd/ir/Limits.h"
#include "neverd/ir/high/HighIR.h"
#include "neverd/loader/BinaryImage.h"

#include "llvm/AsmParser/Parser.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/Support/SourceMgr.h"

using namespace neverd;

namespace {

TEST(CSourcePresentation, KnownNarrowStringCallsDoNotRequireAWideLiteral) {
  for (Arch Architecture : {Arch::AArch64, Arch::X64}) {
    BinaryImage Image;
    Image.Arch = Architecture;
    Image.Format = BinaryFormat::ELF;
    Image.Bits = Bitness::Bits64;
    Segment Data;
    Data.VA = 0x2000;
    // A one-character C string followed immediately by another string.
    Data.Data = {'C', 0, 'n', 'e', 'x', 't', 0};
    Data.Size = Data.FileSz = Data.Data.size();
    Data.Flags = SegmentFlags::Readable;
    Image.Segments.push_back(std::move(Data));
    HighFunc Function;
    Function.Entry = 0x1000;
    Function.Name = "print_message";
    Function.ReturnType = NdType::makeVoid();
    HighStmt Call;
    Call.Kind = StmtKind::Call;
    Call.CallExpr =
        HighExpr::makeCall("puts", 0x3000, {HighExpr::makeConst(0x2000, 8)});
    Function.Body.push_back(std::move(Call));
    llvm::LLVMContext Context;
    llvm::SMDiagnostic Diagnostic;
    auto Module = llvm::parseAssemblyString(R"(
      target datalayout = "e-p:64:64"
      declare i32 @puts(ptr)
      define void @print_message() {
        call i32 @puts(ptr inttoptr (i64 8192 to ptr))
        ret void
      }
    )",
                                            Diagnostic, Context);
    ASSERT_TRUE(Module);
    for (bool LLVM : {false, true}) {
      SCOPED_TRACE(LLVM);
      CEmitterOptions Options;
      Options.TheArch = Architecture;
      Options.Format = Image.Format;
      Options.Image = &Image;
      std::string Text;
      llvm::raw_string_ostream Out(Text);
      ASSERT_TRUE(
          LLVM ? LLVMCEmitter().emit(*Module, Out, Options, nullptr, &Image)
               : HighCEmitter().emit({Function}, Out, Options));
      EXPECT_NE(Text.find("puts(\"C\")"), std::string::npos) << Text;
    }
    // Without a narrow consumer, a non-ASCII wide string must not become
    // its one-byte prefix. The typed call above supplies the missing width.
    EXPECT_FALSE(imageStringLiteral(&Image, 0x2000));
  }
}

TEST(CSourcePresentation, LiteralMessagesUseTheFullStringReadWindow) {
  for (unsigned Length : {66u, 4095u, 4096u}) {
    BinaryImage Image;
    Image.Arch = Arch::AArch64;
    Image.Format = BinaryFormat::ELF;
    Image.Bits = Bitness::Bits64;
    Segment Data;
    Data.VA = 0x2000;
    Data.Data.assign(Length, 'x');
    Data.Data.push_back(0);
    Data.Size = Data.FileSz = Data.Data.size();
    Data.Flags = SegmentFlags::Readable;
    Image.Segments.push_back(std::move(Data));
    const auto Literal = imageStringLiteral(&Image, 0x2000);
    if (Length < 4096)
      EXPECT_EQ(Literal, std::optional<std::string>(
                             "\"" + std::string(Length, 'x') + "\""));
    else
      EXPECT_FALSE(Literal);
    // Expanding presentation length never makes fixup storage into text.
    Image.DataPtrRelocSlots.insert(0x2000 + Length / 2);
    EXPECT_FALSE(imageStringLiteral(&Image, 0x2000));
  }
}

TEST(CSourcePresentation, AggregateStorageIsNotAnIntegerVariable) {
  for (llvm::StringRef Storage : {"[248 x i8]", "[16 x i8]", "{ i64, i64 }"}) {
    llvm::LLVMContext Context;
    llvm::SMDiagnostic Diagnostic;
    const std::string IR =
        "target datalayout = \"e-p:64:64-i64:64-n8:16:32:64-S128\"\n"
        "declare i64 @consume(ptr)\n"
        "define i64 @sample() {\n"
        "  %buffer = alloca " +
        Storage.str() +
        ", align 16\n"
        "  store i8 65, ptr %buffer\n"
        "  %value = call i64 @consume(ptr %buffer)\n"
        "  ret i64 %value\n}\n";
    auto Module = llvm::parseAssemblyString(IR, Diagnostic, Context);
    ASSERT_TRUE(Module) << Diagnostic.getMessage().str();
    rewrite_source::setOriginalVA(*Module->getFunction("sample"), 0x1000);
    CSourceMap Map;
    CEmitterOptions Options;
    Options.SourceMap = &Map;
    Options.TheArch = Arch::X64;
    std::string Text;
    llvm::raw_string_ostream Stream(Text);
    ASSERT_TRUE(LLVMCEmitter().emit(*Module, Stream, Options));
    bool Found = false;
    for (const auto &[Key, Variable] : Map.Variables) {
      if (Variable.Bytes < 16)
        continue;
      Found = true;
      EXPECT_TRUE(Variable.Ambiguous);
      // Source-variable editing accepts scalar storage only. An allocation
      // keeps its byte extent without inventing an integer of that extent.
      EXPECT_FALSE(
          SourceVariableTypes::compatible(Variable.Type, Variable.Bytes));
      EXPECT_EQ(Variable.Bytes, Storage == "[248 x i8]" ? 248u : 16u);
    }
    EXPECT_TRUE(Found) << Text;
    for (const auto &[Key, Variable] : Map.Variables)
      if (SourceVariableTypes::compatible(Variable.Type, Variable.Bytes))
        EXPECT_NO_THROW((void)declarationToC(Variable.Type, ""));
  }
}

TEST(CSourcePresentation, GlobalTypesFollowLoadsFromSharedImageStorage) {
  for (const auto Format :
       {BinaryFormat::ELF, BinaryFormat::COFF, BinaryFormat::MachO})
    for (const bool Volatile : {false, true}) {
      SCOPED_TRACE(unsigned(Format));
      SCOPED_TRACE(Volatile);
      llvm::LLVMContext Context;
      llvm::SMDiagnostic Diagnostic;
      const std::string IR =
          "target datalayout = \"e-p:64:64-i64:64-n8:16:32:64-S128\"\n"
          "@__nd_data_2000 = external global [32 x i8]\n"
          "define i32 @read_field() {\n"
          "  %slot = getelementptr i8, ptr @__nd_data_2000, i64 8\n"
          "  %value = load " +
          std::string(Volatile ? "volatile " : "") +
          "i64, ptr %slot, align 1\n"
          "  %field = add i64 %value, 4\n"
          "  %pointer = inttoptr i64 %field to ptr\n"
          "  %result = load i32, ptr %pointer, align 1\n"
          "  ret i32 %result\n}\n";
      auto Module = llvm::parseAssemblyString(IR, Diagnostic, Context);
      ASSERT_TRUE(Module) << Diagnostic.getMessage().str();
      llvm_original_image::mark(*Module->getGlobalVariable("__nd_data_2000"));
      rewrite_source::setOriginalVA(*Module->getFunction("read_field"), 0x1000);
      auto Record = NdType::makeNamedRecord("ObservedRecord", 8);
      Record->FieldDisplayNames = {"State", "Length"};
      Record->FieldDisplayOffsets = {0, 4};
      Record->FieldDisplayTypes = {NdType::makeInt(4, false),
                                   NdType::makeInt(4, false)};
      for (const bool MatchingWidth : {true, false}) {
        SCOPED_TRACE(MatchingWidth);
        auto PointerType = NdType::makePtr(Record);
        if (!MatchingWidth)
          PointerType->Size = 4;
        SourceVariableTypes Types;
        Types.Globals[0x2008] = PointerType;
        CSourceMap Map;
        CEmitterOptions Options;
        Options.TheArch = Arch::X64;
        Options.Format = Format;
        Options.SourceMap = &Map;
        Options.VariableTypes = &Types;
        std::string Text;
        llvm::raw_string_ostream Stream(Text);
        ASSERT_TRUE(LLVMCEmitter().emit(*Module, Stream, Options));
        // A type describes the loaded pointer, never the backing array's
        // address or a replacement scalar with independent storage.
        EXPECT_NE(Text.find("extern uint8_t unk_2000[32]"), std::string::npos)
            << Text;
        EXPECT_EQ(Text.find("->Length") != std::string::npos, MatchingWidth)
            << Text;
        EXPECT_EQ(Text.find("uint64_t qword_2008;"), std::string::npos) << Text;
        const auto Global = Map.Variables.find({0x1000, "global:8200"});
        ASSERT_NE(Global, Map.Variables.end()) << Text;
        EXPECT_EQ(Global->second.Address, 0x2008u);
        EXPECT_EQ(Global->second.Bytes, 8u);
        EXPECT_FALSE(Global->second.Ambiguous);
      }
    }
}

TEST(CSourcePresentation, WideImageReadsShowTextInBothCRoutes) {
  struct Case {
    std::vector<uint8_t> Bytes;
    std::string Text;
  };
  const std::string Long = "A copied string across vector loads";
  std::vector<Case> Cases;
  Cases.push_back({std::vector<uint8_t>(Long.begin(), Long.end()),
                   "\"A copied string across vector loads\""});
  for (unsigned Unit : {2u, 4u}) {
    for (bool Big : {false, true}) {
      std::vector<uint8_t> Bytes;
      for (unsigned char Ch : Long)
        for (unsigned I = 0; I < Unit; ++I)
          Bytes.push_back(I == (Big ? Unit - 1 : 0) ? Ch : 0);
      const char *Encoding = Unit == 2 ? (Big ? "UTF-16BE" : "UTF-16LE")
                                       : (Big ? "UTF-32BE" : "UTF-32LE");
      Cases.push_back(
          {std::move(Bytes), std::string(Encoding) + " \"" + Long + "\""});
    }
  }
  const std::string Unicode = "中文字符 copied string";
  Cases.push_back({std::vector<uint8_t>(Unicode.begin(), Unicode.end()),
                   "中文字符 copied string"});
  Cases.push_back(
      {{0xd6, 0xd0, 0xce, 0xc4, 0xd7, 0xd6, 0xb7, 0xfb}, "GBK \"中文字符\""});
  for (const auto &Test : Cases) {
    BinaryImage Image;
    Image.Arch = Arch::X64;
    Image.Bits = Bitness::Bits64;
    Image.Format = BinaryFormat::ELF;
    Segment Data;
    Data.VA = 0x2000;
    Data.Name = ".rodata";
    Data.Flags = SegmentFlags::Readable;
    Data.Data = Test.Bytes;
    Data.Data.resize(Data.Data.size() + 16, 0);
    Data.Size = Data.Data.size();
    Image.Segments.push_back(std::move(Data));
    HighFunc Function;
    Function.Entry = 0x1000;
    Function.Name = "read_chunk";
    Function.ReturnType = NdType::makeInt(16, false);
    HighStmt Return;
    Return.Kind = StmtKind::Return;
    Return.RetVal =
        HighExpr::makeLoad(HighExpr::makeConst(0x2000, 8), Function.ReturnType);
    Function.Body.push_back(std::move(Return));
    llvm::LLVMContext Context;
    llvm::SMDiagnostic Diagnostic;
    auto Module = llvm::parseAssemblyString(R"(
      target datalayout = "e-p:64:64-i64:64-i128:128-n8:16:32:64-S128"
      define i128 @read_chunk() {
        %value = load i128, ptr inttoptr (i64 8192 to ptr), align 16
        ret i128 %value
      }
      define i128 @read_volatile_chunk() {
        %value = load volatile i128, ptr inttoptr (i64 8192 to ptr), align 16
        ret i128 %value
      }
      define i128 @read_unknown(ptr %address) {
        %value = load volatile i128, ptr %address, align 16
        ret i128 %value
      }
    )",
                                            Diagnostic, Context);
    ASSERT_TRUE(Module) << Diagnostic.getMessage().str();
    for (bool LLVM : {false, true}) {
      for (bool Comments : {true, false}) {
        CEmitterOptions Options;
        Options.TheArch = Image.Arch;
        Options.Format = Image.Format;
        Options.Image = &Image;
        Options.EmitComments = Comments;
        std::string Text;
        llvm::raw_string_ostream Stream(Text);
        ASSERT_TRUE(LLVM ? LLVMCEmitter().emit(*Module, Stream, Options,
                                               nullptr, &Image)
                         : HighCEmitter().emit({Function}, Stream, Options));
        if (Comments)
          EXPECT_NE(Text.find(Test.Text), std::string::npos) << Text;
        else
          EXPECT_EQ(Text.find(Test.Text), std::string::npos) << Text;
        if (LLVM) {
          for (const char *Name :
               {"read_chunk", "read_volatile_chunk", "read_unknown"}) {
            const size_t Function = Text.rfind(std::string(Name) + "(");
            ASSERT_NE(Function, std::string::npos) << Text;
            const size_t Begin = Text.find('{', Function);
            ASSERT_NE(Begin, std::string::npos) << Text;
            const size_t End = Text.find("\n}", Begin);
            ASSERT_NE(End, std::string::npos) << Text;
            // The GUI folds declarations. The load's text must survive in
            // the visible body, including ordered accesses that cannot inline.
            const std::string Body = Text.substr(Begin, End - Begin);
            if (Comments && llvm::StringRef(Name) != "read_unknown")
              EXPECT_NE(Body.find(Test.Text), std::string::npos) << Body;
            else
              EXPECT_EQ(Body.find(Test.Text), std::string::npos) << Body;
          }
        }
      }
    }
  }
}

TEST(CSourcePresentation, StackTextSurvivesBothRoutesAsCommentsOnly) {
  for (unsigned Unit : {1u, 2u, 4u}) {
    for (bool Big : {false, true}) {
      if (Unit == 1 && Big)
        continue;
      SourceFrameBytes Observation{0x1234, -256, {}};
      const std::string Message = "Text assembled in separate writes";
      for (unsigned char Character : Message)
        for (unsigned I = 0; I < Unit; ++I)
          Observation.Bytes.push_back(I == (Big ? Unit - 1 : 0) ? Character
                                                                : 0);
      Observation.Bytes.insert(Observation.Bytes.end(), Unit, 0);
      HighFunc Function;
      Function.Entry = 0x1000;
      Function.Name = "frame_text";
      Function.ReturnType = NdType::makeInt(8);
      Function.FrameBytes.push_back(Observation);
      HighStmt Return;
      Return.Kind = StmtKind::Return;
      Return.RetVal = HighExpr::makeConst(42, 8);
      Function.Body.push_back(std::move(Return));
      llvm::LLVMContext Context;
      llvm::SMDiagnostic Diagnostic;
      auto Module = llvm::parseAssemblyString(
          "target datalayout = \"e-p:64:64-i64:64-n8:16:32:64-S128\"\n"
          "define i64 @frame_text() { ret i64 42 }",
          Diagnostic, Context);
      ASSERT_TRUE(Module) << Diagnostic.getMessage().str();
      writeSourceFrameBytes(*Module->getFunction("frame_text"), {Observation});
      const auto RoundTrip =
          readSourceFrameBytes(*Module->getFunction("frame_text"));
      ASSERT_EQ(RoundTrip.size(), 1U);
      EXPECT_EQ(RoundTrip[0].Bytes, Observation.Bytes);
      EXPECT_EQ(RoundTrip[0].FrameOffset, Observation.FrameOffset);
      EXPECT_EQ(RoundTrip[0].Instruction, Observation.Instruction);
      for (bool LLVM : {false, true}) {
        for (bool Comments : {false, true}) {
          CEmitterOptions Options;
          Options.TheArch = Arch::X64;
          Options.Format = BinaryFormat::ELF;
          Options.EmitComments = Comments;
          Options.PreserveLLVMFunctionTypes = true;
          std::string Text;
          llvm::raw_string_ostream Stream(Text);
          ASSERT_TRUE(LLVM ? LLVMCEmitter().emit(*Module, Stream, Options)
                           : HighCEmitter().emit({Function}, Stream, Options));
          EXPECT_TRUE(Text.find("return 42") != std::string::npos ||
                      Text.find("return 0x2A") != std::string::npos)
              << Text;
          const size_t Body = Text.find('{', Text.rfind("frame_text("));
          ASSERT_NE(Body, std::string::npos) << Text;
          if (Comments) {
            EXPECT_NE(Text.find(Message, Body), std::string::npos) << Text;
            EXPECT_NE(Text.find("stack text at 0x1234", Body),
                      std::string::npos)
                << Text;
            if (Unit > 1) {
              const std::string Encoding = std::string("UTF-") +
                                           (Unit == 2 ? "16" : "32") +
                                           (Big ? "BE" : "LE");
              EXPECT_NE(Text.find(Encoding, Body), std::string::npos) << Text;
            }
          } else {
            EXPECT_EQ(Text.find(Message), std::string::npos) << Text;
          }
        }
      }
      writeSourceFrameBytes(*Module->getFunction("frame_text"), {});
      EXPECT_FALSE(Module->getFunction("frame_text")
                       ->getMetadata(kSourceFrameBytesMetadata));
      EXPECT_TRUE(
          readSourceFrameBytes(*Module->getFunction("frame_text")).empty());
      for (int64_t Offset : {int64_t{0}, int64_t{1}, int64_t{-1},
                             -limits::ir::kSourceFrameAnalysisMaxFrame - 1}) {
        auto Invalid = Observation;
        Invalid.FrameOffset = Offset;
        writeSourceFrameBytes(*Module->getFunction("frame_text"), {Invalid});
        EXPECT_TRUE(
            readSourceFrameBytes(*Module->getFunction("frame_text")).empty());
      }
      auto Oversized = Observation;
      Oversized.Bytes.resize(limits::ir::kSourceFrameTextBytes + 1);
      writeSourceFrameBytes(*Module->getFunction("frame_text"), {Oversized});
      EXPECT_TRUE(
          readSourceFrameBytes(*Module->getFunction("frame_text")).empty());
      writeSourceFrameBytes(
          *Module->getFunction("frame_text"),
          std::vector<SourceFrameBytes>(
              limits::ir::kSourceFrameTextObservations + 1, Observation));
      EXPECT_TRUE(
          readSourceFrameBytes(*Module->getFunction("frame_text")).empty());
      Module->getFunction("frame_text")
          ->setMetadata(kSourceFrameBytesMetadata,
                        llvm::MDNode::get(Context, llvm::MDString::get(
                                                       Context, "malformed")));
      EXPECT_TRUE(
          readSourceFrameBytes(*Module->getFunction("frame_text")).empty());
    }
  }
}

} // namespace
