//===- MBASourceTests.cpp - Executable modular MBA source checks ----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "../NeverDLiftFixture.h"

#include <string_view>
#include <tuple>

namespace {

using SourceCase = std::tuple<const char *, const char *, bool>;

std::string readSource(const fs::path &Path) {
  std::ifstream Input(Path);
  return std::string(std::istreambuf_iterator<char>(Input), {});
}

std::string functionBody(const std::string &Source, const std::string &Name) {
  size_t Search = 0;
  while (true) {
    const auto NamePos = Source.find(Name + "(", Search);
    if (NamePos == std::string::npos)
      break;
    const auto Begin = Source.find('{', NamePos);
    const auto Semicolon = Source.find(';', NamePos);
    if (Begin != std::string::npos &&
        (Semicolon == std::string::npos || Begin < Semicolon)) {
      const auto End = Source.find("\n}", Begin);
      EXPECT_NE(End, std::string::npos) << Source;
      return End == std::string::npos ? std::string{}
                                      : Source.substr(Begin, End - Begin);
    }
    Search = NamePos + Name.size() + 1;
  }
  ADD_FAILURE() << "Missing function definition " << Name << "\n" << Source;
  return {};
}

std::string returnedExpression(const std::string &Body) {
  const auto Return = Body.find("return ");
  EXPECT_NE(Return, std::string::npos) << Body;
  if (Return == std::string::npos)
    return {};
  const auto End = Body.find(';', Return);
  EXPECT_NE(End, std::string::npos) << Body;
  if (End == std::string::npos)
    return {};
  return Body.substr(Return + 7, End - Return - 7);
}

// An unsigned arithmetic oracle keeps the expected answers independent of
// both decompilation paths and of the assembly's Boolean decomposition.
const char *executionHarness() {
  return R"(
#include <inttypes.h>
#include <stdio.h>

static uint64_t value8(uint64_t x, uint64_t y) {
  return (uint32_t)mba_sub8(x, y);
}
static uint64_t value16(uint64_t x, uint64_t y) {
  return (uint32_t)mba_sub16(x, y);
}
static uint64_t value32(uint64_t x, uint64_t y) {
  return (uint32_t)mba_sub32(x, y);
}
static uint64_t value64(uint64_t x, uint64_t y) {
  return (uint64_t)mba_sub64(x, y);
}
static uint64_t spilled_add(uint64_t x, uint64_t y) {
  return (uint64_t)mba_spilled_add(x, y);
}
static uint64_t spilled_sub(uint64_t x, uint64_t y) {
  return (uint64_t)mba_spilled_sub(x, y);
}
static uint64_t disjunction8(uint64_t x, uint64_t y) {
  return (uint32_t)mba_or8(x, y);
}
static uint64_t disjunction16(uint64_t x, uint64_t y) {
  return (uint32_t)mba_or16(x, y);
}
static uint64_t disjunction32(uint64_t x, uint64_t y) {
  return (uint32_t)mba_or32(x, y);
}
static uint64_t disjunction64(uint64_t x, uint64_t y) {
  return (uint64_t)mba_or64(x, y);
}
static uint64_t parity8(uint64_t x, uint64_t y) {
  return mba_parity8(x, y);
}
static uint64_t parity16(uint64_t x, uint64_t y) {
  return mba_parity16(x, y);
}
static uint64_t parity32(uint64_t x, uint64_t y) {
  return mba_parity32(x, y);
}
static uint64_t parity64(uint64_t x, uint64_t y) {
  return mba_parity64(x, y);
}
static uint64_t even_parity(uint64_t value) {
  unsigned ones = 0;
  for (unsigned bit = 0; bit != 8; ++bit)
    ones += (unsigned)((value >> bit) & 1);
  return (ones & 1) == 0;
}
static int check_pair(uint64_t x, uint64_t y) {
  uint64_t actual_sum = spilled_add(x, y);
  uint64_t actual_difference = spilled_sub(x, y);
  if (actual_sum != x + y || actual_difference != x - y) {
    fprintf(stderr, "spilled MBA mismatch x=%" PRIx64 " y=%" PRIx64
                    " sum=%" PRIx64 " difference=%" PRIx64 "\n",
            x, y, actual_sum, actual_difference);
    return 1;
  }
  static uint64_t (*const values[])(uint64_t, uint64_t) = {
    value8, value16, value32, value64
  };
  static uint64_t (*const disjunctions[])(uint64_t, uint64_t) = {
    disjunction8, disjunction16, disjunction32, disjunction64
  };
  static uint64_t (*const parities[])(uint64_t, uint64_t) = {
    parity8, parity16, parity32, parity64
  };
  static const uint64_t masks[] = {
    UINT64_C(0xff), UINT64_C(0xffff), UINT64_C(0xffffffff), UINT64_MAX
  };
  for (unsigned i = 0; i != 4; ++i) {
    uint64_t expected = (x - y) & masks[i];
    uint64_t actual = values[i](x, y);
    uint64_t parity = parities[i](x, y);
    if (actual != expected || parity != even_parity(expected)) {
      fprintf(stderr, "width=%u x=%" PRIx64 " y=%" PRIx64
              " value=%" PRIx64 " expected=%" PRIx64 " parity=%" PRIu64 "\n",
              8u << i, x, y, actual, expected, parity);
      return 1;
    }
    uint64_t expected_or = (x | y) & masks[i];
    uint64_t actual_or = disjunctions[i](x, y);
    if (actual_or != expected_or) {
      fprintf(stderr, "OR width=%u x=%" PRIx64 " y=%" PRIx64
              " value=%" PRIx64 " expected=%" PRIx64 "\n",
              8u << i, x, y, actual_or, expected_or);
      return 1;
    }
  }
  uint64_t storage[] = {UINT64_C(0x13579bdf2468ace0), ~(x - y),
                        UINT64_C(0xfedcba9876543210)};
  uint64_t returned = (uint64_t)mba_store64(x, y, &storage[1]);
  if (returned != x - y || storage[1] != x - y ||
      storage[0] != UINT64_C(0x13579bdf2468ace0) ||
      storage[2] != UINT64_C(0xfedcba9876543210)) {
    fprintf(stderr, "store/return mismatch x=%" PRIx64 " y=%" PRIx64 "\n", x, y);
    return 1;
  }
  return 0;
}
int main(void) {
  // Exhaust every pair of bytes, including negative low-byte differences.
  for (uint64_t x = 0; x != 256; ++x) {
    if ((uint64_t)mba_full_product8(x) != x * UINT64_C(255)) {
      fprintf(stderr, "full product incorrectly narrowed at x=%" PRIu64 "\n", x);
      return 1;
    }
    for (uint64_t y = 0; y != 256; ++y)
      if (check_pair(x, y))
        return 1;
  }
  static const uint64_t edges[] = {
    0, 1, 2, 0x7f, 0x80, 0xff, 0x100, 0x7fff, 0x8000, 0xffff, 0x10000,
    UINT64_C(0x7fffffff), UINT64_C(0x80000000), UINT64_C(0xffffffff),
    UINT64_C(0x100000000), UINT64_C(0x7fffffffffffffff),
    UINT64_C(0x8000000000000000), UINT64_MAX - 1, UINT64_MAX,
    UINT64_C(0xaaaaaaaaaaaaaaaa), UINT64_C(0x5555555555555555)
  };
  for (unsigned i = 0; i != sizeof(edges) / sizeof(edges[0]); ++i)
    for (unsigned j = 0; j != sizeof(edges) / sizeof(edges[0]); ++j)
      if (check_pair(edges[i], edges[j]))
        return 1;
  uint64_t state = UINT64_C(0x92d68ca2f53b17e9);
  for (unsigned i = 0; i != 4096; ++i) {
    state = state * UINT64_C(6364136223846793005) + 1;
    uint64_t x = state;
    state = state * UINT64_C(6364136223846793005) + 1;
    if (check_pair(x, state))
      return 1;
  }
  return 0;
}
)";
}

