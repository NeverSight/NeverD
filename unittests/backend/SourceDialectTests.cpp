//===- SourceDialectTests.cpp - Emitted C spelled as Rust and Go ----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "CSyntax.h"
#include "gtest/gtest.h"

#include "neverd/backend/c/CSourceMap.h"
#include "neverd/backend/c/HighC/HighCEmitter.h"
#include "neverd/backend/c/dialect/SourceDialect.h"
#include "neverd/loader/SymbolSpelling.h"

#include "llvm/ADT/StringMap.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/Program.h"
#include "llvm/Support/raw_ostream.h"

#include <cstdlib>
#include <map>

using namespace neverd;

namespace {

SourceDialectText spell(llvm::StringRef C, SourceDialect Dialect,
                        std::vector<CSourceName> Names = {}) {
  SourceDialectOptions Opts;
  Opts.Dialect = Dialect;
  Opts.Names = Names;
  return spellInDialect(C, Opts);
}

/// The significant tokens of a C text, for comparing a text printed back.
std::vector<std::string> tokens(llvm::StringRef C) {
  std::vector<std::string> Result;
  auto Lexed = csyntax::lex(C);
  if (!Lexed) {
    llvm::consumeError(Lexed.takeError());
    return Result;
  }
  for (const csyntax::Token &T : *Lexed)
    if (T.Kind != csyntax::TokenKind::Comment &&
        T.Kind != csyntax::TokenKind::End)
      Result.push_back(T.Text.str());
  return Result;
}

TEST(SourceDialect, CxxUsesQualifiedNamesAndExactLibraryAliases) {
  const char *C = "struct basic_string;\n"
                  "int work(struct basic_string *s) { return size(s); }\n";
  std::vector<CSourceName> Names = {
      {CSourceName::Kind::Function, "work", "_ZN4Demo4workEi", 0x1000},
      {CSourceName::Kind::Function, "size",
       "_ZNKSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEE4sizeEv", 0x2000},
      {CSourceName::Kind::Type, "basic_string",
       "std::__cxx11::basic_string<char, std::char_traits<char>, "
       "std::allocator<char>>",
       std::nullopt}};
  const auto Cpp = spell(C, SourceDialect::Cpp, Names);
  EXPECT_TRUE(Cpp.Unread.empty());
  EXPECT_NE(Cpp.Text.find("Demo::work(std::string *s)"), std::string::npos)
      << Cpp.Text;
  EXPECT_NE(Cpp.Text.find("std::string::size(s)"), std::string::npos)
      << Cpp.Text;
  bool Linked = false;
  for (const auto &Name : Cpp.Names) {
    EXPECT_NE(Name.Identifier, "basic_string");
    if (Name.Address == 0x2000) {
      EXPECT_EQ(Cpp.Text.substr(Name.Begin, Name.End - Name.Begin),
                "std::string::size");
      Linked = true;
    }
  }
  EXPECT_TRUE(Linked);
  EXPECT_EQ(tokens(spell(C, SourceDialect::C, Names).Text), tokens(C));
  EXPECT_EQ(
      readableCxxTypeName(
          "std::__1::basic_string<char, OtherTraits<char>, MyAlloc<char>>"),
      "std::basic_string<char, OtherTraits<char>, MyAlloc<char>>");
  EXPECT_EQ(
      readableCxxTypeName("ATL::CStringT<wchar_t, ATL::StrTraitATL<wchar_t>>"),
      "ATL::CStringT<wchar_t, ATL::StrTraitATL<wchar_t>>");
}

TEST(SourceDialect, CxxSymbolsAreValidatedAndKeepOperatorsAndTemplates) {
  EXPECT_EQ(cxxSourceName("?push_back@?$vector@HV?$allocator@H@std@@@std@@"
                          "QEAAXAEBH@Z"),
            "std::vector<int>::push_back");
  EXPECT_EQ(cxxSourceName("_ZNSt6vectorIi7MyAllocIiEE5clearEv"),
            "std::vector<int, MyAlloc<int>>::clear");
  EXPECT_EQ(cxxSourceName(
                "_ZNSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEED1Ev"),
            "std::string::~string");
  EXPECT_TRUE(cxxSourceName("_Zebra").empty());
  EXPECT_TRUE(cxxSourceName("_ZN4core3fmt5write17h0123456789abcdefE").empty());
  EXPECT_TRUE(cxxSourceName("?nodeType@QDomNode@@QEBAHXZjunk").empty());
}

TEST(SourceDialect, NavigationKeepsStatementsSeparateInEveryDialect) {
  const llvm::StringRef C = "int next(int v);\n"
                            "int work(int v) {\n"
                            "    v = next(v);\n"
                            "    if (v) {\n"
                            "        return v;\n"
                            "    }\n"
                            "    return 0;\n"
                            "}\n";
  for (SourceDialect Dialect :
       {SourceDialect::Cpp, SourceDialect::Rust, SourceDialect::Go}) {
    SCOPED_TRACE(sourceDialectKey(Dialect).str());
    const auto Spelled = spell(
        C, Dialect,
        {{CSourceName::Kind::Function, "next", "_ZN4Demo4nextEi", 0x2000}});
    ASSERT_TRUE(Spelled.Unread.empty());
    size_t PreviousEnd = 0;
    for (llvm::StringRef Line :
         {"    v = next(v);\n", "        return v;\n", "    return 0;\n"}) {
      const size_t Begin = C.find(Line);
      ASSERT_NE(Begin, llvm::StringRef::npos);
      const auto Span = Spelled.mapExact(C, Begin, Begin + Line.size());
      ASSERT_TRUE(Span) << Line.str();
      EXPECT_GE(Span->first, PreviousEnd);
      PreviousEnd = Span->second;
      const auto Text =
          llvm::StringRef(Spelled.Text).slice(Span->first, Span->second).trim();
      EXPECT_FALSE(Text.contains('\n')) << Text.str();
      EXPECT_TRUE(Text.contains(Line.contains("next")       ? "next(v)"
                                : Line.contains("return v") ? "return v"
                                                            : "return 0"))
          << Text.str();
    }
    // Partial expressions and slices containing only part of a control-flow
    // statement cannot borrow its mapping, even if they contain whole children.
    const size_t Call = C.find("next(v)");
    EXPECT_FALSE(Spelled.mapExact(C, Call, Call + 7));
    const size_t InnerReturn = C.find("        return v;");
    const size_t FinalReturn = C.find("    return 0;");
    EXPECT_FALSE(Spelled.mapExact(C, InnerReturn, FinalReturn));
  }
}

TEST(SourceDialect, NavigationRefusesAFunctionShownAsC) {
  const llvm::StringRef C = "int work(int v) {\n"
                            "    v = v * 2;\n"
                            "    return v++ + 1;\n"
                            "}\n";
  for (SourceDialect Dialect : {SourceDialect::Rust, SourceDialect::Go}) {
    SCOPED_TRACE(sourceDialectKey(Dialect).str());
    const auto Spelled = spell(C, Dialect);
    ASSERT_EQ(Spelled.Unread.size(), 1u);
    for (llvm::StringRef Line : {"    v = v * 2;\n", "    return v++ + 1;\n"}) {
      const size_t Begin = C.find(Line);
      EXPECT_TRUE(Spelled.map(Begin, Begin + Line.size()));
      EXPECT_FALSE(Spelled.mapExact(C, Begin, Begin + Line.size()));
    }
    EXPECT_TRUE(llvm::StringRef(Spelled.Text).contains(C.trim()));
  }
}

TEST(SourceDialect, NavigationRefusesConflictingOrInvalidSpans) {
  const llvm::StringRef C = "    return 1;\n";
  SourceDialectText Spelled;
  Spelled.Text = "return 1;\nreturn 1;\n";
  Spelled.Pieces = {{4, 13, 0, 9}, {4, 13, 10, 19}};
  EXPECT_FALSE(Spelled.mapExact(C, 0, C.size()));
  Spelled.Pieces.pop_back();
  EXPECT_TRUE(Spelled.mapExact(C, 0, C.size()));
  EXPECT_FALSE(Spelled.mapExact(C, 0, 4));
  EXPECT_FALSE(Spelled.mapExact(C, 0, C.size() + 1));
  EXPECT_FALSE(Spelled.mapExact(C, C.size(), 0));
  Spelled.Pieces.front().End = Spelled.Text.size() + 1;
  EXPECT_FALSE(Spelled.mapExact(C, 0, C.size()));
}

TEST(SourceDialect, CxxProgramDefaultsIncludeCLinkageEntryPoints) {
  for (auto Runtime :
       {SourceLanguageRuntime::CxxItanium, SourceLanguageRuntime::CxxMSVC}) {
    LanguageRuntimeInfo Language;
    Language.Runtime = Runtime;
    EXPECT_EQ(
        offeredSourceDialects(Language),
        (std::vector<SourceDialect>{SourceDialect::C, SourceDialect::Cpp}));
    EXPECT_EQ(sourceDialectOfFunction("main", true, Language),
              SourceDialect::Cpp);
    EXPECT_EQ(sourceDialectOfFunction("", false, Language), SourceDialect::Cpp);
  }
  LanguageRuntimeInfo Plain;
  Plain.Runtime = SourceLanguageRuntime::C;
  EXPECT_EQ(offeredSourceDialects(Plain),
            (std::vector<SourceDialect>{SourceDialect::C}));
  EXPECT_EQ(sourceDialectOfFunction("main", true, Plain), SourceDialect::C);
}

TEST(SourceDialect, PlainCKeepsNativeExceptionCallsAndHandlerDefinitions) {
  HighFunc Function;
  Function.Entry = 0x1000;
  Function.Name = "parent";
  Function.ReturnType = NdType::makeVoid();
  HighStmt Region;
  Region.Kind = StmtKind::CxxTry;
  Region.EHRange = {0x1000, 0x1040};
  HighStmt Throw;
  Throw.Kind = StmtKind::Call;
  Throw.CallExpr = HighExpr::makeCall("_CxxThrowException", 0x3000, {});
  Throw.CallExpr->Type = NdType::makeVoid();
  Region.Body.push_back(Throw);
  HighEHClause Catch;
  Catch.Kind = HighEHClauseKind::CxxCatch;
  Catch.HandlerVA = 0x2000;
  Catch.TypeDescriptorVA = 0x4000;
  Region.EHClauses.push_back(Catch);
  HighStmt Ret;
  Ret.Kind = StmtKind::Return;
  Region.EHClauseBodies.push_back({Ret});
  Function.Body.push_back(Region);
  HighFunc Handler;
  Handler.Entry = Catch.HandlerVA;
  Handler.Name = "native_handler";
  Handler.ReturnType = NdType::makeVoid();
  Handler.Body.push_back(Ret);
  CEmitterOptions Options;
  Options.StructuredExceptionSyntax = false;
  std::string Text;
  llvm::raw_string_ostream OS(Text);
  ASSERT_TRUE(HighCEmitter().emit({Function, Handler}, OS, Options));
  EXPECT_NE(Text.find("CxxThrowException("), std::string::npos) << Text;
  // The native call also needs a declaration in the C projection.
  EXPECT_NE(
      Text.find("CxxThrowException(", Text.find("CxxThrowException(") + 1),
      std::string::npos)
      << Text;
  EXPECT_NE(Text.find("native_handler(void)"), std::string::npos) << Text;
  EXPECT_NE(Text.find("handler @ 0x2000"), std::string::npos) << Text;
  const auto Tokens = tokens(Text);
  for (const char *Keyword : {"try", "catch", "throw", "__wind", "__unwind"})
    EXPECT_EQ(std::find(Tokens.begin(), Tokens.end(), Keyword), Tokens.end())
        << Text;
  // No guessed callback ABI may replace a handler embedded in its parent.
  std::string Refused;
  llvm::raw_string_ostream RefusedOS(Refused);
  EXPECT_THROW(HighCEmitter().emit({Function}, RefusedOS, Options),
               std::invalid_argument);
}

TEST(SourceDialect, ConvertsWhatCPromotes) {
  const char *C = "#include <stdint.h>\n"
                  "uint32_t f(uint8_t a, uint8_t b, int32_t s, uint32_t u) {\n"
                  "    uint32_t x;\n"
                  "    x = a + b;\n"
                  "    if (s < u) {\n"
                  "        return (uint8_t)(a + b);\n"
                  "    }\n"
                  "    return x;\n"
                  "}\n";
  const std::string Rust = spell(C, SourceDialect::Rust).Text;
  EXPECT_NE(Rust.find("unsafe fn f(a: u8, b: u8, s: i32, u: u32) -> u32 {"),
            std::string::npos)
      << Rust;
  EXPECT_NE(Rust.find("x = a as u32 + b as u32;"), std::string::npos) << Rust;
  EXPECT_NE(Rust.find("if (s as u32) < u {"), std::string::npos) << Rust;
  EXPECT_NE(Rust.find("return (a + b) as u32;"), std::string::npos) << Rust;
  const std::string Go = spell(C, SourceDialect::Go).Text;
  EXPECT_NE(Go.find("func f(a uint8, b uint8, s int32, u uint32) uint32 {"),
            std::string::npos)
      << Go;
  EXPECT_NE(Go.find("x = uint32(a) + uint32(b)"), std::string::npos) << Go;
  EXPECT_NE(Go.find("if uint32(s) < u {"), std::string::npos) << Go;
  EXPECT_NE(Go.find("return uint32(a + b)"), std::string::npos) << Go;
}

TEST(SourceDialect, SpellsMemoryAndTruthiness) {
  const char *C = "#include <stdint.h>\n"
                  "void g(void *arg0, int64_t v) {\n"
                  "    uint64_t t;\n"
                  "    *(uint64_t *)(arg0 + 8) = 0;\n"
                  "    t = *(uint64_t *)(uintptr_t)(v + 16);\n"
                  "    if (v) {\n"
                  "        t = (__builtin_trap(), 0 /* unknown value */);\n"
                  "    }\n"
                  "    if (arg0) {\n"
                  "        return;\n"
                  "    }\n"
                  "}\n";
  const std::string Rust = spell(C, SourceDialect::Rust).Text;
  EXPECT_NE(Rust.find("*(arg0.byte_add(8) as *mut u64) = 0;"),
            std::string::npos)
      << Rust;
  EXPECT_NE(Rust.find("t = *((v + 16) as *mut u64);"), std::string::npos)
      << Rust;
  EXPECT_NE(Rust.find("if v != 0 {"), std::string::npos) << Rust;
  EXPECT_NE(Rust.find("t = abort() /* unknown value */;"), std::string::npos)
      << Rust;
  EXPECT_NE(Rust.find("if !arg0.is_null() {"), std::string::npos) << Rust;
  const std::string Go = spell(C, SourceDialect::Go).Text;
  EXPECT_NE(Go.find("*(*uint64)(unsafe.Add(arg0, 8)) = 0"), std::string::npos)
      << Go;
  EXPECT_NE(Go.find("t = *(*uint64)(v + 16)"), std::string::npos) << Go;
  EXPECT_NE(Go.find("t = trap() /* unknown value */"), std::string::npos) << Go;
  EXPECT_NE(Go.find("if arg0 != nil {"), std::string::npos) << Go;
}

TEST(SourceDialect, NamesSymbolsAsTheirLanguage) {
  const char *C = "#include <stdint.h>\n"
                  "extern int64_t core_fmt_write() "
                  "__asm__(\"_ZN4core3fmt5write17h0123456789abcdefE\");\n"
                  "/* main.main */\n"
                  "int64_t main_main(void) {\n"
                  "    return core_fmt_write(1);\n"
                  "}\n";
  std::vector<CSourceName> Names = {
      {CSourceName::Kind::Function, "core_fmt_write",
       "_ZN4core3fmt5write17h0123456789abcdefE", std::nullopt},
      {CSourceName::Kind::Function, "main_main", "main.main", 0x401000}};
  const std::string Rust = spell(C, SourceDialect::Rust, Names).Text;
  EXPECT_NE(Rust.find("return core::fmt::write(1);"), std::string::npos)
      << Rust;
  EXPECT_NE(Rust.find("unsafe fn main_main() -> i64 {"), std::string::npos)
      << Rust;
  const std::string Go = spell(C, SourceDialect::Go, Names).Text;
  EXPECT_NE(Go.find("func main.main() int64 {"), std::string::npos) << Go;
  // The comment that only repeated the name is gone.
  EXPECT_EQ(Go.find("/* main.main */"), std::string::npos) << Go;
}

TEST(SourceDialect, KeepsCBreakAndContinue) {
  const char *C = "#include <stdint.h>\n"
                  "int32_t h(int32_t v, int32_t n) {\n"
                  "    while (1) {\n"
                  "        switch (v) {\n"
                  "        case 1:\n"
                  "        case 2:\n"
                  "            if (n) {\n"
                  "                break;\n"
                  "            }\n"
                  "            n = 0;\n"
                  "            break;\n"
                  "        default:\n"
                  "            return n;\n"
                  "        }\n"
                  "        do {\n"
                  "            if (n == 3) {\n"
                  "                continue;\n"
                  "            }\n"
                  "            n = n - 1;\n"
                  "        } while (n > 0);\n"
                  "    }\n"
                  "}\n";
  const std::string Rust = spell(C, SourceDialect::Rust).Text;
  EXPECT_NE(Rust.find("'switch1: {"), std::string::npos) << Rust;
  EXPECT_NE(Rust.find("1 | 2 => {"), std::string::npos) << Rust;
  EXPECT_NE(Rust.find("break 'switch1;"), std::string::npos) << Rust;
  EXPECT_NE(Rust.find("break 'body2;"), std::string::npos) << Rust;
  EXPECT_NE(Rust.find("if !(n > 0) { break; }"), std::string::npos) << Rust;
  const std::string Go = spell(C, SourceDialect::Go).Text;
  EXPECT_NE(Go.find("case 1, 2:"), std::string::npos) << Go;
  EXPECT_NE(Go.find("for again := true; again; again = n > 0 {"),
            std::string::npos)
      << Go;
}

TEST(SourceDialect, SpellsSparseArraysAndOneDeclarationPerObject) {
  const char *C = "#include <stdint.h>\n"
                  "extern int64_t counter;\n"
                  "int64_t counter = 5;\n"
                  "unsigned char bytes[6] = { [0] = 0x40, [3] = 0x88, };\n";
  const std::string Rust = spell(C, SourceDialect::Rust).Text;
  EXPECT_NE(Rust.find("static mut bytes: [u8; 6] = [0x40, 0, 0, 0x88, 0, 0];"),
            std::string::npos)
      << Rust;
  EXPECT_EQ(Rust.find("extern \"C\""), std::string::npos) << Rust;
  const std::string Go = spell(C, SourceDialect::Go).Text;
  EXPECT_NE(Go.find("var bytes [6]uint8 = [6]uint8{0: 0x40, 3: 0x88}"),
            std::string::npos)
      << Go;
  EXPECT_EQ(Go.find("var counter int64\n"), std::string::npos) << Go;
}

TEST(SourceDialect, ShowsWhatItCannotSpellAsC) {
  const char *C = "#include <stdint.h>\n"
                  "int64_t k(int64_t v) {\n"
                  "    return v++ + 1;\n"
                  "}\n"
                  "int64_t m(int64_t v) {\n"
                  "    return v;\n"
                  "}\n";
  SourceDialectText Rust = spell(C, SourceDialect::Rust);
  ASSERT_EQ(Rust.Unread.size(), 1u) << Rust.Text;
  EXPECT_NE(Rust.Unread.front().find("increment"), std::string::npos);
  EXPECT_NE(Rust.Text.find("// NeverD: shown as C: an increment"),
            std::string::npos);
  EXPECT_NE(Rust.Text.find("    return v++ + 1;"), std::string::npos);
  EXPECT_NE(Rust.Text.find("unsafe fn m(v: i64) -> i64 {"), std::string::npos)
      << Rust.Text;
}

/// Functions in the shape HighC writes, one per conversion rule the Rust
/// view spells out.
constexpr const char *DifferentialC = R"C(#include <stdint.h>
#include <string.h>

uint32_t promote(uint8_t a, uint8_t b) {
    return a + b;
}

uint8_t narrow(uint8_t a, uint8_t b) {
    return (uint8_t)(a + b);
}

uint8_t narrow_literal(uint8_t a) {
    return (uint8_t)(a + 300);
}

int32_t mixed_compare(int32_t s, uint32_t u) {
    if (s < u) {
        return 1;
    }
    return 0;
}

uint32_t shift_promoted(uint8_t a, int32_t n) {
    return a << n;
}

int64_t arithmetic_shift(int64_t v, int32_t n) {
    return v >> n;
}

uint64_t byte_offset(void *p, int64_t i) {
    return *(uint64_t *)(p + i);
}

int64_t element_offset(int64_t *p, int64_t i) {
    return *(p + i);
}

int64_t integer_address(uint64_t a) {
    return *(int64_t *)(uintptr_t)(a + 8);
}

int32_t do_while_continue(int32_t n) {
    int32_t s;

    s = 0;
    do {
        n = n - 1;
        if (n == 3) {
            continue;
        }
        s = s + n;
    } while (n > 0);
    return s;
}

int32_t switch_break(int32_t v, int32_t n) {
    int32_t r;

    r = 0;
    switch (v) {
    case 1:
    case 2:
        if (n) {
            break;
        }
        r = 10;
        break;
    case 3:
        r = 30;
        break;
    default:
        r = -1;
        break;
    }
    return r;
}

int32_t loop_switch(int32_t n) {
    int32_t s;

    s = 0;
    while (1) {
        if (n == 0) {
            break;
        }
        switch (n & 3) {
        case 0:
            s = s + 1;
            break;
        case 1:
            n = n - 1;
            continue;
        default:
            s = s + 100;
            break;
        }
        n = n - 1;
    }
    return s;
}

int64_t unaligned_load(void *p) {
    int64_t memory_value;
    int64_t r;

    r = (__builtin_memcpy(&memory_value, (const void *)p, sizeof(memory_value)), memory_value);
    return r;
}

int32_t add_overflows(int64_t a, int64_t b) {
    return __builtin_add_overflow(a, b, &(int64_t){0});
}

int32_t choose(int32_t c, int32_t a, int32_t b) {
    return (c ? a : b);
}

int32_t logical(int32_t a, int32_t b) {
    return (a && b) + (a || b) * 2 + !a * 4;
}

uint16_t negate_unsigned(uint16_t a) {
    return -a;
}

int8_t complement(int8_t a) {
    return ~a;
}

int32_t truthiness(int64_t v, void *p) {
    int32_t r;

    r = 0;
    if (v) {
        r = r + 1;
    }
    if (p) {
        r = r + 2;
    }
    return r;
}

uint64_t widen_signed(int32_t v) {
    return (uint64_t)(int64_t)v + (uint64_t)(uint32_t)v;
}

int32_t bitwise_compare(uint32_t a, uint32_t b) {
    return (a & 0xFF) == (b & 0xFF);
}

uint32_t popcount(uint32_t a) {
    return __builtin_popcount(a);
}

int64_t divide(int64_t a, int64_t b, uint64_t c, uint64_t d) {
    return a / b + a % b + (int64_t)(c / d) + (int64_t)(c % d);
}

uint64_t multiply_high(uint64_t a, uint64_t b) {
    return (uint64_t)((unsigned __int128)a * (unsigned __int128)b >> 64);
}

int32_t byte_range(uint8_t a) {
    if (a + 1 == 256) {
        return 1;
    }
    return 0;
}

int64_t cast_chain(int32_t x) {
    return (int64_t)(uint32_t)((int32_t)x) + (int64_t)(int16_t)x;
}
)C";

