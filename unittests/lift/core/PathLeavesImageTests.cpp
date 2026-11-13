//===- PathLeavesImageTests.cpp - Paths the image holds no code for -------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// A path that reaches an address where the image maps no code keeps its
/// function lifted and ends in a trap on every route.  It used to return
/// zero, or vanish from a conditional branch that then always fell through.
///
//===----------------------------------------------------------------------===//

#include "NeverDLiftFixture.h"

#include <fstream>
#include <iterator>
#include <string>
#include <vector>

namespace {

class PathLeavesImageTest : public NeverDLiftTest {
protected:
  fs::path assemble(const std::string &Source,
                    const char *Triple = "x86_64-linux-gnu") {
    const auto Assembly = tmpFile("leaves.s");
    std::ofstream(Assembly) << Source;
    const auto Object = tmpFile("leaves.o");
    const auto Compiled =
        exec(NEVERD_TEST_CLANG, {"-target", Triple, "-c", Assembly.string(),
                                 "-o", Object.string()});
    EXPECT_TRUE(Compiled.ok()) << Compiled.err;
    return Object;
  }

  std::string highC(const fs::path &Object) {
    const auto Decompiled = decompileToHighC(Object);
    EXPECT_TRUE(Decompiled.ok()) << Decompiled.err;
    std::ifstream File(tmpFile("decompiled_high.c"));
    return std::string((std::istreambuf_iterator<char>(File)),
                       std::istreambuf_iterator<char>());
  }

  /// The text of the definition in \p Module whose opening line names
  /// \p Name, up to the brace that closes it.
  static std::string body(const std::string &Module, const std::string &Name) {
    for (size_t Line = 0; Line < Module.size();) {
      const size_t End = Module.find('\n', Line);
      const std::string Text = Module.substr(Line, End - Line);
      if (!Text.empty() && Text.back() == '{' &&
          Text.find(Name + "(") != std::string::npos) {
        const size_t Close = Module.find("\n}", Line);
        return Module.substr(Line,
                             Close == std::string::npos ? Close : Close - Line);
      }
      if (End == std::string::npos)
        break;
      Line = End + 1;
    }
    return {};
  }
};

// Each function below has one path the object holds no code for: the taken
// arm of a conditional jump, an unconditional jump, the arm a conditional
// jump falls into, and the end of the section a function runs off.  The last
// keeps every path it has.
const char *const LeavingPaths = R"(
        .text
        .globl  taken_leaves
        .type   taken_leaves, @function
taken_leaves:
        cmpq    $6, %rdi
        jl      some_data
        leaq    1(%rsi), %rax
        ret
        .size   taken_leaves, .-taken_leaves

        .globl  jump_leaves
        .type   jump_leaves, @function
jump_leaves:
        leaq    2(%rsi), %rax
        jmp     some_data
        .size   jump_leaves, .-jump_leaves

        .globl  other_arm_leaves
        .type   other_arm_leaves, @function
other_arm_leaves:
        cmpq    $6, %rdi
        jl      1f
        jmp     some_data
1:      leaq    3(%rsi), %rax
        ret
        .size   other_arm_leaves, .-other_arm_leaves

        .globl  stays
        .type   stays, @function
stays:
        cmpq    $6, %rdi
        jl      2f
        leaq    5(%rsi), %rax
        ret
2:      leaq    6(%rsi), %rax
        ret
        .size   stays, .-stays

        .globl  runs_off
        .type   runs_off, @function
runs_off:
        leaq    4(%rsi), %rax
        .size   runs_off, .-runs_off

        .data
some_data:
        .quad   0
)";

TEST_F(PathLeavesImageTest, TheLiftedPathTraps) {
  const fs::path Object = assemble(LeavingPaths);
  const auto Lifted = liftToLLVMIR(Object);
  ASSERT_TRUE(Lifted.ok()) << Lifted.err;
  for (const char *Name :
       {"taken_leaves", "jump_leaves", "other_arm_leaves", "runs_off"}) {
    const std::string Body = body(Lifted.out, std::string("@") + Name);
    ASSERT_FALSE(Body.empty()) << Name << "\n" << Lifted.out;
    EXPECT_NE(Body.find("@llvm.trap"), std::string::npos) << Body;
  }
  // The arms that stay keep their values and the comparison that picks them.
  const std::string Taken = body(Lifted.out, "@taken_leaves");
  EXPECT_NE(Taken.find("icmp"), std::string::npos) << Taken;
  EXPECT_NE(Taken.find("add i64 %arg1, 1"), std::string::npos) << Taken;
  const std::string Other = body(Lifted.out, "@other_arm_leaves");
  EXPECT_NE(Other.find("icmp"), std::string::npos) << Other;
  EXPECT_NE(Other.find("add i64 %arg1, 3"), std::string::npos) << Other;
  const std::string Stays = body(Lifted.out, "@stays");
  ASSERT_FALSE(Stays.empty()) << Lifted.out;
  EXPECT_EQ(Stays.find("@llvm.trap"), std::string::npos) << Stays;
}

