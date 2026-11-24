//===- CSourcePresentationTests.cpp - Source text and variable metadata ---===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Source presentation retains storage widths and decodes image text.
//===----------------------------------------------------------------------===//

#include "SourceCallExecution.h"
#include "gtest/gtest.h"

#include "neverd/backend/LLVMOriginalImageStorage.h"
#include "neverd/backend/Limits.h"
#include "neverd/backend/RewriteSourceIdentity.h"
#include "neverd/backend/c/CSourceMap.h"
#include "neverd/backend/c/HighC/HighCEmitter.h"
#include "neverd/backend/c/LLVMC/LLVMCEmitter.h"
#include "neverd/backend/c/dialect/SourceDialect.h"
#include "neverd/backend/c/render/CTypeFormat.h"
#include "neverd/backend/llvm/LLVMSourceMap.h"
#include "neverd/backend/llvm/SourceFrameBytes.h"
#include "neverd/backend/llvm/SourceImageBytes.h"
#include "neverd/debug/DebugContext.h"
#include "neverd/ir/Limits.h"
#include "neverd/ir/high/HighIR.h"
#include "neverd/loader/BinaryImage.h"
#include "neverd/loader/RelocationProvenance.h"

#include "llvm/AsmParser/Parser.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/Support/SourceMgr.h"

using namespace neverd;

TEST(CSourcePresentation, CatalogCNamesDoNotChangeSourceTypes) {
  const std::string Original = "sample::Box<sample::Pair<int, long>, 3>";
  auto Record = NdType::makeNamedRecord(Original, 16);
  auto Pointer = NdType::makePtr(Record);
  Pointer->Size = 4;
  const auto Spell = [&](llvm::StringRef Name) -> std::string {
    return Name == Original ? "sample_Box_Pair_3" : std::string();
  };
  {
    CRecordNameSpelling Scope(Spell);
    EXPECT_EQ(typeToC(Pointer), "sample_Box_Pair_3*");
    EXPECT_EQ(declarationToC(Pointer, "", true), Original + "*");
    EXPECT_EQ(Pointer->Size, 4u);
    EXPECT_EQ(Record->SourceName, Original);
    const auto Nested = [](llvm::StringRef) { return std::string("nested"); };
    {
      CRecordNameSpelling Inner(Nested);
      EXPECT_EQ(typeToC(Pointer), "nested*");
    }
    EXPECT_EQ(typeToC(Pointer), "sample_Box_Pair_3*");
  }
  EXPECT_TRUE(mappedCRecordName(Original).empty());
}

TEST(CSourcePresentation, ThunkCalleeRetainsItsQualifiedSymbolIdentity) {
  class MethodDebug final : public NullDebugContext {
  public:
    std::optional<FunctionSym> resolveFunction(va_t Address) const override {
      if (Address != 0x2000)
        return std::nullopt;
      FunctionSym Function;
      Function.Addr = Address;
      Function.Name = "draw";
      Function.ReturnType = NdType::makeVoid();
      Function.Params = {{"self", NdType::makeInt(8)}};
      return Function;
    }
  } Debug;
  const std::string Target = "_ZN6Widget4drawEv";
  const std::string Thunk = "_ZThn24_N6Widget4drawEv";
  BinaryImage Image;
  Image.Arch = Arch::AArch64;
  Image.Format = BinaryFormat::ELF;
  Image.Bits = Bitness::Bits64;
  Symbol Method = Symbol::makeFunc(0x2000);
  Method.Name = Target;
  Image.Symbols.push_back(Method);
  for (const std::string Name : {std::string("sub_2000"), std::string("draw"),
                                 Target, std::string("user_draw")}) {
    SCOPED_TRACE(Name);
    HighFunc Function;
    Function.Name = Thunk;
    Function.Entry = 0x1000;
    Function.ReturnType = NdType::makeVoid();
    Function.Params.push_back({"self", NdType::makeInt(8)});
    MedVar Self;
    Self.Kind = MedVar::Param;
    Self.Id = 0;
    Self.Size = 8;
    Self.TheArch = Arch::AArch64;
    HighStmt Call;
    Call.Kind = StmtKind::Call;
    Call.CallExpr = HighExpr::makeCall(
        Name, 0x2000,
        {HighExpr::makeBinop(NdOp::INT_SUB,
                             HighExpr::makeVar(Self, NdType::makeInt(8)),
                             HighExpr::makeConst(24, 8))});
    Function.Body.push_back(Call);
    CSourceMap SourceMap;
    CEmitterOptions Options;
    Options.TheArch = Image.Arch;
    Options.Format = Image.Format;
    Options.Image = &Image;
    Options.SourceMap = &SourceMap;
    std::string Text;
    llvm::raw_string_ostream Out(Text);
    ASSERT_TRUE(HighCEmitter().emit({Function}, Out, Options, &Debug));
    const bool Renamed = Name == "user_draw";
    bool LinkedTarget = false, LinkedThunk = false;
    for (const CSourceName &Source : SourceMap.Names) {
      LinkedTarget |= Source.Symbol == (Renamed ? Name : Target) &&
                      Source.Address == 0x2000;
      LinkedThunk |= Source.Symbol == Thunk && Source.Address == 0x1000;
    }
    EXPECT_TRUE(LinkedTarget) << Text;
    EXPECT_TRUE(LinkedThunk) << Text;
    SourceDialectOptions Dialect;
    Dialect.Dialect = SourceDialect::Cpp;
    Dialect.Names = SourceMap.Names;
    const auto Cpp = spellInDialect(Text, Dialect);
    EXPECT_NE(Cpp.Text.find(Renamed ? "user_draw(" : "Widget::draw("),
              std::string::npos)
        << Cpp.Text;
    EXPECT_NE(Cpp.Text.find("0x18"), std::string::npos) << Cpp.Text;
  }
}