bool containsResidualMBA(const std::string &AST) {
  // Inspect expression kinds rather than punctuation: address-of and
  // dereference are unary operators, and pointer casts are type expressions.
  static const std::regex Binary(
      R"(\b(?:BinaryOperator|CompoundAssignOperator)\b[^\r\n]* '(?:\^|&|\||<<|>>|\*|/|%)=?')");
  static const std::regex Complement(R"(\bUnaryOperator\b[^\r\n]* prefix '~')");
  return std::regex_search(AST, Binary) || std::regex_search(AST, Complement);
}

class MBAExecutableSourceTest : public NeverDLiftTest {
protected:
  std::string functionAST(const fs::path &Source, const std::string &Name) {
    // The scalar fixtures need no SIMD declarations. Leave the original C
    // body intact while allowing LLVMC's blanket x86 include on other hosts.
    std::ofstream(tmpFile("immintrin.h")).close();
    const auto Parsed = exec(NEVERD_TEST_CLANG,
                             {"-std=c11", "-fsyntax-only",
                              "-Werror=implicit-function-declaration", "-I",
                              tmp().string(), "-Xclang", "-ast-dump", "-Xclang",
                              "-ast-dump-filter=" + Name, Source.string()});
    EXPECT_TRUE(Parsed.ok()) << Parsed.err;
    EXPECT_NE(Parsed.out.find("FunctionDecl"), std::string::npos)
        << Name << "\n"
        << Parsed.out;
    EXPECT_NE(Parsed.out.find("CompoundStmt"), std::string::npos)
        << Name << "\n"
        << Parsed.out;
    EXPECT_NE(Parsed.out.find("ReturnStmt"), std::string::npos) << Name << "\n"
                                                                << Parsed.out;
    return Parsed.out;
  }

  void expectNoResidualMBA(const fs::path &Source, const std::string &Name) {
    const auto AST = functionAST(Source, Name);
    EXPECT_FALSE(containsResidualMBA(AST)) << Name << "\n" << AST;
  }
};

TEST_F(MBAExecutableSourceTest,
       WholeFunctionCheckFindsHiddenMBAAndAllowsAddressExpressions) {
  if (!hasCrossTargetClang())
    GTEST_SKIP() << "C expression checks require clang";
  const auto Source = tmpFile("expression-shapes.c");
  std::ofstream(Source) << R"(
#include <stdint.h>

uint32_t hidden_mba(uint32_t x, uint32_t y) {
  uint32_t parity = x ^ y;
  uint32_t carry = (x & y) << 1;
  return parity + carry;
}

uint32_t compound_mba(uint32_t x, uint32_t y) {
  x ^= y;
  return x + y;
}

uint32_t addressed_add(uint32_t x, uint32_t y) {
  uint32_t local = x;
  uintptr_t address = (uintptr_t)&local;
  uint32_t *pointer = (uint32_t *)address;
  return *pointer + y;
}
)";
  EXPECT_TRUE(containsResidualMBA(functionAST(Source, "hidden_mba")));
  EXPECT_TRUE(containsResidualMBA(functionAST(Source, "compound_mba")));
  EXPECT_FALSE(containsResidualMBA(functionAST(Source, "addressed_add")));
}

TEST(MBASourceBodyTest, FindsDefinitionAfterPrototypeAndCall) {
  const std::string Source = R"(
int target(int);
int unrelated(int x) {
  return x + 1;
}
int caller(void) {
  return target(1);
}
int target(int x) {
  return x ^ 1;
}
)";
  const std::string Body = functionBody(Source, "target");
  EXPECT_NE(Body.find("x ^ 1"), std::string::npos) << Body;
  EXPECT_EQ(Body.find("x + 1"), std::string::npos) << Body;
}