/// The C side's checks: one line per call.
constexpr const char *DifferentialCMain = R"C(
#include <stdio.h>
int main(void) {
    int64_t words[4] = {0x1122334455667788, -5, 7, 9};
    printf("%lld\n", (long long)promote(200, 100));
    printf("%lld\n", (long long)narrow(200, 100));
    printf("%lld\n", (long long)narrow_literal(250));
    printf("%lld\n", (long long)mixed_compare(-1, 5));
    printf("%lld\n", (long long)mixed_compare(3, 5));
    printf("%lld\n", (long long)shift_promoted(0xF0, 20));
    printf("%lld\n", (long long)arithmetic_shift(-1024, 3));
    printf("%lld\n", (long long)byte_offset(words, 8));
    printf("%lld\n", (long long)element_offset(words, 2));
    printf("%lld\n", (long long)integer_address((uint64_t)(uintptr_t)words));
    printf("%lld\n", (long long)do_while_continue(8));
    printf("%lld\n", (long long)switch_break(2, 1));
    printf("%lld\n", (long long)switch_break(1, 0));
    printf("%lld\n", (long long)switch_break(3, 0));
    printf("%lld\n", (long long)switch_break(9, 0));
    printf("%lld\n", (long long)loop_switch(11));
    printf("%lld\n", (long long)unaligned_load((char *)words + 3));
    printf("%lld\n", (long long)add_overflows(INT64_MAX, 1));
    printf("%lld\n", (long long)add_overflows(5, 1));
    printf("%lld\n", (long long)choose(0, 4, 9));
    printf("%lld\n", (long long)logical(0, 3));
    printf("%lld\n", (long long)logical(2, 3));
    printf("%lld\n", (long long)negate_unsigned(3));
    printf("%lld\n", (long long)complement(5));
    printf("%lld\n", (long long)truthiness(0, words));
    printf("%lld\n", (long long)widen_signed(-2));
    printf("%lld\n", (long long)bitwise_compare(0x1234, 0x5534));
    printf("%lld\n", (long long)popcount(0xF00F));
    printf("%lld\n", (long long)divide(-7, 2, 7, 2));
    printf("%lld\n", (long long)multiply_high(0xFFFFFFFFFFFFFFFFull, 3));
    printf("%lld\n", (long long)byte_range(255));
    printf("%lld\n", (long long)cast_chain(-70000));
    return 0;
}
)C";