TEST_F(PathLeavesImageTest, TheDecompiledPathTraps) {
  const std::string Source = highC(assemble(LeavingPaths));
  for (const char *Name :
       {"taken_leaves", "jump_leaves", "other_arm_leaves", "runs_off"}) {
    const std::string Body = body(Source, std::string(" ") + Name);
    ASSERT_FALSE(Body.empty()) << Name << "\n" << Source;
    EXPECT_NE(Body.find("__builtin_trap();"), std::string::npos) << Body;
    EXPECT_NE(Body.find("where the image maps no code"), std::string::npos)
        << Body;
    // No jump to a label the function never declares.
    EXPECT_EQ(Body.find("goto "), std::string::npos) << Body;
  }
  const std::string Stays = body(Source, " stays");
  ASSERT_FALSE(Stays.empty()) << Source;
  EXPECT_EQ(Stays.find("__builtin_trap"), std::string::npos) << Stays;
}

// The same on the other instruction sets: AArch64, and AArch32, whose
// conditional branch is a branch a predicate guards.
TEST_F(PathLeavesImageTest, EveryInstructionSetTraps) {
  struct Form {
    const char *Triple;
    const char *Source;
    std::vector<const char *> Functions;
  };
  const Form Forms[] = {
      {"aarch64-linux-gnu",
       R"(
        .text
        .p2align 2
        .globl  taken_leaves
        .type   taken_leaves, %function
taken_leaves:
        cmp     x0, #6
        b.lt    some_data
        add     x0, x1, #1
        ret
        .size   taken_leaves, .-taken_leaves
        .globl  jump_leaves
        .type   jump_leaves, %function
jump_leaves:
        add     x0, x1, #2
        b       some_data
        .size   jump_leaves, .-jump_leaves
        .data
        .p2align 3
some_data:
        .quad   0
)",
       {"taken_leaves", "jump_leaves"}},
      {"armv7-linux-gnueabihf",
       R"(
        .text
        .arm
        .globl  taken_leaves
        .type   taken_leaves, %function
taken_leaves:
        cmp     r0, #6
        blt     some_data
        add     r0, r1, #1
        bx      lr
        .size   taken_leaves, .-taken_leaves
        .globl  jump_leaves
        .type   jump_leaves, %function
jump_leaves:
        add     r0, r1, #2
        b       some_data
        .size   jump_leaves, .-jump_leaves
        .thumb
        .globl  thumb_taken_leaves
        .type   thumb_taken_leaves, %function
        .thumb_func
thumb_taken_leaves:
        cmp     r0, #6
        blt     some_data
        adds    r0, r1, #1
        bx      lr
        .size   thumb_taken_leaves, .-thumb_taken_leaves
        .data
        .p2align 2
some_data:
        .long   0
)",
       {"taken_leaves", "jump_leaves", "thumb_taken_leaves"}},
  };
  for (const Form &F : Forms) {
    SCOPED_TRACE(F.Triple);
    const fs::path Object = assemble(F.Source, F.Triple);
    const auto Lifted = liftToLLVMIR(Object);
    ASSERT_TRUE(Lifted.ok()) << Lifted.err;
    const std::string Source = highC(Object);
    for (const char *Name : F.Functions) {
      const std::string IR = body(Lifted.out, std::string("@") + Name);
      ASSERT_FALSE(IR.empty()) << Name << "\n" << Lifted.out;
      EXPECT_NE(IR.find("@llvm.trap"), std::string::npos) << IR;
      // The arm that stays is still chosen by the comparison.
      if (std::string(Name) != "jump_leaves") {
        EXPECT_NE(IR.find("br i1"), std::string::npos) << IR;
        EXPECT_NE(IR.find("add i"), std::string::npos) << IR;
      }
      const std::string C = body(Source, std::string(" ") + Name);
      ASSERT_FALSE(C.empty()) << Name << "\n" << Source;
      EXPECT_NE(C.find("__builtin_trap();"), std::string::npos) << C;
      EXPECT_NE(C.find("where the image maps no code"), std::string::npos) << C;
      EXPECT_EQ(C.find("goto "), std::string::npos) << C;
    }
  }
}

} // namespace