TEST(CSourcePresentation, CCallsKeepTheirNamesAcrossImplementationVeneers) {
  class RoutineDebug final : public NullDebugContext {
  public:
    std::optional<FunctionSym> resolveFunction(va_t Address) const override {
      if (Address != 0x2000)
        return std::nullopt;
      FunctionSym Function;
      Function.Addr = Address;
      Function.Name = "consume";
      Function.ReturnType = NdType::makeVoid();
      Function.Params = {{"value", NdType::makeInt(8)}};
      return Function;
    }
  } Debug;
  BinaryImage Image;
  Image.Arch = Arch::AArch64;
  Image.Format = BinaryFormat::ELF;
  Symbol Veneer = Symbol::makeFunc(0x2000);
  Veneer.Name = "__AArch64ADRPThunk_consume";
  Image.Symbols.push_back(Veneer);
  for (const char *Name : {"sub_2000", "consume"}) {
    HighFunc Function;
    Function.Name = "caller";
    Function.Entry = 0x1000;
    Function.ReturnType = NdType::makeVoid();
    HighStmt Call;
    Call.Kind = StmtKind::Call;
    Call.CallExpr =
        HighExpr::makeCall(Name, 0x2000, {HighExpr::makeConst(7, 8)});
    Function.Body.push_back(Call);
    CEmitterOptions Options;
    Options.TheArch = Image.Arch;
    Options.Format = Image.Format;
    Options.Image = &Image;
    std::string Text;
    llvm::raw_string_ostream Out(Text);
    ASSERT_TRUE(HighCEmitter().emit({Function}, Out, Options, &Debug));
    EXPECT_NE(Text.find("consume(7)"), std::string::npos) << Text;
    EXPECT_EQ(Text.find(Veneer.Name), std::string::npos) << Text;
  }
}

TEST(CSourcePresentation, AnImportSlotsPrototypeDeclaresItsRecords) {
  // A unit that reads an import's slot declares the slot a pointer to the
  // routine its prototype gives.  Go's Windows syscalls read
  // `__imp_GetThreadContext`, whose prototype names `_CONTEXT`: the unit
  // spelled the record with no declaration, an unknown type to C.
  constexpr va_t Slot = 0x3000;
  class SlotDebug final : public NullDebugContext {
  public:
    std::optional<FunctionSym> resolveFunction(va_t Address) const override {
      if (Address != Slot)
        return std::nullopt;
      auto Context = std::make_shared<NdType>();
      Context->Kind = NdTypeKind::Struct;
      Context->SourceName = "_CONTEXT";
      FunctionSym Function;
      Function.Addr = Address;
      Function.Name = "GetThreadContext";
      Function.ReturnType = NdType::makeInt(4, true);
      Function.Params = {{"hThread", NdType::makePtr()},
                         {"lpContext", NdType::makePtr(Context)}};
      return Function;
    }
  } Debug;
  BinaryImage Image;
  Image.Arch = Arch::X64;
  Image.Format = BinaryFormat::COFF;
  Image.Bits = Bitness::Bits64;
  Import GetThreadContext;
  GetThreadContext.Module = "KERNEL32.dll";
  GetThreadContext.Name = "GetThreadContext";
  GetThreadContext.IATAddr = Slot;
  Image.Imports.push_back(GetThreadContext);
  // keep(void **out): *out = __imp_GetThreadContext.
  MedVar Out;
  Out.Kind = MedVar::Param;
  Out.Id = 0;
  Out.Size = 8;
  Out.TheArch = Arch::X64;
  HighFunc Function;
  Function.Name = "keep";
  Function.Entry = 0x1000;
  Function.ReturnType = NdType::makeVoid();
  Function.Params = {{"arg0", NdType::makeInt(8, false)}};
  HighStmt Store;
  Store.Kind = StmtKind::Store;
  Store.StoreAddr = HighExpr::makeVar(Out, NdType::makeInt(8, false));
  Store.StoreVal = HighExpr::makeLoad(HighExpr::makeConst(Slot, 8),
                                      NdType::makeInt(8, false));
  Function.Body.push_back(Store);
  CEmitterOptions Options;
  Options.TheArch = Image.Arch;
  Options.Format = Image.Format;
  Options.Image = &Image;
  std::string Text;
  llvm::raw_string_ostream Out2(Text);
  ASSERT_TRUE(HighCEmitter().emit({Function}, Out2, Options, &Debug));
  const size_t Slots = Text.find("__imp_GetThreadContext");
  ASSERT_NE(Slots, std::string::npos) << Text;
  const size_t Record = Text.find("typedef struct _CONTEXT _CONTEXT;");
  EXPECT_LT(Record, Slots) << Text;
  source_call_execution_test::checkSyntax(Text);
}