class MBASourceTest : public MBAExecutableSourceTest,
                      public ::testing::WithParamInterface<SourceCase> {};

TEST_P(MBASourceTest, RemovesModularMBAAndPreservesExecutableBehavior) {
  if (!hasCrossTargetClang())
    GTEST_SKIP() << "cross-target MBA fixture requires clang";
  const auto &[Format, Triple, LLVM] = GetParam();
  SCOPED_TRACE(std::string(Format) + (LLVM ? " LLVMC" : " HighC"));
  const auto Object = tmpFile("modular-widths.o");
  const auto Compiled =
      exec(NEVERD_TEST_CLANG,
           {"-target", Triple, "-c",
            (fs::path(TEST_SOURCE_DIR) / "x86_64/test_mba_modular_widths.S")
                .string(),
            "-o", Object.string()});
  ASSERT_TRUE(Compiled.ok()) << Compiled.err;

  const auto Output = tmpFile("modular-widths.c");
  std::vector<std::string> Arguments{"decompile"};
  if (LLVM)
    Arguments.push_back("--llvm");
  Arguments.insert(Arguments.end(), {"-o", Output.string(), Object.string()});
  const auto Decompiled = exec(ndBin(), Arguments);
  ASSERT_TRUE(Decompiled.ok()) << Decompiled.err;
  const std::string Source = readSource(Output);
  ASSERT_FALSE(Source.empty());
  for (unsigned Bits : {8u, 16u, 32u, 64u}) {
    const auto Value = functionBody(Source, "mba_sub" + std::to_string(Bits));
    // Casts may remain to express the ABI. Both x - y and -y + x are basic
    // arithmetic; neither needs the original product or Boolean expansion.
    for (char Operator : {'*', '&', '|', '^', '~'})
      EXPECT_EQ(Value.find(Operator), std::string::npos) << Value;
    EXPECT_EQ(std::count(Value.begin(), Value.end(), '-'), 1) << Value;
    EXPECT_LE(std::count(Value.begin(), Value.end(), '+'), 1) << Value;
    EXPECT_EQ(Value.find("128"), std::string::npos) << Value;

    const auto Disjunction =
        functionBody(Source, "mba_or" + std::to_string(Bits));
    for (char Operator : {'*', '+', '-', '&', '^', '~'})
      EXPECT_EQ(Disjunction.find(Operator), std::string::npos) << Disjunction;
    EXPECT_EQ(std::count(Disjunction.begin(), Disjunction.end(), '|'), 1)
        << Disjunction;

    const auto Parity =
        functionBody(Source, "mba_parity" + std::to_string(Bits));
    // Parity legitimately uses masks, XOR, or the complementary byte
    // difference: complementing eight bits preserves even/odd parity.
    for (char Operator : {'*', '|'})
      EXPECT_EQ(Parity.find(Operator), std::string::npos) << Parity;
    EXPECT_EQ(Parity.find("128"), std::string::npos) << Parity;
  }
  const auto Store = functionBody(Source, "mba_store64");
  for (char Operator : {'|', '^', '~'})
    EXPECT_EQ(Store.find(Operator), std::string::npos) << Store;
  EXPECT_EQ(std::count(Store.begin(), Store.end(), '-'), 1) << Store;
  EXPECT_LE(std::count(Store.begin(), Store.end(), '+'), 1) << Store;
  EXPECT_EQ(Store.find("128"), std::string::npos) << Store;
  EXPECT_FALSE(functionBody(Source, "mba_full_product8").empty());

  expectNoResidualMBA(Output, "mba_spilled_add");
  expectNoResidualMBA(Output, "mba_spilled_sub");
  const auto SpilledAdd =
      returnedExpression(functionBody(Source, "mba_spilled_add"));
  for (char Operator : {'^', '&', '|', '~'})
    EXPECT_EQ(SpilledAdd.find(Operator), std::string::npos) << SpilledAdd;
  EXPECT_EQ(SpilledAdd.find("<<"), std::string::npos) << SpilledAdd;
  EXPECT_EQ(SpilledAdd.find(">>"), std::string::npos) << SpilledAdd;
  EXPECT_EQ(std::count(SpilledAdd.begin(), SpilledAdd.end(), '+'), 1)
      << SpilledAdd;
  EXPECT_EQ(SpilledAdd.find('-'), std::string::npos) << SpilledAdd;

  const auto SpilledSub =
      returnedExpression(functionBody(Source, "mba_spilled_sub"));
  for (char Operator : {'^', '&', '|', '~'})
    EXPECT_EQ(SpilledSub.find(Operator), std::string::npos) << SpilledSub;
  EXPECT_EQ(SpilledSub.find("<<"), std::string::npos) << SpilledSub;
  EXPECT_EQ(SpilledSub.find(">>"), std::string::npos) << SpilledSub;
  EXPECT_EQ(std::count(SpilledSub.begin(), SpilledSub.end(), '-'), 1)
      << SpilledSub;
  EXPECT_EQ(SpilledSub.find('+'), std::string::npos) << SpilledSub;

  // This scalar-only fixture needs no SIMD declarations. LLVMC's blanket
  // x86 header otherwise prevents recompiling the source on non-x86 hosts.
  std::ofstream(tmpFile("immintrin.h")).close();
  const auto Harness = tmpFile("execute.c");
  std::ofstream(Harness) << Source << executionHarness();
  for (const char *Optimization : {"-O0", "-O2"}) {
    SCOPED_TRACE(Optimization);
    const auto Executable = tmpFile("execute");
    const auto Recompiled =
        exec(NEVERD_TEST_CLANG,
             {"-std=c11", Optimization, "-fno-inline", "-Werror=return-type",
              "-Werror=implicit-function-declaration", "-fsanitize=undefined",
              "-fsanitize-trap=undefined", "-I", tmp().string(),
              Harness.string(), "-o", Executable.string()});
    ASSERT_TRUE(Recompiled.ok()) << Recompiled.err << "\n" << Source;
    const auto Ran = exec(Executable.string(), {});
    EXPECT_TRUE(Ran.ok()) << Ran.err << "\n" << Source;
  }
}