/// The Rust side's checks, the same calls.
constexpr const char *DifferentialRustMain = R"RS(
fn main() {
    unsafe {
        let mut words: [i64; 4] = [0x1122334455667788, -5, 7, 9];
        let w = words.as_mut_ptr();
        println!("{}", promote(200, 100) as i64);
        println!("{}", narrow(200, 100) as i64);
        println!("{}", narrow_literal(250) as i64);
        println!("{}", mixed_compare(-1, 5) as i64);
        println!("{}", mixed_compare(3, 5) as i64);
        println!("{}", shift_promoted(0xF0, 20) as i64);
        println!("{}", arithmetic_shift(-1024, 3) as i64);
        println!("{}", byte_offset(w as *mut c_void, 8) as i64);
        println!("{}", element_offset(w, 2) as i64);
        println!("{}", integer_address(w as u64) as i64);
        println!("{}", do_while_continue(8) as i64);
        println!("{}", switch_break(2, 1) as i64);
        println!("{}", switch_break(1, 0) as i64);
        println!("{}", switch_break(3, 0) as i64);
        println!("{}", switch_break(9, 0) as i64);
        println!("{}", loop_switch(11) as i64);
        println!("{}", unaligned_load((w as *mut u8).add(3) as *mut c_void) as i64);
        println!("{}", add_overflows(i64::MAX, 1) as i64);
        println!("{}", add_overflows(5, 1) as i64);
        println!("{}", choose(0, 4, 9) as i64);
        println!("{}", logical(0, 3) as i64);
        println!("{}", logical(2, 3) as i64);
        println!("{}", negate_unsigned(3) as i64);
        println!("{}", complement(5) as i64);
        println!("{}", truthiness(0, w as *mut c_void) as i64);
        println!("{}", widen_signed(-2) as i64);
        println!("{}", bitwise_compare(0x1234, 0x5534) as i64);
        println!("{}", popcount(0xF00F) as i64);
        println!("{}", divide(-7, 2, 7, 2) as i64);
        println!("{}", multiply_high(0xFFFFFFFFFFFFFFFF, 3) as i64);
        println!("{}", byte_range(255) as i64);
        println!("{}", cast_chain(-70000) as i64);
    }
}
)RS";