TEST(CSourcePresentation, ExternalCallAddressesSurviveCProjection) {
  for (const bool Ambiguous : {false, true}) {
    HighFunc Function;
    Function.Name = "caller";
    Function.Entry = 0x1000;
    Function.ReturnType = NdType::makeVoid();
    // No image symbol is needed: the call already owns its exact target.
    // Repeating a conflicting name must never restore an ambiguous address.
    for (va_t Address : {va_t(0x2000), Ambiguous ? va_t(0x3000) : va_t(0x2000),
                         va_t(0x2000)}) {
      HighStmt Call;
      Call.Kind = StmtKind::Call;
      Call.CallExpr = HighExpr::makeCall("Widget::flush", Address, {});
      Function.Body.push_back(Call);
    }
    CSourceMap Map;
    CEmitterOptions Options;
    Options.SourceMap = &Map;
    std::string Text;
    llvm::raw_string_ostream Out(Text);
    ASSERT_TRUE(HighCEmitter().emit({Function}, Out, Options));
    bool Found = false;
    for (const auto &Name : Map.Names)
      if (Name.Symbol == "Widget::flush") {
        Found = true;
        EXPECT_EQ(Name.Address,
                  Ambiguous ? std::nullopt : std::optional<va_t>(0x2000));
      }
    EXPECT_TRUE(Found) << Text;
  }
}

TEST(CSourcePresentation, IndexedRelocationsPreserveStringStorageProofs) {
  for (BinaryFormat Format :
       {BinaryFormat::ELF, BinaryFormat::COFF, BinaryFormat::MachO}) {
    BinaryImage Image;
    Image.Format = Format;
    Image.Arch = Arch::AArch64;
    Image.Bits = Bitness::Bits64;
    Segment Data;
    Data.VA = 0x2000;
    Data.Flags = SegmentFlags::Readable;
    Data.Size = Data.FileSz = 0x100;
    Data.Data.resize(Data.Size, 0);
    const std::string Text = "indexed text";
    std::copy(Text.begin(), Text.end(), Data.Data.begin() + 0x20);
    Image.Segments.push_back(std::move(Data));
    Section DataSection;
    DataSection.VA = 0x2000;
    DataSection.Size = DataSection.FileSz = 0x100;
    DataSection.Flags = SegmentFlags::Readable;
    Image.Sections.push_back(DataSection);
    Image.BaseRelocations.push_back(BaseRelocation{0x2080, 3});
    RelocationEntry Reloc;
    Reloc.Address = 0x2070;
    Image.Relocations.push_back(Reloc);
    Reloc.Address = 0x2060;
    Image.Relocations.push_back(Reloc);
    Image.Relocations.push_back(Reloc);
    Image.CodePtrRelocSlots.insert(0x2090);
    Image.DataPtrRelocSlots.insert(0x2098);
    Image.RelDataPtrRelocSlots.insert(0x20a0);
    Image.RelCodeRelocSlots.insert(0x20a8);
    Image.ImportPtrSlots[0x20b0] = "imported";
    Image.ImportPtrSlots[0x20b9] = "unaligned";
    Image.DyldBindSlots[0x20c0] = ImportBindSlot{"bound"};
    for (bool Relocated : {false, true}) {
      if (Relocated) {
        // A slot beginning before the text still changes its first bytes.
        Reloc.Address = 0x201f;
        Image.Relocations.push_back(Reloc);
      }
      const RelocationProvenanceIndex Index(Image);
      for (va_t First = 0x1ff8; First < 0x2110; ++First)
        for (va_t Width : {va_t{0}, va_t{1}, va_t{7}, va_t{20}}) {
          bool Expected = false;
          for (va_t Byte = First; Byte < First + Width; ++Byte)
            Expected |= Image.hasRelocationProvenanceAt(Byte);
          EXPECT_EQ(
              hasRelocationProvenanceIn(Image, First, First + Width, &Index),
              Expected);
        }
      const auto Plain = imageStringLiteral(&Image, 0x2020);
      const auto Indexed =
          imageStringLiteral(&Image, 0x2020, false, false, &Index);
      EXPECT_EQ(Plain, Indexed);
      EXPECT_EQ(Indexed.has_value(), !Relocated);
      EXPECT_EQ(imageCString(&Image, 0x2020, &Index).has_value(), !Relocated);
    }
  }
}

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