INSTANTIATE_TEST_SUITE_P(
    NativeFormats, MBASourceTest,
    ::testing::Values(SourceCase{"ELF", "x86_64-linux-gnu", false},
                      SourceCase{"ELF", "x86_64-linux-gnu", true},
                      SourceCase{"COFF", "x86_64-pc-windows-msvc", false},
                      SourceCase{"COFF", "x86_64-pc-windows-msvc", true},
                      SourceCase{"MachO", "x86_64-apple-macos11", false},
                      SourceCase{"MachO", "x86_64-apple-macos11", true}),
    [](const ::testing::TestParamInfo<SourceCase> &Info) {
      return std::string(std::get<0>(Info.param)) +
             (std::get<2>(Info.param) ? "LLVMC" : "HighC");
    });

struct FrameSourceCase {
  const char *Name;
  const char *Triple;
  const char *Fixture;
  unsigned WordBits;
  bool Thumb;
  bool LLVM;
};

// Execute the emitted C on the host: the fixed-width unsigned oracle remains
// valid even when the fixture's machine width differs from the host's width.
const char *frameExecutionHarness() {
  return R"(
#include <inttypes.h>
#include <stdio.h>

static int check_pair(uint64_t x, uint64_t y) {
  const uint64_t mask = MBA_WORD_BITS == 32 ? UINT32_MAX : UINT64_MAX;
  x &= mask;
  y &= mask;
  const uint64_t sum = (uint64_t)mba_spilled_add(x, y) & mask;
  const uint64_t difference = (uint64_t)mba_spilled_sub(x, y) & mask;
  if (sum != ((x + y) & mask) || difference != ((x - y) & mask)) {
    fprintf(stderr, "width=%u x=%" PRIx64 " y=%" PRIx64
                    " sum=%" PRIx64 " difference=%" PRIx64 "\n",
            MBA_WORD_BITS, x, y, sum, difference);
    return 1;
  }
  return 0;
}

int main(void) {
  for (uint64_t x = 0; x != 256; ++x)
    for (uint64_t y = 0; y != 256; ++y)
      if (check_pair(x, y))
        return 1;
  static const uint64_t edges[] = {
    0, 1, 2, 0x7f, 0x80, 0xff, 0x100, 0x7fff, 0x8000, 0xffff, 0x10000,
    UINT64_C(0x7fffffff), UINT64_C(0x80000000), UINT64_C(0xffffffff),
    UINT64_C(0x100000000), UINT64_C(0x7fffffffffffffff),
    UINT64_C(0x8000000000000000), UINT64_MAX - 1, UINT64_MAX,
    UINT64_C(0xaaaaaaaaaaaaaaaa), UINT64_C(0x5555555555555555)
  };
  for (unsigned i = 0; i != sizeof(edges) / sizeof(edges[0]); ++i)
    for (unsigned j = 0; j != sizeof(edges) / sizeof(edges[0]); ++j)
      if (check_pair(edges[i], edges[j]))
        return 1;
  uint64_t state = UINT64_C(0x92d68ca2f53b17e9);
  for (unsigned i = 0; i != 4096; ++i) {
    state = state * UINT64_C(6364136223846793005) + 1;
    const uint64_t x = state;
    state = state * UINT64_C(6364136223846793005) + 1;
    if (check_pair(x, state))
      return 1;
  }
  return 0;
}
)";
}

class MBAFrameSourceTest
    : public MBAExecutableSourceTest,
      public ::testing::WithParamInterface<FrameSourceCase> {};

