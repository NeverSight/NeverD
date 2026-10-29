//===- ObjectExternTests.cpp - Undefined symbols of ELF objects -----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
//
// A relocatable object's relocations against symbols it does not define, and
// against the common symbols it leaves to the linker, resolve to addresses
// the loader places past its sections, on every architecture: a call to
// `exit` reaches the import `exit` and a read of `counter` reads `counter`,
// not address zero, where the object's first function sits.
//
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "neverd/backend/c/CEmitterOptions.h"
#include "neverd/backend/c/HighC/HighCEmitter.h"
#include "neverd/decode/Decoder.h"
#include "neverd/ir/low/CFGBuilder.h"
#include "neverd/loader/BinaryImage.h"
#include "neverd/pipeline/Pipeline.h"
#include "neverd/support/BinaryLoading.h"

#include "llvm/ADT/StringExtras.h"
#include "llvm/Analysis/ValueTracking.h"
#include "llvm/IR/GlobalVariable.h"
#include "llvm/IR/InstIterator.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/Support/Error.h"
#include "llvm/Support/raw_ostream.h"

#include <filesystem>
#include <set>
#include <string>
#include <utility>
#include <vector>

using namespace neverd;

namespace {

struct ExternFixture {
  const char *Object;
  bool PositionIndependent;
  /// AArch64 calls do not yet pass a forwarded parameter or a value loaded
  /// just before a tail call, so `put` prints no `stdout`.
  bool CallArgumentsKnown;
};

constexpr ExternFixture kFixtures[] = {
    {"test_extern_calls_nopic.o", false, true},
    {"test_extern_calls_nopic_i386.o", false, true},
    {"test_extern_calls_nopic_arm.o", false, true},
    {"test_extern_calls_nopic_a64.o", false, false},
    {"test_extern_calls.o", true, true},
    {"test_extern_calls_i386.o", true, true},
    {"test_extern_calls_arm.o", true, true},
    {"test_extern_calls_a64.o", true, false},
};

BinaryImage loadFixture(const char *Object) {
  auto ImageOrErr = loadBinary(std::filesystem::path(TEST_OBJ_DIR) / Object);
  if (!ImageOrErr) {
    ADD_FAILURE() << llvm::toString(ImageOrErr.takeError());
    return BinaryImage();
  }
  return std::move(*ImageOrErr);
}

/// The body of \p Name's definition in \p Source, or empty.
std::string definitionBody(const std::string &Source, const std::string &Name) {
  for (size_t At = Source.find(" " + Name + "("); At != std::string::npos;
       At = Source.find(" " + Name + "(", At + 1)) {
    const size_t LineEnd = Source.find('\n', At);
    if (LineEnd == std::string::npos || LineEnd == 0 ||
        Source[LineEnd - 1] != '{')
      continue;
    const size_t End = Source.find("\n}\n", LineEnd);
    return Source.substr(LineEnd,
                         End == std::string::npos ? End : End - LineEnd);
  }
  return {};
}

/// True when \p Body spells \p Address as a decimal or hexadecimal literal.
bool mentionsAddress(const std::string &Body, va_t Address) {
  return Body.find(std::to_string(Address)) != std::string::npos ||
         Body.find("0x" + llvm::utohexstr(Address)) != std::string::npos;
}

const Symbol *dataSymbol(const BinaryImage &Img, llvm::StringRef Name) {
  for (const Symbol &Sym : Img.Symbols)
    if (Sym.Name == Name && !Sym.IsFunc)
      return &Sym;
  return nullptr;
}

TEST(ObjectExterns,
     OnlyLoaderOwnedObjectCalleesBypassExecutableEntryAdmission) {
  for (bool Relocatable : {false, true}) {
    for (bool Callable : {false, true}) {
      SCOPED_TRACE(::testing::Message() << Relocatable << ':' << Callable);
      BinaryImage Img;
      Img.Arch = Arch::X64;
      Img.Bits = Bitness::Bits64;
      Img.Format = BinaryFormat::ELF;
      Img.IsRelocatable = Relocatable;
      Segment Text;
      Text.VA = 0x1000;
      Text.Size = 5;
      Text.Flags = SegmentFlags::Readable | SegmentFlags::Executable;
      Text.Data = {0xe9, 0xfb, 0x0f, 0, 0}; // jmp 0x2000
      Img.Segments.push_back(std::move(Text));
      Segment Extern;
      Extern.VA = 0x2000;
      Extern.Size = 8;
      Extern.Flags = SegmentFlags::Readable | SegmentFlags::Writable;
      Extern.Data.resize(Extern.Size);
      Img.Segments.push_back(std::move(Extern));
      Img.Imports.push_back({"extern", "fputs", 0, 0x2000});
      Img.Symbols.push_back(Symbol::makeFunc(0x2000));
      if (Callable)
        Img.ObjectExternFunctionEntries.insert(0x2000);
      ASSERT_FALSE(Img.isFunctionEntryCandidateAt(0x2000));
      Decoder Dec;
      ASSERT_TRUE(Dec.init(Arch::X64));
      CFGBuilder Builder;
      const auto Function = Builder.build(Img, Dec, 0x1000, "tail");
      unsigned Calls = 0;
      for (const auto &Block : Function.Blocks)
        for (const auto &Op : Block.Ops)
          if (Op.Opcode == NdOp::CALL && Op.NumInputs &&
              Op.Inputs[0].isConst() && Op.Inputs[0].Offset == 0x2000)
            ++Calls;
      EXPECT_EQ(Calls, unsigned(Relocatable && Callable));
    }
  }
}

TEST(ObjectExterns, UndefinedAndCommonSymbolsResolveOnEveryArchitecture) {
  for (const ExternFixture &Fixture : kFixtures) {
    SCOPED_TRACE(Fixture.Object);
    const BinaryImage Img = loadFixture(Fixture.Object);
    ASSERT_FALSE(Img.Segments.empty());

    // A called extern is an import; data is a symbol in writable storage of
    // its own, outside the object's code.
    std::set<std::string> Imports;
    for (const Import &Imp : Img.Imports)
      Imports.insert(Imp.Name);
    for (const char *Called : {"perror", "exit", "fputs"})
      EXPECT_TRUE(Imports.count(Called)) << Called;
    for (const Import &Imp : Img.Imports)
      EXPECT_TRUE(Img.ObjectExternFunctionEntries.count(Imp.IATAddr));
    // A weak reference keeps the null test the code makes of it.
    EXPECT_FALSE(Imports.count("maybe"));
    EXPECT_EQ(Img.findSymbol("maybe"), nullptr);
    const Symbol *Counter = dataSymbol(Img, "counter");
    const Symbol *Stdout = dataSymbol(Img, "stdout");
    const Symbol *Tentative = dataSymbol(Img, "tentative");
    const Symbol *Defined = dataSymbol(Img, "defined_global");
    ASSERT_NE(Counter, nullptr);
    ASSERT_NE(Stdout, nullptr);
    ASSERT_NE(Tentative, nullptr);
    ASSERT_NE(Defined, nullptr);
    for (const Symbol *Data : {Counter, Stdout, Tentative}) {
      EXPECT_FALSE(Img.ObjectExternFunctionEntries.count(Data->Addr));
      const Segment *Storage = Img.getSegmentFor(Data->Addr);
      ASSERT_NE(Storage, nullptr) << Data->Name;
      EXPECT_TRUE(Storage->isWritable()) << Data->Name;
      EXPECT_FALSE(Img.hasExecutableCodeOwnerAt(Data->Addr)) << Data->Name;
    }
    EXPECT_NE(Counter->Addr, Stdout->Addr);

    const Symbol *Die = Img.findSymbol("die");
    const Symbol *Put = Img.findSymbol("put");
    const Symbol *Bump = Img.findSymbol("bump");
    const Symbol *ReadDefined = Img.findSymbol("read_defined");
    const Symbol *ReadTentative = Img.findSymbol("read_tentative");
    ASSERT_TRUE(Die && Put && Bump && ReadDefined && ReadTentative);
    llvm::LLVMContext Ctx;
    PipelineOptions Opts;
    Opts.EmitDumpOutput = false;
    Opts.OnlyFunctionEntries = {Die->Addr, Put->Addr, Bump->Addr,
                                ReadDefined->Addr, ReadTentative->Addr};
    const PipelineResult Result = Pipeline().run(Img, Ctx, Opts);
    ASSERT_TRUE(Result.Success) << Result.Error;
    std::string Source;
    llvm::raw_string_ostream OS(Source);
    CEmitterOptions Options;
    Options.TheArch = Img.Arch;
    Options.Format = Img.Format;
    Options.Image = &Img;
    ASSERT_TRUE(HighCEmitter().emit(Result.HighFuncs, OS, Options));

    const std::string DieBody = definitionBody(Source, "die");
    ASSERT_FALSE(DieBody.empty()) << Source;
    EXPECT_NE(DieBody.find("perror("), std::string::npos) << DieBody;
    EXPECT_NE(DieBody.find("exit("), std::string::npos) << DieBody;
    EXPECT_EQ(DieBody.find("first("), std::string::npos) << DieBody;
    const std::string PutBody = definitionBody(Source, "put");
    ASSERT_FALSE(PutBody.empty()) << Source;
    EXPECT_NE(PutBody.find("fputs("), std::string::npos) << PutBody;
    EXPECT_EQ(PutBody.find("first("), std::string::npos) << PutBody;

    // A direct reference prints its symbol.  A GOT entry's load folds to the
    // address the entry holds, which prints as a number for the object's own
    // data.  Data another module defines prints, either way, through the
    // declaration the unit gives it, which binds the symbol.
    auto ExpectData = [&](const char *Function, const Symbol &Data) {
      const std::string Body = definitionBody(Source, Function);
      ASSERT_FALSE(Body.empty()) << Source;
      const bool Named = Body.find(Data.Name) != std::string::npos;
      if (Fixture.PositionIndependent)
        EXPECT_TRUE(Named || mentionsAddress(Body, Data.Addr))
            << Data.Name << Body;
      else
        EXPECT_TRUE(Named) << Data.Name << Body;
    };
    ExpectData("bump", *Counter);
    ExpectData("read_defined", *Defined);
    ExpectData("read_tentative", *Tentative);
    EXPECT_NE(definitionBody(Source, "bump").find("neverd_data_counter"),
              std::string::npos)
        << Source;
    EXPECT_NE(Source.find("extern unsigned char neverd_data_counter[] "
                          "__asm__(\"counter\");"),
              std::string::npos)
        << Source;
    if (Fixture.CallArgumentsKnown)
      EXPECT_NE(PutBody.find(Fixture.PositionIndependent ? "neverd_data_stdout"
                                                         : "stdout"),
                std::string::npos)
          << PutBody;
  }
}

TEST(ObjectExterns, COFFObjectsOnEveryMachine) {
  // A COFF object names its externs the same way, and reaches a dllimport
  // function through the `__imp_` pointer an import library supplies.  The
  // imports carry the names an import directory gives them: 32-bit Windows'
  // `__imp__ImportedApi@4` imports `ImportedApi`.
  for (const char *Object :
       {"test_coff_externs_x86_64.obj", "test_coff_externs_i686.obj",
        "test_coff_externs_aarch64.obj"}) {
    SCOPED_TRACE(Object);
    const BinaryImage Img = loadFixture(Object);
    ASSERT_FALSE(Img.Segments.empty());
    std::set<std::string> Imports;
    for (const Import &Imp : Img.Imports)
      Imports.insert(Imp.Name);
    for (const char *Called : {"perror", "exit", "fputs", "ImportedApi"})
      EXPECT_TRUE(Imports.count(Called)) << Called;
    EXPECT_FALSE(Imports.count("maybe"));
    EXPECT_EQ(Img.findSymbol("maybe"), nullptr);

    std::vector<va_t> Entries;
    for (const char *Name :
         {"die", "put", "bump", "call_api", "read_tentative"}) {
      const Symbol *Sym = Img.findSymbol(Name);
      if (!Sym)
        Sym = Img.findSymbol((std::string("_") + Name).c_str());
      ASSERT_NE(Sym, nullptr) << Name;
      Entries.push_back(Sym->Addr);
    }
    llvm::LLVMContext Ctx;
    PipelineOptions Opts;
    Opts.EmitDumpOutput = false;
    Opts.OnlyFunctionEntries.insert(Entries.begin(), Entries.end());
    const PipelineResult Result = Pipeline().run(Img, Ctx, Opts);
    ASSERT_TRUE(Result.Success) << Result.Error;
    std::string Source;
    llvm::raw_string_ostream OS(Source);
    CEmitterOptions Options;
    Options.TheArch = Img.Arch;
    Options.Format = Img.Format;
    Options.Image = &Img;
    ASSERT_TRUE(HighCEmitter().emit(Result.HighFuncs, OS, Options));

    const std::string DieBody = definitionBody(Source, "die");
    ASSERT_FALSE(DieBody.empty()) << Source;
    EXPECT_NE(DieBody.find("perror("), std::string::npos) << DieBody;
    EXPECT_NE(DieBody.find("exit("), std::string::npos) << DieBody;
    EXPECT_EQ(DieBody.find("put("), std::string::npos) << DieBody;
    for (const auto &[Function, Expected] :
         {std::pair<const char *, const char *>{"put", "fputs("},
          {"call_api", "ImportedApi("},
          {"bump", "counter"},
          {"read_tentative", "tentative"}}) {
      const std::string Body = definitionBody(Source, Function);
      ASSERT_FALSE(Body.empty()) << Function << Source;
      EXPECT_NE(Body.find(Expected), std::string::npos) << Function << Body;
    }
  }
}

TEST(ObjectExterns, MachOObjectsOnEveryArchitecture) {
  // A Mach-O object names its externs with the format's underscore and reaches
  // data, and the addresses it takes, through GOT entries.
  for (const char *Object : {"test_extern_calls_macho_x86_64.o",
                             "test_extern_calls_macho_arm64.o"}) {
    SCOPED_TRACE(Object);
    const BinaryImage Img = loadFixture(Object);
    ASSERT_FALSE(Img.Segments.empty());
    std::set<std::string> Imports;
    for (const Import &Imp : Img.Imports)
      Imports.insert(Imp.Name);
    for (const char *Called : {"_perror", "_exit", "_fputs"})
      EXPECT_TRUE(Imports.count(Called)) << Called;
    EXPECT_FALSE(Imports.count("_maybe"));
    EXPECT_EQ(Img.findSymbol("_maybe"), nullptr);

    std::vector<va_t> Entries;
    for (const char *Name : {"_die", "_put", "_bump", "_read_tentative"}) {
      const Symbol *Sym = Img.findSymbol(Name);
      ASSERT_NE(Sym, nullptr) << Name;
      Entries.push_back(Sym->Addr);
    }
    llvm::LLVMContext Ctx;
    PipelineOptions Opts;
    Opts.EmitDumpOutput = false;
    Opts.OnlyFunctionEntries.insert(Entries.begin(), Entries.end());
    const PipelineResult Result = Pipeline().run(Img, Ctx, Opts);
    ASSERT_TRUE(Result.Success) << Result.Error;
    std::string Source;
    llvm::raw_string_ostream OS(Source);
    CEmitterOptions Options;
    Options.TheArch = Img.Arch;
    Options.Format = Img.Format;
    Options.Image = &Img;
    ASSERT_TRUE(HighCEmitter().emit(Result.HighFuncs, OS, Options));

    const std::string DieBody = definitionBody(Source, "die");
    ASSERT_FALSE(DieBody.empty()) << Source;
    EXPECT_NE(DieBody.find("perror("), std::string::npos) << DieBody;
    EXPECT_NE(DieBody.find("exit("), std::string::npos) << DieBody;
    EXPECT_EQ(DieBody.find("first("), std::string::npos) << DieBody;
    const std::string PutBody = definitionBody(Source, "put");
    ASSERT_FALSE(PutBody.empty()) << Source;
    EXPECT_NE(PutBody.find("fputs("), std::string::npos) << PutBody;
    // The data the GOT reaches prints by name or by the address it holds.
    for (const auto &[Function, Data] :
         {std::pair<const char *, const char *>{"bump", "_counter"},
          {"read_tentative", "_tentative"}}) {
      const Symbol *Target = dataSymbol(Img, Data);
      ASSERT_NE(Target, nullptr) << Data;
      const Segment *Storage = Img.getSegmentFor(Target->Addr);
      ASSERT_NE(Storage, nullptr) << Data;
      EXPECT_TRUE(Storage->isWritable()) << Data;
      const std::string Body = definitionBody(Source, Function);
      ASSERT_FALSE(Body.empty()) << Function << Source;
      EXPECT_TRUE(Body.find(llvm::StringRef(Data).drop_front().str()) !=
                      std::string::npos ||
                  mentionsAddress(Body, Target->Addr))
          << Function << Body;
    }
  }
}

TEST(ObjectExterns, LiftedModuleStoresOnlyIntoMutableGlobals) {
  // Another module defines what an extern holds and may change it: the
  // lifted module must not fold it into constant data or store into one.
  for (const ExternFixture &Fixture : kFixtures) {
    SCOPED_TRACE(Fixture.Object);
    const BinaryImage Img = loadFixture(Fixture.Object);
    const Symbol *Bump = Img.findSymbol("bump");
    ASSERT_NE(Bump, nullptr);
    llvm::LLVMContext Ctx;
    PipelineOptions Opts;
    Opts.EmitDumpOutput = false;
    Opts.NoOpt = true;
    Opts.LiftMode = true;
    Opts.OnlyFunctionEntries.insert(Bump->Addr);
    const PipelineResult Result = Pipeline().run(Img, Ctx, Opts);
    ASSERT_TRUE(Result.Success) << Result.Error;
    ASSERT_NE(Result.LlvmModule, nullptr);
    size_t Stores = 0;
    for (llvm::Function &F : *Result.LlvmModule)
      for (llvm::Instruction &I : llvm::instructions(F)) {
        const auto *Store = llvm::dyn_cast<llvm::StoreInst>(&I);
        if (!Store)
          continue;
        ++Stores;
        if (const auto *Global = llvm::dyn_cast<llvm::GlobalVariable>(
                llvm::getUnderlyingObject(Store->getPointerOperand())))
          EXPECT_FALSE(Global->isConstant())
              << F.getName().str() << " stores into constant "
              << Global->getName().str();
      }
    EXPECT_GT(Stores, 0u);
  }
}

} // namespace