TEST(CSourcePresentation, TypedImageCallArgumentsRetainTheirStorageAddress) {
  class TextCallDebug final : public NullDebugContext {
  public:
    unsigned PointerBytes = 8;
    bool PointerParameter = true;
    bool Admitted = true;
    std::optional<FunctionSym>
    resolvePrototype(llvm::StringRef Name) const override {
      if (Name != "consume_text")
        return std::nullopt;
      auto Pointer = NdType::makePtr(NdType::makeInt(2, false));
      Pointer->Size = PointerBytes;
      FunctionSym Function;
      Function.Name = Name.str();
      Function.ReturnType = NdType::makeVoid();
      Function.Params = {{"value", PointerParameter
                                       ? Pointer
                                       : NdType::makeInt(PointerBytes, false)}};
      Function.PrototypeFromHeader = true;
      Function.PrototypeLibrary = "test-header";
      Function.PlatformConvention = Admitted;
      return Function;
    }
  } Debug;
  for (Arch Architecture : {Arch::X86, Arch::X64, Arch::AArch64})
    for (BinaryFormat Format :
         {BinaryFormat::COFF, BinaryFormat::ELF, BinaryFormat::MachO}) {
      SCOPED_TRACE(unsigned(Architecture));
      SCOPED_TRACE(unsigned(Format));
      Debug.PointerBytes = Architecture == Arch::X86 ? 4 : 8;
      const std::string Bits = std::to_string(Debug.PointerBytes * 8);
      BinaryImage Image;
      Image.Arch = Architecture;
      Image.Bits =
          Architecture == Arch::X86 ? Bitness::Bits32 : Bitness::Bits64;
      Image.Format = Format;
      Segment Data;
      Data.VA = 0x2000;
      for (char Ch : std::string("ConfigToken")) {
        Data.Data.push_back(Ch);
        Data.Data.push_back(0);
      }
      Data.Data.insert(Data.Data.end(), {0, 0});
      Data.Size = Data.FileSz = Data.Data.size();
      // Displaying the initial image text grants neither immutable bytes
      // nor a replacement C literal with different storage identity.
      Data.Flags = SegmentFlags::Readable | SegmentFlags::Writable;
      Image.Segments.push_back(std::move(Data));
      for (bool Pointer : {false, true})
        for (bool Admitted : {false, true})
          for (bool Dynamic : {false, true})
            for (bool Comments : {false, true}) {
              SCOPED_TRACE(Pointer);
              SCOPED_TRACE(Admitted);
              SCOPED_TRACE(Dynamic);
              SCOPED_TRACE(Comments);
              Debug.PointerParameter = Pointer;
              Debug.Admitted = Admitted;
              llvm::LLVMContext Context;
              llvm::SMDiagnostic Diagnostic;
              const std::string IR = "target datalayout = \"e-p:" + Bits + ":" +
                                     Bits +
                                     "\"\n"
                                     "declare void @consume_text(i" +
                                     Bits +
                                     ")\n"
                                     "define void @show_text(i" +
                                     Bits +
                                     " %input) {\n"
                                     "  call void @consume_text(i" +
                                     Bits + (Dynamic ? " %input" : " 8192") +
                                     ")\n"
                                     "  ret void\n}\n";
              auto Module = llvm::parseAssemblyString(IR, Diagnostic, Context);
              ASSERT_TRUE(Module) << Diagnostic.getMessage().str();
              CEmitterOptions Options;
              Options.TheArch = Architecture;
              Options.Format = Format;
              Options.EmitComments = Comments;
              std::string Text;
              llvm::raw_string_ostream Out(Text);
              ASSERT_TRUE(
                  LLVMCEmitter().emit(*Module, Out, Options, &Debug, &Image));
              EXPECT_EQ(Text.find("UTF-16LE \"ConfigToken\"") !=
                            std::string::npos,
                        Pointer && Admitted && !Dynamic && Comments)
                  << Text;
              EXPECT_EQ(Text.find("L\"ConfigToken\""), std::string::npos)
                  << Text;
              if (!Dynamic)
                EXPECT_NE(Text.find("0x2000"), std::string::npos) << Text;
            }
    }
}