TEST_P(MBAFrameSourceTest, RemovesSpilledMBAAndPreservesExecutableBehavior) {
  if (!hasCrossTargetClang())
    GTEST_SKIP() << "cross-target MBA fixture requires clang";
  const auto &Case = GetParam();
  SCOPED_TRACE(std::string(Case.Name) + (Case.LLVM ? " LLVMC" : " HighC"));
  const auto Object = tmpFile("frame-spills.o");
  std::vector<std::string> CompileArguments{"-target", Case.Triple};
  if (Case.Thumb)
    CompileArguments.push_back("-mthumb");
  CompileArguments.insert(CompileArguments.end(),
                          {"-c",
                           (fs::path(TEST_SOURCE_DIR) / Case.Fixture).string(),
                           "-o", Object.string()});
  const auto Compiled = exec(NEVERD_TEST_CLANG, CompileArguments);
  ASSERT_TRUE(Compiled.ok()) << Compiled.err;

  const auto Output = tmpFile("frame-spills.c");
  std::vector<std::string> Arguments{"decompile"};
  if (Case.LLVM)
    Arguments.push_back("--llvm");
  Arguments.insert(Arguments.end(), {"-o", Output.string(), Object.string()});
  const auto Decompiled = exec(ndBin(), Arguments);
  ASSERT_TRUE(Decompiled.ok()) << Decompiled.err;
  const auto Source = readSource(Output);
  ASSERT_FALSE(Source.empty());
  std::string AddName = "mba_spilled_add";
  std::string SubName = "mba_spilled_sub";
  if (Source.find(AddName + "(") == std::string::npos ||
      Source.find(SubName + "(") == std::string::npos) {
    // Object formats can keep more than one symbol at a function entry. Bind
    // the harness to the emitted alias only after authenticating its address.
    const auto Listed = exec(ndBin(), {"funcs", "--json", Object.string()});
    ASSERT_TRUE(Listed.ok()) << Listed.err;
    const std::regex Definition(
        R"json(\{"addr":"([^"]+)","name":"([^"]+)","size":)json");
    std::vector<std::pair<std::string, std::string>> Symbols;
    for (std::sregex_iterator
             It(Listed.out.begin(), Listed.out.end(), Definition),
         End;
         It != End; ++It)
      Symbols.emplace_back((*It)[1].str(), (*It)[2].str());
    auto ResolveAlias = [&](const std::string &Name) {
      const auto Requested =
          std::find_if(Symbols.begin(), Symbols.end(), [&](const auto &Symbol) {
            return Symbol.second == Name || Symbol.second == "_" + Name;
          });
      EXPECT_NE(Requested, Symbols.end()) << Name << "\n" << Listed.out;
      if (Requested == Symbols.end())
        return Name;
      for (const auto &[Address, RawName] : Symbols) {
        const auto Candidate =
            RawName.starts_with("_") ? RawName.substr(1) : RawName;
        if (Address == Requested->first &&
            Source.find(Candidate + "(") != std::string::npos)
          return Candidate;
      }
      ADD_FAILURE() << "No emitted alias for " << Name << "\n" << Source;
      return Name;
    };
    AddName = ResolveAlias(AddName);
    SubName = ResolveAlias(SubName);
  }
  for (const bool Subtract : {false, true}) {
    expectNoResidualMBA(Output, Subtract ? SubName : AddName);
    const auto Expression =
        returnedExpression(functionBody(Source, Subtract ? SubName : AddName));
    for (const char Operator : {'^', '&', '|', '~', '*'})
      EXPECT_EQ(Expression.find(Operator), std::string::npos) << Expression;
    EXPECT_EQ(Expression.find("<<"), std::string::npos) << Expression;
    EXPECT_EQ(Expression.find(">>"), std::string::npos) << Expression;
    EXPECT_EQ(
        std::count(Expression.begin(), Expression.end(), Subtract ? '-' : '+'),
        1)
        << Expression;
    EXPECT_EQ(Expression.find(Subtract ? '+' : '-'), std::string::npos)
        << Expression;
  }

  std::ofstream(tmpFile("immintrin.h")).close();
  const auto Harness = tmpFile("execute-frame.c");
  std::ofstream(Harness) << "#define MBA_WORD_BITS " << Case.WordBits << "\n"
                         << Source << "\n#define mba_spilled_add " << AddName
                         << "\n#define mba_spilled_sub " << SubName << "\n"
                         << frameExecutionHarness();
  for (const char *Optimization : {"-O0", "-O2"}) {
    SCOPED_TRACE(Optimization);
    const auto Executable = tmpFile("execute-frame");
    const auto Recompiled =
        exec(NEVERD_TEST_CLANG,
             {"-std=c11", Optimization, "-fno-inline", "-Werror=return-type",
              "-Werror=implicit-function-declaration", "-fsanitize=undefined",
              "-fsanitize-trap=undefined", "-I", tmp().string(),
              Harness.string(), "-o", Executable.string()});
    ASSERT_TRUE(Recompiled.ok()) << Recompiled.err << "\n" << Source;
    const auto Ran = exec(Executable.string(), {});
    EXPECT_TRUE(Ran.ok()) << Ran.err << "\n" << Source;
  }
}

INSTANTIATE_TEST_SUITE_P(
    CrossArchitecture, MBAFrameSourceTest,
    ::testing::Values(
        FrameSourceCase{"X86ELF", "i386-linux-gnu",
                        "x86_32/test_mba_frame_spills.S", 32, false, false},
        FrameSourceCase{"X86ELF", "i386-linux-gnu",
                        "x86_32/test_mba_frame_spills.S", 32, false, true},
        FrameSourceCase{"X86COFF", "i386-pc-windows-msvc",
                        "x86_32/test_mba_frame_spills.S", 32, false, false},
        FrameSourceCase{"X86COFF", "i386-pc-windows-msvc",
                        "x86_32/test_mba_frame_spills.S", 32, false, true},
        FrameSourceCase{"X86MachO", "i386-apple-macos10.7",
                        "x86_32/test_mba_frame_spills.S", 32, false, false},
        FrameSourceCase{"X86MachO", "i386-apple-macos10.7",
                        "x86_32/test_mba_frame_spills.S", 32, false, true},
        FrameSourceCase{"ARM32", "armv7-linux-gnueabi",
                        "arm32/test_mba_frame_spills.S", 32, false, false},
        FrameSourceCase{"ARM32", "armv7-linux-gnueabi",
                        "arm32/test_mba_frame_spills.S", 32, false, true},
        FrameSourceCase{"Thumb2", "armv7-linux-gnueabi",
                        "arm32/test_mba_frame_spills.S", 32, true, false},
        FrameSourceCase{"Thumb2", "armv7-linux-gnueabi",
                        "arm32/test_mba_frame_spills.S", 32, true, true},
        FrameSourceCase{"Thumb1", "thumbv6m-none-eabi",
                        "arm32/test_mba_frame_spills_thumb1.S", 32, true,
                        false},
        FrameSourceCase{"Thumb1", "thumbv6m-none-eabi",
                        "arm32/test_mba_frame_spills_thumb1.S", 32, true, true},
        FrameSourceCase{"AArch64ELF", "aarch64-linux-gnu",
                        "aarch64/test_mba_frame_spills.S", 64, false, false},
        FrameSourceCase{"AArch64ELF", "aarch64-linux-gnu",
                        "aarch64/test_mba_frame_spills.S", 64, false, true},
        FrameSourceCase{"AArch64COFF", "aarch64-pc-windows-msvc",
                        "aarch64/test_mba_frame_spills.S", 64, false, false},
        FrameSourceCase{"AArch64COFF", "aarch64-pc-windows-msvc",
                        "aarch64/test_mba_frame_spills.S", 64, false, true},
        FrameSourceCase{"AArch64MachO", "aarch64-apple-macos11",
                        "aarch64/test_mba_frame_spills.S", 64, false, false},
        FrameSourceCase{"AArch64MachO", "aarch64-apple-macos11",
                        "aarch64/test_mba_frame_spills.S", 64, false, true}),
    [](const ::testing::TestParamInfo<FrameSourceCase> &Info) {
      return std::string(Info.param.Name) +
             (Info.param.LLVM ? "LLVMC" : "HighC");
    });