/// Runs \p Program with \p Args and gives its standard output, or an error.
llvm::Expected<std::string> run(llvm::StringRef Program,
                                llvm::ArrayRef<llvm::StringRef> Args,
                                llvm::StringRef Dir) {
  llvm::SmallString<128> Out(Dir), Err(Dir);
  llvm::sys::path::append(Out, "stdout.txt");
  llvm::sys::path::append(Err, "stderr.txt");
  std::vector<llvm::StringRef> Argv = {Program};
  Argv.insert(Argv.end(), Args.begin(), Args.end());
  const std::optional<llvm::StringRef> Redirects[] = {std::nullopt, Out.str(),
                                                      Err.str()};
  std::string Message;
  const int Status = llvm::sys::ExecuteAndWait(Program, Argv, std::nullopt,
                                               Redirects, 120, 0, &Message);
  auto Text = llvm::MemoryBuffer::getFile(Out);
  auto Errors = llvm::MemoryBuffer::getFile(Err);
  if (Status != 0)
    return llvm::createStringError(Program + " failed (" + llvm::Twine(Status) +
                                   "): " + Message + "\n" +
                                   (Errors ? (*Errors)->getBuffer() : ""));
  return Text ? (*Text)->getBuffer().str() : std::string();
}

std::string normalizeLineEndings(std::string Text) {
  for (size_t At = 0; (At = Text.find("\r\n", At)) != std::string::npos;)
    Text.erase(At, 1);
  return Text;
}