TEST(CSourcePresentation, LLVMImagePointerCallsUseTheSharedTextScanner) {
  BinaryImage Image;
  Image.Arch = Arch::X64;
  Image.Bits = Bitness::Bits64;
  Image.Format = BinaryFormat::COFF;
  Segment Data;
  Data.VA = 0x2000;
  const std::string Message = "Config*/Token";
  Data.Data.assign(Message.begin(), Message.end());
  Data.Data.push_back(0);
  Data.Size = Data.FileSz = Data.Data.size();
  Data.Flags = SegmentFlags::Readable | SegmentFlags::Writable;
  Image.Segments.push_back(std::move(Data));
  const auto Comment = imageStringComment(&Image, 0x2000);
  ASSERT_TRUE(Comment);
  ASSERT_EQ(Comment->find("*/"), std::string::npos);
  llvm::LLVMContext Context;
  llvm::SMDiagnostic Diagnostic;
  auto Module = llvm::parseAssemblyString(R"(
    target datalayout = "e-p:64:64"
    declare void @consume_text(ptr)
    define void @show_text() {
      call void @consume_text(ptr inttoptr (i64 8192 to ptr))
      ret void
    }
  )",
                                          Diagnostic, Context);
  ASSERT_TRUE(Module);
  for (bool Comments : {false, true}) {
    CEmitterOptions Options;
    Options.TheArch = Image.Arch;
    Options.Format = Image.Format;
    Options.EmitComments = Comments;
    std::string Text;
    llvm::raw_string_ostream Out(Text);
    ASSERT_TRUE(LLVMCEmitter().emit(*Module, Out, Options, nullptr, &Image));
    const auto Call = Text.find("consume_text(", Text.find("void show_text"));
    ASSERT_NE(Call, std::string::npos) << Text;
    const auto End = Text.find(';', Call);
    const auto Statement = Text.substr(Call, End - Call);
    EXPECT_EQ(Statement.find("/* " + *Comment + " */") != std::string::npos,
              Comments)
        << Text;
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

TEST(CSourcePresentation, DebugGlobalPointerKeepsItsRecordMembers) {
  class GlobalDebug final : public NullDebugContext {
  public:
    TypeRef Pointer;
    std::optional<DataObjectSym>
    resolveDataObject(va_t Address) const override {
      if (Address != 0x2000)
        return std::nullopt;
      return DataObjectSym{"GlobalRoot", Address, Pointer->Size, false,
                           Pointer};
    }
  } Debug;
  auto Record = NdType::makeNamedRecord("Device", 16);
  Record->FieldDisplayNames = {"flags", "value"};
  Record->FieldDisplayOffsets = {0, 8};
  Record->FieldDisplayTypes = {NdType::makeInt(8, false),
                               NdType::makeInt(8, false)};
  for (Arch Architecture : {Arch::X86, Arch::X64, Arch::ARM, Arch::AArch64}) {
    const uint16_t Bytes =
        Architecture == Arch::X86 || Architecture == Arch::ARM ? 4 : 8;
    Debug.Pointer = NdType::makePtr(Record);
    Debug.Pointer->Size = Bytes;
    for (bool MatchingWidth : {true, false}) {
      SCOPED_TRACE(int(Architecture));
      SCOPED_TRACE(MatchingWidth);
      BinaryImage Image;
      Image.Arch = Architecture;
      Image.Format = BinaryFormat::ELF;
      Image.Bits = Bytes == 8 ? Bitness::Bits64 : Bitness::Bits32;
      Segment Data;
      Data.VA = 0x2000;
      Data.Data.resize(32);
      Data.Size = Data.FileSz = Data.Data.size();
      Data.Flags = SegmentFlags::Readable | SegmentFlags::Writable;
      Image.Segments.push_back(std::move(Data));
      HighFunc Function;
      Function.Name = "read_value";
      Function.Entry = 0x1000;
      Function.ReturnType = NdType::makeInt(8, false);
      auto Pointer =
          HighExpr::makeLoad(HighExpr::makeConst(0x2000, Bytes),
                             NdType::makeInt(MatchingWidth ? Bytes : 2, false));
      HighStmt Return;
      Return.Kind = StmtKind::Return;
      Return.RetVal =
          HighExpr::makeLoad(HighExpr::makeBinop(NdOp::INT_ADD, Pointer,
                                                 HighExpr::makeConst(8, Bytes)),
                             Function.ReturnType);
      Function.Body.push_back(Return);
      CEmitterOptions Options;
      Options.TheArch = Architecture;
      Options.Format = Image.Format;
      Options.Image = &Image;
      Options.RecordDefinitions = [](const std::vector<TypeRef> &) {
        CRecordDefinitions Definitions;
        Definitions.Text =
            "struct Device { uint64_t flags; uint64_t value; };\n";
        Definitions.Records["Device"] = false;
        return Definitions;
      };
      std::string Text;
      llvm::raw_string_ostream Stream(Text);
      ASSERT_TRUE(HighCEmitter().emit({Function}, Stream, Options, &Debug));
      EXPECT_EQ(Text.find("GlobalRoot->value") != std::string::npos,
                MatchingWidth)
          << Text;
      if (MatchingWidth && Bytes == sizeof(uintptr_t))
        source_call_execution_test::compileAndRun(
            Text + "\nint main(void) { struct Device a = {1, 17}, b = {2, 39}; "
                   "GlobalRoot = &a; if (read_value() != 17) return 1; "
                   "GlobalRoot = &b; return read_value() != 39; }\n",
            {"-Werror=int-conversion", "-Werror=incompatible-pointer-types"});
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

TEST(CSourcePresentation, DataSectionsInExecutableMappingsRetainTextComments) {
  const std::string Message = "R42独立文本 fixture";
  for (BinaryFormat Format :
       {BinaryFormat::ELF, BinaryFormat::COFF, BinaryFormat::MachO}) {
    BinaryImage Image;
    Image.Arch = Arch::AArch64;
    Image.Bits = Bitness::Bits64;
    Image.Format = Format;
    Segment Mapping;
    Mapping.VA = 0x2000;
    Mapping.Flags = SegmentFlags::Readable | SegmentFlags::Executable;
    Mapping.Data.resize(0x100, 0);
    std::copy(Message.begin(), Message.end(), Mapping.Data.begin() + 0x20);
    Mapping.Size = Mapping.FileSz = Mapping.Data.size();
    Image.Segments.push_back(std::move(Mapping));
    Section Data;
    Data.Name = "stored_text";
    Data.VA = 0x2020;
    Data.Size = Data.FileSz = Message.size() + 1;
    Data.Flags = SegmentFlags::Readable;
    Image.Sections.push_back(Data);
    const auto Comment = imageStringComment(&Image, Data.VA);
    ASSERT_TRUE(Comment) << static_cast<int>(Format);
    EXPECT_NE(Comment->find(Message), std::string::npos);
    ASSERT_TRUE(imageCString(&Image, Data.VA));

    HighFunc Function;
    Function.Entry = 0x1000;
    Function.Name = "read_rx_data";
    Function.ReturnType = NdType::makeInt(16, false);
    MedVar Value;
    Value.Kind = MedVar::Temp;
    Value.Id = 7;
    Value.Size = 16;
    HighStmt Load;
    Load.Kind = StmtKind::Assign;
    Load.Dst = HighExpr::makeVar(Value, Function.ReturnType);
    Load.Val = HighExpr::makeLoad(HighExpr::makeConst(Data.VA, 8),
                                  Function.ReturnType);
    Function.Body.push_back(std::move(Load));
    HighStmt Return;
    Return.Kind = StmtKind::Return;
    Return.RetVal = HighExpr::makeVar(Value, Function.ReturnType);
    Function.Body.push_back(std::move(Return));
    llvm::LLVMContext Context;
    llvm::SMDiagnostic Diagnostic;
    auto Module = llvm::parseAssemblyString(R"(
      target datalayout = "e-p:64:64-i64:64-i128:128-n8:16:32:64-S128"
      define i128 @read_rx_data() {
        %value = load volatile i128, ptr inttoptr (i64 8224 to ptr), align 1
        ret i128 %value
      }
    )",
                                            Diagnostic, Context);
    ASSERT_TRUE(Module);
    for (bool LLVM : {false, true})
      for (bool Comments : {false, true}) {
        CEmitterOptions Options;
        Options.TheArch = Image.Arch;
        Options.Format = Format;
        Options.Image = &Image;
        Options.EmitComments = Comments;
        std::string Text;
        llvm::raw_string_ostream Out(Text);
        ASSERT_TRUE(
            LLVM ? LLVMCEmitter().emit(*Module, Out, Options, nullptr, &Image)
                 : HighCEmitter().emit({Function}, Out, Options));
        const auto Start = Text.rfind("read_rx_data(");
        ASSERT_NE(Start, std::string::npos) << Text;
        const auto Body = Text.substr(Text.find('{', Start));
        EXPECT_EQ(Body.find(Message) != std::string::npos, Comments) << Body;
      }

    // A readable mapping alone does not authorize headers, gaps or code.
    EXPECT_FALSE(imageStringComment(&Image, 0x2000));
    Image.Sections[0].Flags = SegmentFlags::Readable | SegmentFlags::Executable;
    Image.Sections[0].Type = Format == BinaryFormat::MachO
                                 ? llvm::MachO::S_ATTR_PURE_INSTRUCTIONS
                                 : 0;
    EXPECT_FALSE(imageStringComment(&Image, Data.VA));
    EXPECT_FALSE(imageCString(&Image, Data.VA));
    Image.Sections[0] = Data;
    // A terminator in the next region does not terminate this data object.
    --Image.Sections[0].Size;
    EXPECT_FALSE(imageCString(&Image, Data.VA));
    Image.Sections.clear();
    EXPECT_FALSE(imageStringComment(&Image, Data.VA));
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

TEST(CSourcePresentation, LLVMRecordInventoryIgnoresCommentsAndLiterals) {
  for (Arch Architecture : {Arch::AArch64, Arch::X64})
    for (BinaryFormat Format :
         {BinaryFormat::ELF, BinaryFormat::COFF, BinaryFormat::MachO}) {
      SCOPED_TRACE(unsigned(Architecture));
      SCOPED_TRACE(unsigned(Format));
      llvm::LLVMContext Context;
      llvm::SMDiagnostic Diagnostic;
      auto Module = llvm::parseAssemblyString(R"(
        target datalayout = "e-p:64:64"
        @message = private unnamed_addr constant [15 x i8] c"literal_record\00"
        declare i32 @puts(ptr)
        define i64 @real_identifier() {
          call i32 @puts(ptr @message)
          ret i64 42
        }
      )",
                                              Diagnostic, Context);
      ASSERT_TRUE(Module) << Diagnostic.getMessage().str();
      const std::string Comment = "comment_record entry";
      SourceImageBytes Value{
          0x1100, 0x1120, 0x4020, {}, {{0x4000, 0x1000, 0x47}}};
      Value.Bytes.assign(Comment.begin(), Comment.end());
      Value.Bytes.push_back(0);
      writeSourceImageBytes(*Module->getFunction("real_identifier"),
                            {{Value}, true});
      auto *Function = Module->getFunction("real_identifier");
      LLVMSourceMap Sources;
      Sources.Functions[0x1000] = Function;
      Sources.Observations.push_back(
          {0x1000, {0x1010, 0}, &Function->getEntryBlock().front()});
      CEmitterOptions Options;
      Options.TheArch = Architecture;
      Options.Format = Format;
      Options.PreserveLLVMFunctionTypes = true;
      Options.LLVMSources = &Sources;
      bool SawCodeIdentifier = false;
      Options.RecordDefinitions = [&](const std::vector<TypeRef> &Types) {
        for (auto Type : Types) {
          while (Type->Kind == NdTypeKind::Ptr)
            Type = Type->ElemType;
          SawCodeIdentifier |= Type->SourceName == "real_identifier";
          EXPECT_NE(Type->SourceName, "comment_record");
          EXPECT_NE(Type->SourceName, "literal_record");
          EXPECT_NE(Type->SourceName, "entry");
        }
        return CRecordDefinitions{};
      };
      std::string Text;
      llvm::raw_string_ostream Stream(Text);
      ASSERT_TRUE(LLVMCEmitter().emit(*Module, Stream, Options));
      EXPECT_TRUE(SawCodeIdentifier);
      EXPECT_NE(Text.find("comment_record entry"), std::string::npos) << Text;
      EXPECT_NE(Text.find("\"literal_record\""), std::string::npos) << Text;
      CSourceMap Map;
      Options.SourceMap = &Map;
      std::string Mapped;
      llvm::raw_string_ostream MappedStream(Mapped);
      ASSERT_TRUE(LLVMCEmitter().emit(*Module, MappedStream, Options));
      EXPECT_EQ(Mapped, Text);
      bool CallAnchored = false;
      for (const auto &Anchor : Map.Anchors)
        if (llvm::StringRef(Mapped)
                .slice(Anchor.Span.Begin, Anchor.Span.End)
                .contains("puts("))
          for (const auto &Occurrence : Anchor.Occurrences)
            CallAnchored |= Occurrence.Address == 0x1010;
      EXPECT_TRUE(CallAnchored) << Mapped;
    }
}

TEST(CSourcePresentation, ConditionalImageTextKeepsItsConditionsInBothRoutes) {
  SourceImageBytes Value{0x1100,
                         0x1120,
                         0x4020,
                         {'*', '/', 'x', 0},
                         {{0x4000, 0x1000, 0x47}, {0x4001, 0x1004, 0x4e}}};
  // Conditional observations already have a byte bound. A generic reference
  // preview limit must not hide the recovered suffix in either C route.
  const std::string FullText =
      std::string(limits::backend::kCTypeFormatMaxCharacters + 1, 'a') +
      " recovered_tail */x";
  Value.Bytes.assign(FullText.begin(), FullText.end());
  Value.Bytes.push_back(0);
  Value.EntryRegisters = {{0x40, 8, 0x4000}, {0x50, 1, 7}};
  SourceImageByteObservations Observations{{Value}, false};
  HighFunc Function;
  Function.Entry = 0x1000;
  Function.Name = "conditional_text";
  Function.ReturnType = NdType::makeInt(8);
  Function.ImageBytes = Observations;
  HighStmt Return;
  Return.Kind = StmtKind::Return;
  Return.RetVal = HighExpr::makeConst(42, 8);
  Function.Body.push_back(std::move(Return));
  llvm::LLVMContext Context;
  llvm::SMDiagnostic Diagnostic;
  auto Module = llvm::parseAssemblyString(
      "target datalayout = \"e-p:64:64-i64:64-n8:16:32:64-S128\"\n"
      "define i64 @conditional_text() { ret i64 42 }",
      Diagnostic, Context);
  ASSERT_TRUE(Module);
  auto &LLVMFunction = *Module->getFunction("conditional_text");
  writeSourceImageBytes(LLVMFunction, Observations);
  const auto RoundTrip = readSourceImageBytes(LLVMFunction);
  EXPECT_EQ(RoundTrip.Values, Observations.Values);
  EXPECT_FALSE(RoundTrip.Complete);
  std::string Annotation;
  for (bool LLVM : {false, true})
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
      const auto At = Text.find("/* conditional text");
      if (!Comments) {
        EXPECT_EQ(At, std::string::npos) << Text;
        EXPECT_EQ(Text.find("image text observations incomplete"),
                  std::string::npos)
            << Text;
        continue;
      }
      ASSERT_NE(At, std::string::npos) << Text;
      const auto End = Text.find('\n', At);
      const auto Line = Text.substr(At, End - At);
      if (Annotation.empty())
        Annotation = Line;
      EXPECT_EQ(Line, Annotation);
      EXPECT_NE(Line.find("after 0x1120 at 0x4020"), std::string::npos);
      EXPECT_NE(Line.find("requires at block 0x1100 entry:"),
                std::string::npos);
      EXPECT_NE(Line.find("[0x4000]=0x47 (initializer 0x1000)"),
                std::string::npos);
      EXPECT_NE(Line.find("[0x4001]=0x4E (initializer 0x1004)"),
                std::string::npos);
      EXPECT_NE(Line.find("reg[0x40:8]=0x4000"), std::string::npos);
      EXPECT_NE(Line.find("reg[0x50:1]=0x7"), std::string::npos);
      EXPECT_NE(Line.find(" recovered_tail * /x\""), std::string::npos);
      EXPECT_EQ(Line.find("\xE2\x80\xA6"), std::string::npos);
      // A decoded comment terminator must not create executable C text.
      EXPECT_EQ(Line.find("*/"), Line.size() - 2);
      EXPECT_NE(Text.find("image text observations incomplete"),
                std::string::npos);
    }
  writeSourceImageBytes(LLVMFunction, {});
  EXPECT_FALSE(LLVMFunction.getMetadata(kSourceImageBytesMetadata));
  EXPECT_TRUE(readSourceImageBytes(LLVMFunction).Complete);
  auto Legacy = Observations;
  Legacy.Values.front().EntryRegisters.clear();
  writeSourceImageBytes(LLVMFunction, Legacy);
  EXPECT_EQ(readSourceImageBytes(LLVMFunction).Values, Legacy.Values);
  auto Large = Observations;
  Large.Values.front().Bytes.assign(4097, 'x');
  Large.Values.front().Bytes.back() = 0;
  writeSourceImageBytes(LLVMFunction, Large);
  EXPECT_EQ(readSourceImageBytes(LLVMFunction).Values, Large.Values);
  Large.Values.resize(5000, Value);
  writeSourceImageBytes(LLVMFunction, Large);
  EXPECT_EQ(readSourceImageBytes(LLVMFunction).Values, Large.Values);
  for (unsigned Mode = 0; Mode < 10; ++Mode) {
    auto Invalid = Observations;
    auto &Bad = Invalid.Values.front();
    if (Mode == 0)
      Bad.Inputs.clear();
    else if (Mode == 1)
      Bad.Bytes.back() = 'x';
    else if (Mode == 2)
      Bad.Address = InvalidVA;
    else if (Mode == 3)
      Bad.Inputs.back().Address = Bad.Inputs.front().Address;
    else if (Mode == 4)
      Bad.Bytes.clear();
    else if (Mode == 5)
      Bad.EntryRegisters.front().Bytes = 0;
    else if (Mode == 6)
      Bad.EntryRegisters.front().Bytes = 9;
    else if (Mode == 7)
      Bad.EntryRegisters.front().Offset = UINT64_MAX;
    else if (Mode == 8)
      Bad.EntryRegisters.back().Value = 0x100;
    else
      Bad.EntryRegisters.back().Offset = 0x41;
    writeSourceImageBytes(LLVMFunction, Invalid);
    const auto Read = readSourceImageBytes(LLVMFunction);
    EXPECT_TRUE(Read.Values.empty());
    EXPECT_FALSE(Read.Complete);
  }
  LLVMFunction.setMetadata(
      kSourceImageBytesMetadata,
      llvm::MDNode::get(Context, llvm::MDString::get(Context, "bad")));
  EXPECT_TRUE(readSourceImageBytes(LLVMFunction).Values.empty());
  EXPECT_FALSE(readSourceImageBytes(LLVMFunction).Complete);
}

} // namespace