struct NestedSourceCase {
  const char *Name;
  const char *Triple;
  unsigned WordBits;
  bool Thumb;
  bool LLVM;
};

const char *nestedExecutionHarness() {
  return R"(
#include <inttypes.h>
#include <stdio.h>

static int check_pair(uint64_t x, uint64_t y, uint64_t z) {
  uint32_t a = (uint32_t)x, b = (uint32_t)y, c = (uint32_t)z;
  if ((uint32_t)mba_nested_add(a, b) != a + b ||
      (uint32_t)mba_nested_sub(a, b) != a - b ||
      (uint32_t)mba_nested_xor(a, b) != (a ^ b) ||
      (uint32_t)mba_nested_or(a, b) != (a | b) ||
      (uint32_t)mba_three_input(a, b, c) != a + b + c) {
    fprintf(stderr, "nested MBA mismatch x=%" PRIx64 " y=%" PRIx64
                    " z=%" PRIx64 "\n", x, y, z);
    return 1;
  }
#if NESTED_WORD_BITS == 64
  if ((uint64_t)mba_wide_add(x, y) != x + y)
    return 1;
#endif
  return 0;
}

int main(void) {
  for (uint64_t x = 0; x != 256; ++x)
    for (uint64_t y = 0; y != 256; ++y)
      if (check_pair(x, y, x * 73 + y * 19))
        return 1;
  static const uint64_t edges[] = {
    0, 1, 0x7fffffff, 0x80000000, 0xffffffff,
    UINT64_C(0x100000000), UINT64_C(0x7fffffffffffffff),
    UINT64_C(0x8000000000000000), UINT64_MAX
  };
  for (unsigned i = 0; i != sizeof(edges) / sizeof(edges[0]); ++i)
    for (unsigned j = 0; j != sizeof(edges) / sizeof(edges[0]); ++j)
      if (check_pair(edges[i], edges[j], edges[(i + j) % 9]))
        return 1;
  uint64_t state = UINT64_C(0x6a09e667f3bcc909);
  for (unsigned i = 0; i != 4096; ++i) {
    state = state * UINT64_C(6364136223846793005) + 1;
    uint64_t x = state;
    state = state * UINT64_C(6364136223846793005) + 1;
    uint64_t y = state;
    state = state * UINT64_C(6364136223846793005) + 1;
    if (check_pair(x, y, state))
      return 1;
  }
  return 0;
}
)";
}

class MBANestedSourceTest
    : public MBAExecutableSourceTest,
      public ::testing::WithParamInterface<NestedSourceCase> {};

TEST_P(MBANestedSourceTest, RecoversSpilledNestedExpressionsInBothCRoutes) {
  if (!hasCrossTargetClang())
    GTEST_SKIP() << "cross-target MBA fixture requires clang";
  const auto &Case = GetParam();
  SCOPED_TRACE(std::string(Case.Name) + (Case.LLVM ? " LLVMC" : " HighC"));
  const auto Object = tmpFile("nested-mba.o");
  std::vector<std::string> CompileArguments{
      "-target",        Case.Triple, "-O0",
      "-ffreestanding", "-nostdinc", "-fno-stack-protector"};
  if (Case.Thumb)
    CompileArguments.push_back("-mthumb");
  CompileArguments.insert(
      CompileArguments.end(),
      {"-c", (fs::path(TEST_SOURCE_DIR) / "core/test_mba_nested.c").string(),
       "-o", Object.string()});
  const auto Compiled = exec(NEVERD_TEST_CLANG, CompileArguments);
  ASSERT_TRUE(Compiled.ok()) << Compiled.err;

  const auto Output = tmpFile("nested-mba.c");
  std::vector<std::string> Arguments{"decompile", "--no-debug"};
  if (Case.LLVM)
    Arguments.push_back("--llvm");
  Arguments.insert(Arguments.end(), {"-o", Output.string(), Object.string()});
  const auto Decompiled = exec(ndBin(), Arguments);
  ASSERT_TRUE(Decompiled.ok()) << Decompiled.err;
  const std::string Source = readSource(Output);
  ASSERT_FALSE(Source.empty());
  for (const char *Name :
       {"mba_nested_add", "mba_nested_sub", "mba_three_input"})
    expectNoResidualMBA(Output, Name);
  if (Case.WordBits == 64)
    expectNoResidualMBA(Output, "mba_wide_add");
  for (const char *Name : {"mba_nested_xor", "mba_nested_or"}) {
    const auto Body = functionBody(Source, Name);
    EXPECT_EQ(std::count(Body.begin(), Body.end(),
                         std::string_view(Name).ends_with("xor") ? '^' : '|'),
              1)
        << Body;
  }

  std::ofstream(tmpFile("immintrin.h")).close();
  const auto Harness = tmpFile("nested-execute.c");
  std::ofstream(Harness) << "#define NESTED_WORD_BITS " << Case.WordBits << "\n"
                         << Source << nestedExecutionHarness();
  for (const char *Optimization : {"-O0", "-O2"}) {
    SCOPED_TRACE(Optimization);
    const auto Executable = tmpFile("nested-execute");
    const auto Recompiled =
        exec(NEVERD_TEST_CLANG,
             {"-std=c11", Optimization, "-fno-inline", "-Werror=return-type",
              "-Werror=implicit-function-declaration", "-fsanitize=undefined",
              "-fsanitize-trap=undefined", "-I", tmp().string(),
              Harness.string(), "-o", Executable.string()});
    ASSERT_TRUE(Recompiled.ok()) << Recompiled.err << "\n" << Source;
    const auto Ran = exec(Executable.string(), {});
    EXPECT_TRUE(Ran.ok()) << Ran.err << "\n" << Source;
  }
}