TEST(SourceDialect, CxxAlignmentUsesStandardSyntaxAndPreservesStorage) {
  const llvm::StringRef Source = R"C(
#include <stdint.h>
/* _Alignas in comments and strings is not a keyword. */
_Alignas(64) unsigned char global_storage[64];
struct aligned_record { _Alignas(32) unsigned char bytes[32]; };
_Static_assert(_Alignof(struct aligned_record) == 32, "_Alignas record");
int main(void) {
  _Alignas(16) unsigned char stack_storage[96];
  return ((uintptr_t)global_storage & 63) |
         ((uintptr_t)stack_storage & 15);
}
)C";
  const auto C = spell(Source, SourceDialect::C);
  const auto Cpp = spell(Source, SourceDialect::Cpp);
  ASSERT_TRUE(C.Unread.empty()) << C.Text;
  ASSERT_TRUE(Cpp.Unread.empty()) << Cpp.Text;
  EXPECT_EQ(tokens(C.Text), tokens(Source));
  EXPECT_NE(Cpp.Text.find("alignas(16)"), std::string::npos);
  EXPECT_NE(Cpp.Text.find("alignas(32)"), std::string::npos);
  EXPECT_NE(Cpp.Text.find("alignas(64)"), std::string::npos);
  EXPECT_NE(Cpp.Text.find("alignof(struct aligned_record)"), std::string::npos);
  EXPECT_NE(Cpp.Text.find("/* _Alignas in comments"), std::string::npos);
  EXPECT_NE(Cpp.Text.find("\"_Alignas record\""), std::string::npos);

  const std::string Clang = NEVERD_TEST_CLANG;
  if (Clang.empty())
    GTEST_SKIP() << "no clang: aligned C and C++ storage are not executed";
  llvm::SmallString<128> Dir;
  ASSERT_FALSE(llvm::sys::fs::createUniqueDirectory("neverd-alignment", Dir));
  for (bool IsCpp : {false, true}) {
    llvm::SmallString<128> File(Dir), Binary(Dir);
    llvm::sys::path::append(File, IsCpp ? "source.cpp" : "source.c");
    llvm::sys::path::append(Binary, IsCpp ? "cpp.out" : "c.out");
    std::error_code EC;
    {
      llvm::raw_fd_ostream OS(File, EC);
      ASSERT_FALSE(EC);
      OS << (IsCpp ? Cpp.Text : C.Text);
    }
    auto Built = run(Clang,
                     {IsCpp ? "-std=c++17" : "-std=c11", "-pedantic-errors",
                      "-O1", "-o", Binary, File},
                     Dir);
    ASSERT_TRUE(bool(Built)) << llvm::toString(Built.takeError());
    auto Result = run(Binary, {}, Dir);
    ASSERT_TRUE(bool(Result)) << llvm::toString(Result.takeError());
  }
  llvm::sys::fs::remove_directories(Dir);
}

