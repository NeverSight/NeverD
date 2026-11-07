//===- JumpTableLinkedI386GOTTests.cpp - GOT-relative i386 tables ---------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
//
// i386 PIC code reads a switch table of `case@GOTOFF` entries and adds each
// entry to the GOT address a get-PC sequence left in a register.  A linked
// library keeps no relocation for the table, its entries or that register,
// so its dispatch resolves only when the register holds the GOT address on
// every path.
//
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "neverd/backend/c/CEmitterOptions.h"
#include "neverd/backend/c/HighC/HighCEmitter.h"
#include "neverd/decode/Decoder.h"
#include "neverd/ir/low/CFGBuilder.h"
#include "neverd/pipeline/Pipeline.h"
#include "neverd/support/BinaryLoading.h"

#include "llvm/IR/LLVMContext.h"
#include "llvm/Support/Error.h"
#include "llvm/Support/raw_ostream.h"

#include <algorithm>
#include <filesystem>
#include <optional>
#include <string>
#include <utility>
#include <vector>

using namespace neverd;

namespace {

std::optional<BinaryImage> loadLibrary() {
  const std::filesystem::path Library =
      std::filesystem::path(TEST_OBJ_DIR) / "test_pic_switch_i386.so";
  if (!std::filesystem::exists(Library))
    return std::nullopt;
  auto ImageOrErr = loadBinary(Library);
  if (!ImageOrErr) {
    ADD_FAILURE() << llvm::toString(ImageOrErr.takeError());
    return std::nullopt;
  }
  return std::move(*ImageOrErr);
}

/// The targets of the table published at \p Name's `_dispatch` label, or
/// nothing when no table was published there.
std::optional<std::vector<va_t>> dispatchTargets(const BinaryImage &Img,
                                                 const std::string &Name) {
  const Symbol *Function = Img.findSymbol(Name);
  const Symbol *Dispatch = Img.findSymbol(Name + "_dispatch");
  if (!Function || !Dispatch) {
    ADD_FAILURE() << "no symbols for " << Name;
    return std::nullopt;
  }
  Decoder Dec;
  if (!Dec.init(Img.Arch, Img.Mode)) {
    ADD_FAILURE() << "no i386 decoder";
    return std::nullopt;
  }
  CFGBuilder Builder;
  const auto Low = Builder.build(Img, Dec, Function->Addr, Function->Name);
  for (const auto &Table : Low.JumpTables)
    if (Table.InsnAddr == Dispatch->Addr)
      return Table.Targets;
  return std::nullopt;
}

/// The addresses of \p Name's labels `_case0` to `_case<Count - 1>`.
std::vector<va_t> caseLabels(const BinaryImage &Img, const std::string &Name,
                             unsigned Count) {
  std::vector<va_t> Labels;
  for (unsigned Case = 0; Case != Count; ++Case) {
    const std::string Label = Name + "_case" + std::to_string(Case);
    const Symbol *Symbol = Img.findSymbol(Label);
    if (!Symbol) {
      ADD_FAILURE() << "no symbol " << Label;
      return {};
    }
    Labels.push_back(Symbol->Addr);
  }
  return Labels;
}

/// The HighC source of the library's function \p Name.
std::string decompile(const BinaryImage &Img, llvm::StringRef Name) {
  const Symbol *Function = Img.findSymbol(Name);
  if (!Function)
    return "no symbol " + Name.str();
  llvm::LLVMContext Ctx;
  PipelineOptions Opts;
  Opts.EmitDumpOutput = false;
  Opts.OnlyFunctionEntries = {Function->Addr};
  const PipelineResult Result = Pipeline().run(Img, Ctx, Opts);
  if (!Result.Success)
    return "pipeline failed: " + Result.Error;
  std::string Source;
  llvm::raw_string_ostream OS(Source);
  CEmitterOptions Options;
  Options.TheArch = Img.Arch;
  Options.Format = Img.Format;
  Options.Image = &Img;
  if (!HighCEmitter().emit(Result.HighFuncs, OS, Options))
    return "emission failed";
  return Source;
}

// GCC's thunk with a separate add, GCC's fused add, and clang's call/pop each
// add the entry to the GOT address, which `saved_got_switch` keeps in %ebx
// across a call on one path.  Before, the dispatch was printed as a call
// through the table entry.
TEST(LinkedI386GOTTable, EveryGetPcShapeResolvesItsSwitch) {
  const std::optional<BinaryImage> Img = loadLibrary();
  if (!Img)
    GTEST_SKIP() << "the fixture links with ld.lld";
  for (const auto &[Name, Cases] :
       {std::pair<std::string, unsigned>{"gcc_switch", 5},
        {"gcc_fused_switch", 4},
        {"clang_switch", 4},
        {"saved_got_switch", 4}}) {
    SCOPED_TRACE(Name);
    const std::optional<std::vector<va_t>> Targets =
        dispatchTargets(*Img, Name);
    ASSERT_TRUE(Targets.has_value());
    EXPECT_EQ(*Targets, caseLabels(*Img, Name, Cases));
  }
}

// GOT+4 is not the GOT address, and a register replaced on one path is not
// the GOT address on every path, so neither table names its targets.
TEST(LinkedI386GOTTable, AnchorThatIsNotTheGOTOnEveryPathStaysUnresolved) {
  const std::optional<BinaryImage> Img = loadLibrary();
  if (!Img)
    GTEST_SKIP() << "the fixture links with ld.lld";
  for (const char *Name : {"offset_anchor_switch", "clobbered_anchor_switch"}) {
    SCOPED_TRACE(Name);
    EXPECT_FALSE(dispatchTargets(*Img, Name).has_value());
  }
}

// An aligned frame spills the GOT register and reloads it for the dispatch
// after a foreign store, a PLT call and an explicit ESP restore. The spill's
// own address escapes only on the default path, which ends in abort.  The
// reload is no constant a block-local fold can see: emulated with an unknown
// %esp, it would read the second program header at 0x54.
TEST(LinkedI386GOTTable, SpilledGOTRegisterResolvesItsSwitch) {
  const std::optional<BinaryImage> Img = loadLibrary();
  if (!Img)
    GTEST_SKIP() << "the fixture links with ld.lld";
  const std::optional<std::vector<va_t>> Targets =
      dispatchTargets(*Img, "spilled_got_switch");
  ASSERT_TRUE(Targets.has_value());
  EXPECT_EQ(*Targets, caseLabels(*Img, "spilled_got_switch", 4));
}

TEST(LinkedI386GOTTable, UnknownImportCannotPreserveTheGOTSpillStack) {
  auto Img = loadLibrary();
  if (!Img)
    GTEST_SKIP() << "the fixture links with ld.lld";
  const Symbol *Begin = Img->findSymbol("spilled_got_switch_restore");
  const Symbol *End = Img->findSymbol("spilled_got_switch_restored");
  ASSERT_NE(Begin, nullptr);
  ASSERT_NE(End, nullptr);
  ASSERT_LT(Begin->Addr, End->Addr);
  ASSERT_NE(Img->readVA(Begin->Addr, End->Addr - Begin->Addr), nullptr);
  // Restore the original instruction shape without changing any table or
  // branch address. note_value's spelling grants no zero-pop contract.
  auto RemoveRestore = [&](auto &Ranges) {
    bool Patched = false;
    for (auto &Range : Ranges)
      if (Range.contains(Begin->Addr) &&
          End->Addr - Range.VA <= Range.Data.size()) {
        std::fill(Range.Data.begin() + (Begin->Addr - Range.VA),
                  Range.Data.begin() + (End->Addr - Range.VA), 0x90);
        Patched = true;
      }
    return Patched;
  };
  ASSERT_TRUE(RemoveRestore(Img->Segments));
  ASSERT_TRUE(RemoveRestore(Img->Sections));
  EXPECT_FALSE(dispatchTargets(*Img, "spilled_got_switch").has_value());
}

// A callee that receives the spill's address before the reload may replace
// the GOT address the dispatch adds.
TEST(LinkedI386GOTTable, EscapedSpillStaysUnresolved) {
  const std::optional<BinaryImage> Img = loadLibrary();
  if (!Img)
    GTEST_SKIP() << "the fixture links with ld.lld";
  EXPECT_FALSE(dispatchTargets(*Img, "escaped_got_switch").has_value());
}

TEST(LinkedI386GOTTable, ResolvedTableDecompilesAsASwitch) {
  const std::optional<BinaryImage> Img = loadLibrary();
  if (!Img)
    GTEST_SKIP() << "the fixture links with ld.lld";
  const std::string Source = decompile(*Img, "gcc_fused_switch");
  EXPECT_NE(Source.find("switch ("), std::string::npos) << Source;
  EXPECT_EQ(Source.find("(*)"), std::string::npos) << Source;
  EXPECT_EQ(Source.find("__builtin_trap"), std::string::npos) << Source;
}

} // namespace