INSTANTIATE_TEST_SUITE_P(
    CrossArchitecture, MBANestedSourceTest,
    ::testing::Values(
        NestedSourceCase{"X64", "x86_64-linux-gnu", 64, false, false},
        NestedSourceCase{"X64", "x86_64-linux-gnu", 64, false, true},
        NestedSourceCase{"X86", "i386-linux-gnu", 32, false, false},
        NestedSourceCase{"X86", "i386-linux-gnu", 32, false, true},
        NestedSourceCase{"ARM32", "armv7-linux-gnueabihf", 32, false, false},
        NestedSourceCase{"ARM32", "armv7-linux-gnueabihf", 32, false, true},
        NestedSourceCase{"Thumb2", "armv7-linux-gnueabihf", 32, true, false},
        NestedSourceCase{"Thumb2", "armv7-linux-gnueabihf", 32, true, true},
        NestedSourceCase{"Thumb1", "thumbv6m-none-eabi", 32, true, false},
        NestedSourceCase{"Thumb1", "thumbv6m-none-eabi", 32, true, true},
        NestedSourceCase{"AArch64", "aarch64-linux-gnu", 64, false, false},
        NestedSourceCase{"AArch64", "aarch64-linux-gnu", 64, false, true}),
    [](const ::testing::TestParamInfo<NestedSourceCase> &Info) {
      return std::string(Info.param.Name) +
             (Info.param.LLVM ? "LLVMC" : "HighC");
    });

struct RegisterPairCase {
  const char *Name;
  const char *Triple;
  bool Thumb;
  bool LLVM;
  bool PIC = false;
};

class MBARegisterPairSourceTest
    : public NeverDLiftTest,
      public ::testing::WithParamInterface<RegisterPairCase> {};