/// Compiles the C functions with clang and their Rust view with rustc, runs
/// both on the same inputs and compares what they print: the Rust view must
/// compute what the C computes.  NEVERD_TEST_RUSTC or a `rustc` on PATH runs
/// it; without one the check is skipped, as the SBF source suite is.
TEST(SourceDialect, RustViewComputesWhatCComputes) {
  std::string Rustc;
  if (const char *FromEnv = std::getenv("NEVERD_TEST_RUSTC"))
    Rustc = FromEnv;
  else if (auto Found = llvm::sys::findProgramByName("rustc"))
    Rustc = *Found;
  if (Rustc.empty())
    GTEST_SKIP() << "no rustc: the Rust view's meaning is not executed";
  const std::string Clang = NEVERD_TEST_CLANG;
  if (Clang.empty())
    GTEST_SKIP() << "no clang: the C side is not executed";

  SourceDialectText Rust = spell(DifferentialC, SourceDialect::Rust);
  ASSERT_TRUE(Rust.Unread.empty()) << Rust.Text;

  llvm::SmallString<128> Dir;
  ASSERT_FALSE(llvm::sys::fs::createUniqueDirectory("neverd-dialect", Dir));
  auto Write = [&](llvm::StringRef Name, llvm::StringRef Text) {
    llvm::SmallString<128> Path(Dir);
    llvm::sys::path::append(Path, Name);
    std::error_code EC;
    llvm::raw_fd_ostream OS(Path, EC);
    EXPECT_FALSE(EC) << Path.str().str();
    OS << Text;
    return std::string(Path);
  };
  const std::string CSource =
      Write("program.c", std::string(DifferentialC) + DifferentialCMain);
  // The view's pseudo-intrinsics, given their meaning.
  const std::string RustSource =
      Write("program.rs",
            "#![allow(unused, non_snake_case, unused_mut, unused_parens, "
            "unused_unsafe, unreachable_code, unused_assignments)]\n"
            "use std::ffi::c_void;\n"
            "use std::mem::{size_of, size_of_val};\n"
            "fn abort() -> ! { std::process::abort() }\n" +
                Rust.Text + DifferentialRustMain);
  llvm::SmallString<128> CBinary(Dir), RustBinary(Dir);
  llvm::sys::path::append(CBinary, "c.out");
  llvm::sys::path::append(RustBinary, "rust.out");
  auto CBuild = run(
      Clang,
      {"-O1", "-fwrapv", "-fno-strict-aliasing", "-o", CBinary.str(), CSource},
      Dir);
  ASSERT_TRUE(bool(CBuild)) << llvm::toString(CBuild.takeError());
  auto RustBuild = run(Rustc,
                       {"--edition=2021", "-O", "-C", "overflow-checks=off",
                        "-o", RustBinary.str(), RustSource},
                       Dir);
  ASSERT_TRUE(bool(RustBuild)) << llvm::toString(RustBuild.takeError()) << "\n"
                               << Rust.Text;
  auto CResult = run(CBinary, {}, Dir);
  auto RustResult = run(RustBinary, {}, Dir);
  ASSERT_TRUE(bool(CResult)) << llvm::toString(CResult.takeError());
  ASSERT_TRUE(bool(RustResult)) << llvm::toString(RustResult.takeError());
  EXPECT_EQ(std::count(CResult->begin(), CResult->end(), '\n'), 32);
  // C's Windows text-mode stdout uses CRLF; Rust's stdout writes LF. Keep
  // the scalar results exact while comparing the same logical line endings.
  EXPECT_EQ(normalizeLineEndings(*RustResult), normalizeLineEndings(*CResult))
      << Rust.Text;
  llvm::sys::fs::remove_directories(Dir);
}