TEST_P(MBARegisterPairSourceTest, PreservesObservedWideReturnAndFold) {
  if (!hasCrossTargetClang())
    GTEST_SKIP() << "cross-target MBA fixture requires clang";
  const auto &Case = GetParam();
  SCOPED_TRACE(std::string(Case.Name) + (Case.LLVM ? " LLVMC" : " HighC"));

  const auto Object = tmpFile("register-pair.o");
  std::vector<std::string> CompileArguments{
      "-target",        Case.Triple, "-O0",
      "-ffreestanding", "-nostdinc", "-fno-stack-protector",
      "-fno-inline"};
  if (Case.Thumb)
    CompileArguments.push_back("-mthumb");
  if (std::string_view(Case.Triple) == "i386-linux-gnu" && !Case.PIC) {
    CompileArguments.push_back("-fno-pic");
    CompileArguments.push_back("-fno-pie");
  }
  CompileArguments.insert(
      CompileArguments.end(),
      {"-c",
       (fs::path(TEST_SOURCE_DIR) / "core/test_mba_register_pair.c").string(),
       "-o", Object.string()});
  const auto Compiled = exec(NEVERD_TEST_CLANG, CompileArguments);
  ASSERT_TRUE(Compiled.ok()) << Compiled.err;

  const auto Output = tmpFile("register-pair.c");
  std::vector<std::string> Arguments{"decompile", "--no-debug"};
  if (Case.LLVM)
    Arguments.push_back("--llvm");
  Arguments.insert(Arguments.end(), {"-o", Output.string(), Object.string()});
  const auto Decompiled = exec(ndBin(), Arguments);
  ASSERT_TRUE(Decompiled.ok()) << Decompiled.err;
  const std::string Source = readSource(Output);
  ASSERT_FALSE(Source.empty());
  EXPECT_NE(Source.find(Case.LLVM ? "uint64_t mba_pair_add("
                                  : "int64_t mba_pair_add("),
            std::string::npos)
      << Source;
  const std::string PairBody = functionBody(Source, "mba_pair_add");
  EXPECT_FALSE(PairBody.empty());
  EXPECT_EQ(PairBody.find(" ^ "), std::string::npos) << PairBody;
  EXPECT_EQ(PairBody.find(" & "), std::string::npos) << PairBody;
  const std::string OrPairBody = functionBody(Source, "mba_pair_or_add");
  EXPECT_FALSE(OrPairBody.empty());
  EXPECT_EQ(OrPairBody.find(" ^ "), std::string::npos) << OrPairBody;
  EXPECT_EQ(OrPairBody.find(" & "), std::string::npos) << OrPairBody;
  const std::string SubPairBody = functionBody(Source, "mba_pair_sub");
  EXPECT_FALSE(SubPairBody.empty());
  EXPECT_EQ(SubPairBody.find(" ^ "), std::string::npos) << SubPairBody;
  EXPECT_EQ(SubPairBody.find(" & "), std::string::npos) << SubPairBody;
  EXPECT_EQ(SubPairBody.find("~"), std::string::npos) << SubPairBody;
  for (const char *Name : {"mba_pair_affine_add", "mba_pair_affine_sub"}) {
    const std::string Body = functionBody(Source, Name);
    EXPECT_FALSE(Body.empty()) << Name;
    EXPECT_EQ(Body.find(" ^ "), std::string::npos) << Body;
    EXPECT_EQ(Body.find(" & "), std::string::npos) << Body;
    EXPECT_EQ(Body.find("~"), std::string::npos) << Body;
  }
  EXPECT_FALSE(functionBody(Source, "mba_pair_fold").empty());
  EXPECT_FALSE(functionBody(Source, "mba_pair_or_fold").empty());
  EXPECT_FALSE(functionBody(Source, "mba_pair_sub_fold").empty());
  EXPECT_FALSE(functionBody(Source, "mba_pair_affine_add_fold").empty());
  EXPECT_FALSE(functionBody(Source, "mba_pair_affine_sub_fold").empty());

  const char *Harness = R"(
#include <inttypes.h>
#include <stdio.h>
static int check_pair(uint32_t xl, uint32_t xh, uint32_t yl, uint32_t yh) {
  uint64_t x = ((uint64_t)xh << 32) | xl;
  uint64_t y = ((uint64_t)yh << 32) | yl;
  uint64_t expected = x + y;
  uint64_t actual = (uint64_t)mba_pair_add(xl, xh, yl, yh);
  uint64_t or_actual = (uint64_t)mba_pair_or_add(xl, xh, yl, yh);
  uint64_t expected_difference = x - y;
  uint64_t difference = (uint64_t)mba_pair_sub(xl, xh, yl, yh);
  uint64_t affine_sum = expected + UINT64_C(0x123456789abcdef0);
  uint64_t affine_difference =
      expected_difference + UINT64_C(0x123456789abcdef0);
  uint32_t folded = (uint32_t)expected ^ (uint32_t)(expected >> 32);
  uint32_t folded_difference =
      (uint32_t)expected_difference ^ (uint32_t)(expected_difference >> 32);
  uint32_t folded_affine_sum =
      (uint32_t)affine_sum ^ (uint32_t)(affine_sum >> 32);
  uint32_t folded_affine_difference =
      (uint32_t)affine_difference ^ (uint32_t)(affine_difference >> 32);
  if (actual != expected || or_actual != expected ||
      difference != expected_difference ||
      (uint64_t)mba_pair_affine_add(xl, xh, yl, yh) != affine_sum ||
      (uint64_t)mba_pair_affine_sub(xl, xh, yl, yh) != affine_difference ||
      (uint32_t)mba_pair_fold(xl, xh, yl, yh) != folded ||
      (uint32_t)mba_pair_or_fold(xl, xh, yl, yh) != folded ||
      (uint32_t)mba_pair_sub_fold(xl, xh, yl, yh) != folded_difference ||
      (uint32_t)mba_pair_affine_add_fold(xl, xh, yl, yh) !=
          folded_affine_sum ||
      (uint32_t)mba_pair_affine_sub_fold(xl, xh, yl, yh) !=
          folded_affine_difference) {
    fprintf(stderr, "pair mismatch %08" PRIx32 ":%08" PRIx32
                    " %08" PRIx32 ":%08" PRIx32 "\n", xh, xl, yh, yl);
    return 1;
  }
  return 0;
}
int main(void) {
  static const uint32_t edges[] = {
      0, 1, 2, UINT32_C(0x7fffffff), UINT32_C(0x80000000),
      UINT32_C(0xfffffffe), UINT32_MAX};
  for (unsigned a = 0; a < 7; ++a)
    for (unsigned b = 0; b < 7; ++b)
      for (unsigned c = 0; c < 7; ++c)
        for (unsigned d = 0; d < 7; ++d)
          if (check_pair(edges[a], edges[b], edges[c], edges[d]))
            return 1;
  uint64_t state = UINT64_C(0x92d68ca2f53b17e9);
  for (unsigned i = 0; i < 4096; ++i) {
    uint32_t words[4];
    for (unsigned j = 0; j < 4; ++j) {
      state = state * UINT64_C(6364136223846793005) + 1;
      words[j] = (uint32_t)(state >> 32);
    }
    if (check_pair(words[0], words[1], words[2], words[3]))
      return 1;
  }
  return 0;
}
)";

  const auto Combined = tmpFile("register-pair-execute.c");
  std::ofstream(Combined) << Source << Harness;
  for (const char *Optimization : {"-O0", "-O2"}) {
    SCOPED_TRACE(Optimization);
    const auto Executable = tmpFile("register-pair-execute");
    const auto Recompiled = exec(
        NEVERD_TEST_CLANG, {"-std=c11", Optimization, "-Werror=return-type",
                            "-Werror=implicit-function-declaration",
                            "-fsanitize=undefined", "-fsanitize-trap=undefined",
                            Combined.string(), "-o", Executable.string()});
    ASSERT_TRUE(Recompiled.ok()) << Recompiled.err << "\n" << Source;
    const auto Ran = exec(Executable.string(), {});
    EXPECT_TRUE(Ran.ok()) << Ran.err << "\n" << Source;
  }
}

INSTANTIATE_TEST_SUITE_P(
    CrossArchitecture, MBARegisterPairSourceTest,
    ::testing::Values(
        RegisterPairCase{"X86", "i386-linux-gnu", false, false},
        RegisterPairCase{"X86", "i386-linux-gnu", false, true},
        RegisterPairCase{"X86PIC", "i386-linux-gnu", false, false, true},
        RegisterPairCase{"X86PIC", "i386-linux-gnu", false, true, true},
        RegisterPairCase{"ARM32", "armv7-linux-gnueabihf", false, false},
        RegisterPairCase{"ARM32", "armv7-linux-gnueabihf", false, true},
        RegisterPairCase{"Thumb2", "armv7-linux-gnueabihf", true, false},
        RegisterPairCase{"Thumb2", "armv7-linux-gnueabihf", true, true},
        RegisterPairCase{"Thumb1", "thumbv6m-none-eabi", true, false},
        RegisterPairCase{"Thumb1", "thumbv6m-none-eabi", true, true}),
    [](const ::testing::TestParamInfo<RegisterPairCase> &Info) {
      return std::string(Info.param.Name) +
             (Info.param.LLVM ? "LLVMC" : "HighC");
    });

} // namespace