/// Execute the Go scalar subset without rewriting its generated functions or
/// supplying pseudo-intrinsic implementations. CI requires this test to run.
TEST(SourceDialect, GoViewComputesWhatCComputes) {
  std::string GoCompiler;
  if (const char *FromEnv = std::getenv("NEVERD_TEST_GO"))
    GoCompiler = FromEnv;
  else if (auto Found = llvm::sys::findProgramByName("go"))
    GoCompiler = *Found;
  if (GoCompiler.empty())
    GTEST_SKIP() << "no go: the Go view's meaning is not executed";
  const std::string Clang = NEVERD_TEST_CLANG;
  if (Clang.empty())
    GTEST_SKIP() << "no clang: the C side is not executed";

  constexpr const char *C = R"C(#include <stdint.h>
uint32_t promote(uint8_t a, uint8_t b) { return a + b; }
uint8_t narrow(uint8_t a, uint8_t b) { return (uint8_t)(a + b); }
int32_t mixed_compare(int32_t s, uint32_t u) {
    if (s < u) { return 1; }
    return 0;
}
int64_t arithmetic_shift(int64_t v, int32_t n) { return v >> n; }
int32_t loop_sum(int32_t n) {
    int32_t s;
    s = 0;
    while (n > 0) {
        s = s + n;
        n = n - 1;
    }
    return s;
}
)C";
  const char *Calls[] = {"promote(200, 100)",
                         "promote(255, 255)",
                         "narrow(200, 100)",
                         "narrow(255, 255)",
                         "mixed_compare(-1, 5)",
                         "mixed_compare(3, 5)",
                         "arithmetic_shift(-1024, 3)",
                         "arithmetic_shift(1024, 3)",
                         "loop_sum(0)",
                         "loop_sum(1)",
                         "loop_sum(8)",
                         "loop_sum(100)"};
  SourceDialectText Go = spell(C, SourceDialect::Go);
  ASSERT_TRUE(Go.Unread.empty()) << Go.Text;
  std::string CSource =
      std::string(C) + "\n#include <stdio.h>\nint main(void) {\n";
  std::string GoSource =
      "package main\nimport \"fmt\"\n" + Go.Text + "\nfunc main() {\n";
  for (const char *Call : Calls) {
    CSource += "printf(\"%lld\\n\", (long long)" + std::string(Call) + ");\n";
    GoSource += "fmt.Println(" + std::string(Call) + ")\n";
  }
  CSource += "return 0;\n}\n";
  GoSource += "}\n";

  llvm::SmallString<128> Dir;
  ASSERT_FALSE(llvm::sys::fs::createUniqueDirectory("neverd-go-dialect", Dir));
  auto Write = [&](llvm::StringRef Name, llvm::StringRef Text) {
    llvm::SmallString<128> Path(Dir);
    llvm::sys::path::append(Path, Name);
    std::error_code EC;
    llvm::raw_fd_ostream OS(Path, EC);
    EXPECT_FALSE(EC) << Path.str().str();
    OS << Text;
    return std::string(Path);
  };
  const std::string CFile = Write("program.c", CSource);
  const std::string GoFile = Write("program.go", GoSource);
  llvm::SmallString<128> CBinary(Dir), GoBinary(Dir);
  llvm::sys::path::append(CBinary, "c.out");
  llvm::sys::path::append(GoBinary, "go.out");
  auto CBuild = run(
      Clang,
      {"-O1", "-fwrapv", "-fno-strict-aliasing", "-o", CBinary.str(), CFile},
      Dir);
  ASSERT_TRUE(bool(CBuild)) << llvm::toString(CBuild.takeError());
  auto GoBuild = run(GoCompiler, {"build", "-o", GoBinary.str(), GoFile}, Dir);
  ASSERT_TRUE(bool(GoBuild)) << llvm::toString(GoBuild.takeError()) << GoSource;
  auto CResult = run(CBinary, {}, Dir);
  auto GoResult = run(GoBinary, {}, Dir);
  ASSERT_TRUE(bool(CResult)) << llvm::toString(CResult.takeError());
  ASSERT_TRUE(bool(GoResult)) << llvm::toString(GoResult.takeError());
  EXPECT_EQ(normalizeLineEndings(*CResult),
            "300\n510\n44\n254\n0\n1\n-128\n128\n0\n1\n36\n5050\n");
  EXPECT_EQ(normalizeLineEndings(*GoResult), normalizeLineEndings(*CResult))
      << Go.Text;
  llvm::sys::fs::remove_directories(Dir);
}

/// NEVERD_SOURCE_DIALECT_CORPUS names a directory of emitted C files: every
/// one must read back as the same C tokens, and the reasons for anything
/// shown as C are counted.
TEST(SourceDialect, CorpusReadsBackAsTheSameC) {
  const char *Dir = std::getenv("NEVERD_SOURCE_DIALECT_CORPUS");
  if (!Dir)
    GTEST_SKIP() << "NEVERD_SOURCE_DIALECT_CORPUS is not set";
  std::error_code EC;
  std::map<std::string, unsigned> Reasons;
  std::map<std::string, std::string> Examples;
  unsigned Files = 0, Mismatches = 0;
  size_t CBytes = 0, RustBytes = 0, GoBytes = 0;
  for (llvm::sys::fs::recursive_directory_iterator It(Dir, EC), End;
       It != End && !EC; It.increment(EC)) {
    if (!llvm::StringRef(It->path()).ends_with(".c"))
      continue;
    auto Buffer = llvm::MemoryBuffer::getFile(It->path());
    ASSERT_TRUE(Buffer) << It->path();
    const llvm::StringRef C = (*Buffer)->getBuffer();
    ++Files;
    CBytes += C.size();
    SourceDialectText Back = spell(C, SourceDialect::C);
    for (const std::string &Reason : Back.Unread) {
      // Group by the reason's words, not its offsets.
      const std::string Key =
          llvm::StringRef(Reason).split(" (byte").first.str();
      if (!Reasons[Key]++)
        Examples[Key] = It->path() + ": " + Reason;
    }
    if (Back.Unread.empty()) {
      const std::vector<std::string> Original = tokens(C);
      const std::vector<std::string> Printed = tokens(Back.Text);
      if (Printed != Original) {
        ++Mismatches;
        size_t I = 0;
        while (I < Original.size() && I < Printed.size() &&
               Original[I] == Printed[I])
          ++I;
        std::string Was, Now;
        for (size_t J = I >= 8 ? I - 8 : 0; J < I + 8; ++J) {
          if (J < Original.size())
            Was += Original[J] + " ";
          if (J < Printed.size())
            Now += Printed[J] + " ";
        }
        ADD_FAILURE() << It->path() << " reads back differently at token " << I
                      << ":\n  C:    " << Was << "\n  back: " << Now;
      }
    }
    for (SourceDialect D : {SourceDialect::Rust, SourceDialect::Go}) {
      SourceDialectText Spelled = spell(C, D);
      (D == SourceDialect::Rust ? RustBytes : GoBytes) += Spelled.Text.size();
      for (const std::string &Reason : Spelled.Unread) {
        const std::string Key = (sourceDialectKey(D) + ": " +
                                 llvm::StringRef(Reason).split(" (byte").first)
                                    .str();
        if (!Reasons[Key]++)
          Examples[Key] = It->path() + ": " + Reason;
      }
    }
  }
  ASSERT_FALSE(EC) << Dir << ": " << EC.message();
  ASSERT_GT(Files, 0u) << "no emitted C files in " << Dir;
  std::vector<std::pair<unsigned, std::string>> Sorted;
  for (auto &[Reason, Count] : Reasons)
    Sorted.push_back({Count, Reason});
  std::sort(Sorted.rbegin(), Sorted.rend());
  std::string Report;
  for (auto &[Count, Reason] : Sorted)
    Report += "  " + std::to_string(Count) + "  " + Reason + "\n      " +
              Examples[Reason] + "\n";
  std::printf("%u files, %zu C bytes, %zu Rust bytes, %zu Go bytes, %u "
              "mismatches\nshown as C:\n%s",
              Files, CBytes, RustBytes, GoBytes, Mismatches, Report.c_str());
}

/// NEVERD_SOURCE_DIALECT_FILE and NEVERD_SOURCE_DIALECT (`c`, `rust` or
/// `go`) print one file in a dialect, for looking at the output.
TEST(SourceDialect, OneFile) {
  const char *File = std::getenv("NEVERD_SOURCE_DIALECT_FILE");
  const char *Key = std::getenv("NEVERD_SOURCE_DIALECT");
  if (!File || !Key)
    GTEST_SKIP() << "NEVERD_SOURCE_DIALECT_FILE is not set";
  auto Buffer = llvm::MemoryBuffer::getFile(File);
  ASSERT_TRUE(Buffer) << File;
  auto Dialect = sourceDialectFromKey(Key);
  ASSERT_TRUE(Dialect) << Key;
  SourceDialectText Spelled = spell((*Buffer)->getBuffer(), *Dialect);
  std::printf("=====BEGIN\n%s=====END\n", Spelled.Text.c_str());
  for (const std::string &Reason : Spelled.Unread)
    std::printf("shown as C: %s\n", Reason.c_str());
}

} // namespace
