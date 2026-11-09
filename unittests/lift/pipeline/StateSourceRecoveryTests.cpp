//===- StateSourceRecoveryTests.cpp - State/call source semantic oracles
//---===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Independent bank, partial-width, guest-memory and unknown-call checks.

#include "../../LLVMHostFixture.h"
#include "../NeverDLiftFixture.h"
#include "ByteCellScalarizationTest.h"

#include "neverd/backend/c/LLVMC/LLVMCEmitter.h"
#include "neverd/backend/llvm/LLVMSourceTypeHints.h"
#include "neverd/pipeline/StateSourceRecovery.h"

using namespace neverd;
namespace {
std::string program() {
  return R"(
declare i64 @gateway(ptr, ptr)
declare i64 @opaque(ptr, ptr)
define i64 @work(ptr %state, ptr %context) {
entry:
  %a = getelementptr i8, ptr %state, i64 8
  %r = getelementptr i8, ptr %state, i64 16
  %sel = getelementptr i8, ptr %state, i64 32
  %off = getelementptr i8, ptr %state, i64 40
  %width = getelementptr i8, ptr %state, i64 48
  %count = getelementptr i8, ptr %state, i64 56
  %x = load i64, ptr %state, align 1
  store i64 %x, ptr %a, align 1
  store i64 7, ptr %sel, align 1
  store i64 16, ptr %off, align 1
  store i64 4, ptr %width, align 1
  store i64 1, ptr %count, align 1
  %state_integer = ptrtoint ptr %state to i64
  %state_pointer = inttoptr i64 %state_integer to ptr
  %status = call i64 @gateway(ptr %state_pointer, ptr %context)
  %bad = icmp ne i64 %status, 0
  br i1 %bad, label %failed, label %next
failed:
  ret i64 %status
next:
  %value = load i32, ptr %r, align 1
  %c1 = icmp slt i32 %value, 1024
  %c2 = icmp ult i32 %value, 100
  %f1 = select i1 %c1, i64 256, i64 0
  %f2 = select i1 %c2, i64 2, i64 0
  %flags = or i64 %f1, %f2
  %bit = and i64 %flags, 256
  %small = icmp ne i64 %bit, 0
  br i1 %small, label %left, label %right
left:
  store i32 %value, ptr %context, align 1
  br label %done
right:
  store i32 123, ptr %context, align 1
  br label %done
done:
  %unknown = call i64 @opaque(ptr %state, ptr %context)
  ret i64 %unknown
})";
}
StateSourceContract contract() {
  StateSourceContract C;
  C.StateBytes = 64;
  C.IsolatedBank = true;
  C.ZeroStatusGateways = true;
  StateSourceGateway G;
  G.Name = "gateway";
  G.Selector = {32, 4};
  G.ArgumentCount = {56, 4};
  G.Arguments = {{8, 8}};
  G.ResultOffset = {40, 8};
  G.ResultBytes = {48, 8};
  G.Writes = {{{56, 8}, 0}};
  G.Imports = {
      {7, "example_query", {StateSourceType::U64}, StateSourceType::I32}};
  C.Gateways = {G};
  return C;
}
void optimize(llvm::Module &M, bool Deep = true) {
  Pipeline::OptimizationOptions O;
  O.Strength = Deep ? Pipeline::OptStrength::Deep : Pipeline::OptStrength::Thin;
  O.LLVMLevel = llvm::OptimizationLevel::O2;
  auto R = Pipeline::optimizeModule(M, O);
  ASSERT_NE(R.Stop, OptimizationStopReason::InputInvalid);
  ASSERT_NE(R.Stop, OptimizationStopReason::VerificationFailed);
}
class StateSourceCompiled : public NeverDLiftTest {};

TEST(StateSource, RejectsMissingAssertionsAndConflictingImportsBeforeMutation) {
  llvm::LLVMContext C;
  auto M = cell_test::parse(C, program());
  ASSERT_TRUE(M);
  auto Before = cell_test::print(*M);
  for (unsigned Case = 0; Case != 19; ++Case) {
    auto Contract = contract();
    if (Case == 0)
      Contract.IsolatedBank = false;
    if (Case == 1)
      Contract.ZeroStatusGateways = false;
    if (Case == 2)
      Contract.Gateways[0].Arguments[0].Offset = 64;
    if (Case == 3)
      Contract.Gateways[0].Imports[0].Name = "work";
    if (Case == 4)
      Contract.Gateways[0].Imports[0].Name = "while";
    if (Case == 5)
      Contract.Gateways[0].Imports[0].Result = StateSourceType(99);
    if (Case == 6) {
      auto I = Contract.Gateways[0].Imports[0];
      I.Selector = 8;
      I.Result = StateSourceType::U32;
      Contract.Gateways[0].Imports.push_back(I);
    }
    if (Case == 7)
      Contract.Gateways[0].Imports[0].VoidResult = StateSourceVoidResult(99);
    if (Case == 8)
      Contract.Gateways[0].Imports[0].VoidResult =
          StateSourceVoidResult::Unspecified;
    if (Case == 9)
      Contract.Gateways[0].Imports[0].Defaults = {0, 1};
    if (Case == 10) {
      auto &I = Contract.Gateways[0].Imports[0];
      I.Parameters = {StateSourceType::U8};
      I.Defaults = {256};
    }
    if (Case == 11) {
      auto &I = Contract.Gateways[0].Imports[0];
      I.Parameters = {StateSourceType::Pointer};
      I.Defaults = {1};
    }
    if (Case == 12) {
      auto &I = Contract.Gateways[0].Imports[0];
      I.VariadicFixedParameters = 1;
      I.Defaults = {0};
    }
    if (Case == 13) {
      auto &G = Contract.Gateways[0];
      G.Arguments.push_back({8, 8});
      G.Imports[0].Parameters = {StateSourceType::I32, StateSourceType::I16};
      G.Imports[0].VariadicFixedParameters = 1;
    }
    if (Case == 14) {
      Contract.Gateways[0].FallbackWrites = StateSourceFallbackWrites{};
      Contract.Gateways[0].FallbackWrites->ResultBytes = 3;
    }
    if (Case >= 15) {
      auto &I = Contract.Gateways[0].Imports[0];
      I.VoidResultArgument = 0;
      if (Case != 15)
        I.Result = StateSourceType::Void;
      if (Case == 16)
        I.VoidResult = StateSourceVoidResult::Unspecified;
      if (Case == 17)
        I.VoidResultArgument = 1;
      if (Case == 18)
        I.Defaults = {0};
    }
    auto R = prepareStateSourceRecovery(*M, Contract);
    EXPECT_FALSE(bool(R));
    if (!R)
      llvm::consumeError(R.takeError());
    EXPECT_EQ(Before, cell_test::print(*M));
  }
}

TEST(StateSource, RefusesObservableDynamicOrOrderedBankAddresses) {
  for (const char *Extra :
       {"store ptr %state, ptr %context, align 1",
        "%escaped = icmp eq ptr %state, %context",
        "%d = getelementptr i8, ptr %state, i64 %x\n  store i8 1, ptr %d",
        "%ordered = load volatile i64, ptr %state, align 1",
        "%end = getelementptr i8, ptr %state, i64 63\n  store i64 1, ptr %end, "
        "align 1"}) {
    llvm::LLVMContext C;
    auto Text = program();
    cell_test::replace(Text, "store i64 %x, ptr %a, align 1",
                       std::string(Extra));
    auto M = cell_test::parse(C, Text);
    ASSERT_TRUE(M);
    auto Before = cell_test::print(*M);
    auto R = prepareStateSourceRecovery(*M, contract());
    EXPECT_FALSE(bool(R));
    if (!R)
      llvm::consumeError(R.takeError());
    EXPECT_EQ(Before, cell_test::print(*M));
  }
}

TEST(StateSource, UnreachableGatewayRemovalPreservesSuccessfulRecovery) {
  llvm::LLVMContext C;
  auto M = cell_test::parse(C, R"(
    declare i64 @gateway(ptr, ptr)
    define i64 @work(ptr %state, ptr %context) {
    entry: ret i64 0
    dead:
      %s = call i64 @gateway(ptr %state, ptr %context)
      ret i64 %s
    })");
  ASSERT_TRUE(M);
  auto R = prepareStateSourceRecovery(*M, contract());
  ASSERT_TRUE(bool(R)) << llvm::toString(R.takeError());
  optimize(*M);
  const auto ProbeIR = cell_test::print(*M);
  auto E = finishStateSourceRecovery(*M, *R);
  ASSERT_FALSE(E) << llvm::toString(std::move(E));
  EXPECT_EQ(R->RewrittenCalls, 0u);
  EXPECT_EQ(R->UnresolvedCalls, 0u);
}

TEST(StateSource, KeepsMixedBankAndGuestSelectionsAsPointers) {
  llvm::LLVMContext C;
  auto Text = program();
  cell_test::replace(
      Text, "%value = load i32, ptr %r, align 1",
      "%which = icmp eq i64 %x, 0\n  %address = select i1 %which, ptr %r, ptr "
      "%context\n  %value = load i32, ptr %address, align 1");
  auto M = cell_test::parse(C, Text);
  ASSERT_TRUE(M);
  auto R = prepareStateSourceRecovery(*M, contract());
  ASSERT_TRUE(bool(R)) << llvm::toString(R.takeError());
  optimize(*M);
  const auto ProbeIR = cell_test::print(*M);
  auto E = finishStateSourceRecovery(*M, *R);
  ASSERT_FALSE(E) << llvm::toString(std::move(E));
  EXPECT_EQ(R->RewrittenCalls, 1u) << ProbeIR;
  EXPECT_FALSE(llvm::verifyModule(*M));
}

TEST(StateSource,
     ResolvesCurrentIRConstantsOnWindowsAndELFAndKeepsUnknownSites) {
  for (const char *Triple :
       {"x86_64-pc-windows-msvc", "aarch64-unknown-linux-gnu"})
    for (bool Known : {false, true}) {
      llvm::LLVMContext C;
      auto Text = program();
      if (!Known)
        cell_test::replace(Text, "store i64 7, ptr %sel",
                           "store i64 %x, ptr %sel");
      auto M = cell_test::parse(C, Text);
      ASSERT_TRUE(M);
      M->setTargetTriple(llvm::Triple(Triple));
      auto R = prepareStateSourceRecovery(*M, contract());
      ASSERT_TRUE(bool(R)) << llvm::toString(R.takeError());
      optimize(*M);
      const auto ProbeIR = cell_test::print(*M);
      auto E = finishStateSourceRecovery(*M, *R);
      ASSERT_FALSE(E) << llvm::toString(std::move(E));
      EXPECT_EQ(R->RewrittenCalls, unsigned(Known)) << ProbeIR;
      EXPECT_EQ(R->UnresolvedCalls, unsigned(!Known));
      optimize(*M, true);
      auto Printed = cell_test::print(*M);
      EXPECT_EQ(Printed.find("__neverd_state_probe_"), std::string::npos);
      EXPECT_NE(Printed.find("call i64 @opaque"), std::string::npos);
      if (Known) {
        EXPECT_NE(Printed.find("call i32 @example_query"), std::string::npos);
        EXPECT_EQ(Printed.find("call i64 @gateway"), std::string::npos);
        EXPECT_EQ(Printed.find("and i64 %flags"), std::string::npos);
      }
    }
}

std::string selectorProgram(bool Loop = false) {
  auto Text = program();
  cell_test::replace(Text, "store i64 16, ptr %off, align 1",
                     "%clobber = call i64 @opaque(ptr %state, ptr %context)\n"
                     "  store i64 16, ptr %off, align 1");
  Text.resize(Text.find("next:\n") + 6);
  if (Loop) {
    cell_test::replace(Text, "store i64 7, ptr %sel, align 1", R"(
  %saved_count = load i64, ptr %count, align 1
  %limit_low = and i64 %x, 3
  %limit = add i64 %limit_low, 2
  br label %loop
loop:
  %iteration = phi i64 [0, %entry], [%next_iteration, %again]
  %candidate = phi i64 [7, %entry], [%changed, %again]
  store i64 %candidate, ptr %sel, align 1
)");
    cell_test::replace(Text, "store i64 1, ptr %count, align 1",
                       "store i64 %saved_count, ptr %count, align 1");
    Text += R"(
  %next_iteration = add i64 %iteration, 1
  %done = icmp eq i64 %next_iteration, %limit
  br i1 %done, label %exit, label %again
again:
  %changed = load i64, ptr %sel, align 1
  br label %loop
exit:
)";
  }
  return Text + "  %value = load i64, ptr %r, align 1\nret i64 %value\n}\n";
}

TEST(StateSource, ScalarCandidatesGuardCurrentSelectorsAcrossOpaqueCalls) {
  for (bool BigEndian : {false, true})
    for (unsigned Case = 0; Case < 7; ++Case) {
      SCOPED_TRACE(Case);
      llvm::LLVMContext C;
      auto Text = selectorProgram(Case == 5);
      auto Contract = contract();
      Contract.Gateways[0].ArgumentCount = {56, 8};
      Contract.Gateways[0].Selector = {32, 8};
      if (Case == 1)
        Contract.Gateways[0].Selector = {BigEndian ? 36u : 32u, 4};
      if (Case == 2)
        cell_test::replace(Text, "store i64 7, ptr %sel",
                           "store i64 %x, ptr %sel");
      if (Case == 3)
        cell_test::replace(Text, "store i64 7, ptr %sel",
                           "store i64 8, ptr %sel");
      if (Case == 4) {
        // Two independent state transports, both crossing the opaque call.
        cell_test::replace(Text, "store i64 7, ptr %sel, align 1",
                           "store i64 7, ptr %count, align 1");
        cell_test::replace(Text, "store i64 16, ptr %off, align 1",
                           "%saved = load i64, ptr %count, align 1\n"
                           "  store i64 %saved, ptr %sel, align 1\n"
                           "  store i64 16, ptr %off, align 1");
      }
      if (Case == 6)
        cell_test::replace(Text, "store i64 7, ptr %sel, align 1",
                           "%choose = icmp eq i64 %x, 0\n"
                           "  %choice = select i1 %choose, i64 7, i64 8\n"
                           "  store i64 %choice, ptr %sel, align 1");
      auto M = cell_test::parse(C, Text);
      ASSERT_TRUE(M);
      M->setTargetTriple(llvm::Triple(BigEndian ? "aarch64_be-unknown-linux-gnu"
                                                : "x86_64-pc-windows-msvc"));
      M->setDataLayout(BigEndian ? "E-p:64:64" : "e-p:64:64");
      auto R = prepareStateSourceRecovery(*M, Contract);
      ASSERT_TRUE(bool(R)) << llvm::toString(R.takeError());
      optimize(*M);
      const auto ProbeIR = cell_test::print(*M);
      auto E = finishStateSourceRecovery(*M, *R);
      ASSERT_FALSE(E) << llvm::toString(std::move(E));
      bool Expected = Case != 2 && Case != 3;
      EXPECT_EQ(R->RewrittenCalls, unsigned(Expected)) << ProbeIR;
      EXPECT_EQ(R->GuardedCalls, unsigned(Expected));
      EXPECT_EQ(R->UnresolvedCalls, unsigned(!Expected));
      EXPECT_FALSE(llvm::verifyModule(*M));
      const auto IR = cell_test::print(*M);
      EXPECT_NE(IR.find("call i64 @gateway"), std::string::npos);
      if (Expected) {
        EXPECT_NE(IR.find("selector_matches"), std::string::npos) << IR;
        EXPECT_NE(IR.find("call i32 @example_query"), std::string::npos);
      }
    }
}

TEST_F(StateSourceCompiled,
       ScalarCandidatesKeepClobberedSelectorsAndFullBanks) {
  if (!hasCrossTargetClang())
    GTEST_SKIP() << "Configured Clang is unavailable";
  for (bool Loop : {false, true}) {
    llvm::LLVMContext C;
    auto Text = selectorProgram(Loop);
    if (!Loop)
      cell_test::replace(Text, "store i64 1, ptr %count, align 1", "");
    auto M = cell_test::parse(C, Text);
    ASSERT_TRUE(M);
    M->getFunction("work")->setName("before");
    auto Original = tmpFile("selector-before.ll");
    std::ofstream(Original) << test::printHostCompilerFixture(*M);
    M->getFunction("before")->setName("after");
    auto R = prepareStateSourceRecovery(*M, contract());
    ASSERT_TRUE(bool(R)) << llvm::toString(R.takeError());
    optimize(*M);
    auto E = finishStateSourceRecovery(*M, *R);
    ASSERT_FALSE(E) << llvm::toString(std::move(E));
    ASSERT_EQ(R->RewrittenCalls, 1u);
    ASSERT_EQ(R->GuardedCalls, 1u);
    optimize(*M);
    std::string Source;
    llvm::raw_string_ostream OS(Source);
    CEmitterOptions Options;
    Options.PreserveLLVMFunctionTypes = true;
    Options.UseUnalignedPointers = true;
    Options.ScalarPointers = CEmitterOptions::ScalarPointerSpelling::AliasTypes;
    ASSERT_TRUE(LLVMCEmitter().emit(*M, OS, Options));
    auto After = tmpFile("selector-after.c");
    std::ofstream(After) << Source;
    auto Harness = tmpFile("selector-oracle.c");
    std::ofstream(Harness) << R"(
#include <stdint.h>
#include <string.h>
extern uint64_t before(void *,void *), after(void *,void *);
static uint64_t get(const void *s,unsigned o) { uint64_t v;memcpy(&v,(const char*)s+o,8);return v; }
static void put(void *s,unsigned o,uint64_t v,unsigned n) { memcpy((char*)s+o,&v,n); }
static unsigned calls,gateways;
static uint64_t selected,trace;
static uint32_t *guest;
int32_t example_query(uint64_t x) { ++calls;trace=x;*guest+=(uint32_t)x;return -(int32_t)(x&0xffff); }
uint64_t opaque(void *s,void *ctx) {
 (void)ctx;put(s,32,selected,8);put(s,8,get(s,8)^UINT64_C(0xe193024d),8);
 *guest^=(uint32_t)selected;return selected;
}
uint64_t gateway(void *s,void *ctx) {
 (void)ctx;++gateways;
 uint64_t x=get(s,8),off=get(s,40),width=get(s,48);
 if((uint32_t)get(s,32)!=7 || (uint32_t)get(s,56)!=1) {
  put(s,(unsigned)off,UINT64_C(0x5a3710998ecab652),8);
  ((unsigned char*)s)[63]^=0x31;*guest+=731;return UINT64_C(0x8000000070000019);
 }
 put(s,56,0,8);int32_t v=example_query(x);
 put(s,(unsigned)off,(uint64_t)(int64_t)v,(unsigned)width);return 0;
}
int main(void) {
 unsigned fast=0,slow=0;
 const uint64_t choices[]={7,8,UINT64_MAX,UINT64_C(0x8000000000000007)};
 for(unsigned k=0;k<2048;k++) {
  unsigned char a[80],b[80];for(unsigned j=0;j<80;j++) a[j]=(unsigned char)(k*7+j*13);
  unsigned off=k&15;put(a+off,0,UINT64_MAX-(uint64_t)k*0x192ad76f,8);
  put(a+off,56,(k/64)%3,8);memcpy(b,a,sizeof(a));selected=choices[(k/16)%4];
  uint32_t ga=k,gb=k;guest=&ga;calls=gateways=0;trace=0;
  uint64_t ra=before(a+off,&ga),ta=trace;unsigned ca=calls;
  guest=&gb;calls=gateways=0;trace=0;uint64_t rb=after(b+off,&gb);
  if(ra!=rb || ta!=trace || ca!=calls || ga!=gb || memcmp(a,b,sizeof(a))) return 1;
  if(gateways) ++slow;else ++fast;
 }
 return !(fast && slow);
})";
    for (const char *Level : {"-O0", "-O2"}) {
      auto Executable = tmpFile(std::string("selector-oracle") + Level +
                                test::executableSuffix());
      auto Built =
          exec(NEVERD_TEST_CLANG,
               {"-std=c11", Level, "-fsanitize=undefined",
                "-fsanitize-trap=all", Original.string(), After.string(),
                Harness.string(), "-o", Executable.string()});
      ASSERT_TRUE(Built.ok()) << Built.err;
      auto Ran = exec(Executable.string(), {});
      EXPECT_TRUE(Ran.ok()) << Ran.exitCode << " " << Ran.err;
    }
  }
}

TEST(StateSource, PartialSelectorsUseTheOriginalSnapshotOnBothByteOrders) {
  for (bool Big : {false, true})
    for (unsigned Bytes : {1u, 2u, 4u}) {
      llvm::LLVMContext C;
      auto Text = program();
      const auto Delta = Big ? 8 - Bytes : 0;
      cell_test::replace(
          Text, "store i64 7, ptr %sel",
          "%partial = getelementptr i8, ptr %state, i64 " +
              std::to_string(24 + Delta) + "\n  store i" +
              std::to_string(Bytes * 8) + " 7, ptr %partial, align 1" +
              "\n  %whole_base = getelementptr i8, ptr %state, i64 24" +
              "\n  %whole = load i64, ptr %whole_base, align 1" +
              "\n  store i" + std::to_string(Bytes * 8) +
              " 9, ptr %partial, align 1" + "\n  store i64 %whole, ptr %sel");
      auto M = cell_test::parse(C, Text, Big ? "E-p:64:64" : "e-p:64:64");
      ASSERT_TRUE(M);
      auto Contract = contract();
      Contract.Gateways[0].Selector = {32 + Delta, Bytes};
      if (Big)
        Contract.Gateways[0].ArgumentCount = {60, 4};
      auto R = prepareStateSourceRecovery(*M, Contract);
      ASSERT_TRUE(bool(R)) << llvm::toString(R.takeError());
      optimize(*M);
      auto E = finishStateSourceRecovery(*M, *R);
      ASSERT_FALSE(E) << llvm::toString(std::move(E));
      EXPECT_EQ(R->RewrittenCalls, 1u) << cell_test::print(*M);
    }
}

std::string guestSelectorProgram() {
  return R"(
declare i64 @gateway(ptr, ptr)
declare i64 @change_guest(ptr, ptr)
define i64 @work(ptr %state, ptr %context) {
  %base1 = load i64, ptr %state, align 1
  %address1 = add i64 %base1, 16
  %p1 = inttoptr i64 %address1 to ptr
  store i32 7, ptr %p1, align 1
  call i64 @change_guest(ptr %state, ptr %context)
  %base2 = load i64, ptr %state, align 1
  %address2 = add i64 %base2, 16
  %p2 = inttoptr i64 %address2 to ptr
  %selector = load i32, ptr %p2, align 1
  %sel = getelementptr i8, ptr %state, i64 32
  store i32 %selector, ptr %sel, align 1
  %off = getelementptr i8, ptr %state, i64 40
  store i64 24, ptr %off, align 1
  %width = getelementptr i8, ptr %state, i64 48
  store i64 8, ptr %width, align 1
  %status = call i64 @gateway(ptr %state, ptr %context)
  ret i64 %status
})";
}
TEST(StateSource, GuestSelectorLineageKeepsEndianSlicesAndRuntimeGuards) {
  for (bool Big : {false, true})
    for (bool Mismatch : {false, true}) {
      llvm::LLVMContext C;
      auto Text = guestSelectorProgram();
      cell_test::replace(Text, "store i32 7, ptr %p1",
                         "store i64 1234605616436508552, ptr %p1");
      cell_test::replace(Text, "%address2 = add i64 %base2, 16",
                         Mismatch ? "%address2 = add i64 %base2, 32"
                                  : "%address2 = add i64 %base2, 18");
      cell_test::replace(Text, "%selector = load i32", "%selector = load i16");
      cell_test::replace(Text, "store i32 %selector", "store i16 %selector");
      auto M = cell_test::parse(C, Text);
      ASSERT_TRUE(M);
      M->setDataLayout(Big ? "E-p:64:64-i64:64" : "e-p:64:64-i64:64");
      auto Contract = contract();
      Contract.Gateways[0].Selector->Bytes = 2;
      Contract.Gateways[0].Imports[0].Selector = Big ? 0x3344 : 0x5566;
      auto R = prepareStateSourceRecovery(*M, Contract);
      ASSERT_TRUE(bool(R)) << llvm::toString(R.takeError());
      optimize(*M);
      auto E = finishStateSourceRecovery(*M, *R);
      ASSERT_FALSE(E) << llvm::toString(std::move(E));
      EXPECT_EQ(R->RewrittenCalls, Mismatch ? 0u : 1u);
      EXPECT_EQ(R->GuardedCalls, Mismatch ? 0u : 1u);
    }
}
TEST_F(StateSourceCompiled, GuestSelectorCandidatesGuardChangedBasesAndBytes) {
  if (!hasCrossTargetClang())
    GTEST_SKIP() << "Configured Clang is unavailable";
  llvm::LLVMContext C;
  auto M = cell_test::parse(C, guestSelectorProgram());
  ASSERT_TRUE(M);
  M->getFunction("work")->setName("before");
  auto Original = tmpFile("guest-selector-before.ll");
  std::ofstream(Original) << test::printHostCompilerFixture(*M);
  M->getFunction("before")->setName("after");
  auto R = prepareStateSourceRecovery(*M, contract());
  ASSERT_TRUE(bool(R)) << llvm::toString(R.takeError());
  optimize(*M);
  auto E = finishStateSourceRecovery(*M, *R);
  ASSERT_FALSE(E) << llvm::toString(std::move(E));
  ASSERT_EQ(R->RewrittenCalls, 1u);
  ASSERT_EQ(R->GuardedCalls, 1u);
  optimize(*M);
  std::string Source;
  llvm::raw_string_ostream OS(Source);
  CEmitterOptions Options;
  Options.PreserveLLVMFunctionTypes = true;
  Options.UseUnalignedPointers = true;
  Options.ScalarPointers = CEmitterOptions::ScalarPointerSpelling::AliasTypes;
  ASSERT_TRUE(LLVMCEmitter().emit(*M, OS, Options));
  auto After = tmpFile("guest-selector-after.c");
  std::ofstream(After) << Source;
  auto Harness = tmpFile("guest-selector-oracle.c");
  std::ofstream(Harness) << R"(
#include <stdint.h>
#include <string.h>
extern uint64_t before(void*,void*),after(void*,void*);
static uint64_t get(const void *s,unsigned o) { uint64_t v;memcpy(&v,(const char*)s+o,8);return v; }
static void put(void *s,unsigned o,uint64_t v,unsigned n) { memcpy((char*)s+o,&v,n); }
static unsigned mode,calls,gateways;
static uint64_t trace;
static unsigned char guest[64];
int32_t example_query(uint64_t x) { ++calls;trace=trace*31+x;guest[1]^=(unsigned char)x;return (int32_t)(x^0x81234567); }
uint64_t change_guest(void *s,void *ctx) {
 (void)ctx;
 if(mode&1) put(s,0,(uintptr_t)(guest+32),8);
 unsigned char *p=(void*)(uintptr_t)get(s,0);
 if(mode&2) put(p,16,9,1);
 if(mode&4) put(p,16,7,4);
 return 0;
}
uint64_t gateway(void *s,void *ctx) {
 (void)ctx;++gateways;
 if((uint32_t)get(s,32)!=7 || (uint32_t)get(s,56)!=1) {
  guest[3]++;put(s,24,0xabcdef,3);return UINT64_C(0xfeed000012345678);
 }
 uint64_t x=get(s,8),off=get(s,40),width=get(s,48);put(s,56,0,8);
 put(s,(unsigned)off,(uint64_t)(int64_t)example_query(x),(unsigned)width);return 0;
}
int main(void) {
 unsigned fast=0,slow=0;
 for(unsigned k=0;k<4096;k++) {
  unsigned char a[80],b[80],observed[64],initial[64];
  for(unsigned j=0;j<80;j++) a[j]=(unsigned char)(k*7+j*13);
  for(unsigned j=0;j<64;j++) initial[j]=(unsigned char)(k*11+j*5);
  unsigned off=k&15;mode=(k/16)%8;
  put(a+off,0,(uintptr_t)guest,8);put(a+off,8,(uint64_t)k*0x192ad76fULL,8);put(a+off,56,(k/128)%2,8);
  memcpy(b,a,80);memcpy(guest,initial,64);calls=gateways=0;trace=0;
  uint64_t ra=before(a+off,0),ta=trace;unsigned ca=calls;memcpy(observed,guest,64);
  memcpy(guest,initial,64);calls=gateways=0;trace=0;uint64_t rb=after(b+off,0);
  if(ra!=rb || ta!=trace || ca!=calls || memcmp(a,b,80) || memcmp(guest,observed,64)) return 1;
  if(gateways) ++slow;else ++fast;
 }
 return !(fast && slow);
})";
  for (const char *Level : {"-O0", "-O2"}) {
    auto Executable = tmpFile(std::string("guest-selector-oracle") + Level +
                              test::executableSuffix());
    auto Built = exec(NEVERD_TEST_CLANG,
                      {"-std=c11", Level, "-fsanitize=undefined",
                       "-fsanitize-trap=all", Original.string(), After.string(),
                       Harness.string(), "-o", Executable.string()});
    ASSERT_TRUE(Built.ok()) << Built.err;
    auto Ran = exec(Executable.string(), {});
    EXPECT_TRUE(Ran.ok()) << Ran.exitCode << " " << Ran.err;
  }
}

TEST(StateSource, StringSpellingRequiresAReadOnlyGlobalAndACallSiteContract) {
  for (unsigned Case = 0; Case < 4; ++Case) {
    llvm::LLVMContext C;
    auto M = cell_test::parse(C, R"(
@text = private constant [5 x i8] c"item\00"
declare i64 @consume(ptr)
define i64 @example(ptr %state, ptr %context) {
  %result = call i64 @consume(ptr @text)
  ret i64 %result
})");
    ASSERT_TRUE(M);
    auto *Call =
        llvm::cast<llvm::CallInst>(&M->getFunction("example")->front().front());
    if (Case != 0)
      Call->addParamAttr(
          0, llvm::Attribute::get(C, LLVMSourceByValueStringParameter));
    if (Case == 2)
      M->getGlobalVariable("text", true)->setConstant(false);
    if (Case == 3)
      M->getGlobalVariable("text", true)->setExternallyInitialized(true);
    std::string Source;
    llvm::raw_string_ostream OS(Source);
    CEmitterOptions Options;
    Options.PreserveLLVMFunctionTypes = true;
    ASSERT_TRUE(LLVMCEmitter().emit(*M, OS, Options));
    EXPECT_EQ(Source.find("consume(\"item\")") != std::string::npos, Case == 1)
        << Source;
    EXPECT_EQ(Source.find("consume(text)") != std::string::npos, Case != 1)
        << Source;
  }
}

TEST_F(StateSourceCompiled,
       PreservesCompleteUnalignedBankGuestEffectsAndFailureStatus) {
  if (!hasCrossTargetClang())
    GTEST_SKIP() << "Configured Clang is unavailable";
  llvm::LLVMContext C;
  auto Text = program();
  cell_test::replace(Text, "store i64 7, ptr %sel",
                     "%partial = getelementptr i8, ptr %state, i64 24\n  store "
                     "i32 7, ptr %partial, align 1\n  %whole = load i64, ptr "
                     "%partial, align 1\n  store i64 %whole, ptr %sel");
  cell_test::replace(Text, "store i64 1, ptr %count",
                     "%arity = and i64 %x, 1\n  store i64 %arity, ptr %count");
  cell_test::replace(Text, "%value = load i32, ptr %r, align 1",
                     "%choice = and i64 %x, 2\n  %which = icmp eq i64 %choice, "
                     "0\n  %address = select i1 %which, ptr %r, ptr %context\n "
                     " %value = load i32, ptr %address, align 1");
  auto M = cell_test::parse(C, Text);
  ASSERT_TRUE(M);
  M->getFunction("work")->setName("before");
  auto Original = tmpFile("before.ll");
  std::ofstream(Original) << test::printHostCompilerFixture(*M);
  M->getFunction("before")->setName("after");
  auto Contract = contract();
  Contract.Gateways[0].Imports[0].Parameters = {StateSourceType::I32};
  auto R = prepareStateSourceRecovery(*M, Contract);
  ASSERT_TRUE(bool(R)) << llvm::toString(R.takeError());
  optimize(*M);
  const auto ProbeIR = cell_test::print(*M);
  auto E = finishStateSourceRecovery(*M, *R);
  ASSERT_FALSE(E) << llvm::toString(std::move(E));
  ASSERT_EQ(R->RewrittenCalls, 1u) << ProbeIR;
  ASSERT_EQ(R->GuardedCalls, 1u);
  optimize(*M, true);
  std::string Source;
  llvm::raw_string_ostream OS(Source);
  CEmitterOptions Options;
  Options.PreserveLLVMFunctionTypes = true;
  Options.UseUnalignedPointers = true;
  Options.ScalarPointers = CEmitterOptions::ScalarPointerSpelling::AliasTypes;
  ASSERT_TRUE(LLVMCEmitter().emit(*M, OS, Options));
  EXPECT_NE(Source.find("int32_t example_query(int32_t)"), std::string::npos);
  auto After = tmpFile("after.c");
  std::ofstream(After) << Source;
  auto Harness = tmpFile("oracle.c");
  std::ofstream(Harness) << R"(
#include <stdint.h>
#include <string.h>
extern uint64_t before(void *,void *), after(void *,void *);
static uint64_t get(const void *s, unsigned o) { uint64_t v; memcpy(&v,(const char *)s+o,8);return v; }
static void put(void *s,unsigned o,uint64_t v,unsigned n) { memcpy((char *)s+o,&v,n); }
static uint32_t *active_guest;
int32_t example_query(int32_t x) { *active_guest += (uint32_t)x; return (int32_t)(x ^ 0x12345678U); }
uint64_t gateway(void *s,void *ctx) {
  (void)ctx;
  if ((uint32_t)get(s,32)!=7 || (uint32_t)get(s,56)!=1) return 19;
  uint64_t x=get(s,8),off=get(s,40),width=get(s,48);
  put(s,56,0,8);put(s,(unsigned)off,(uint64_t)(int64_t)example_query(x),(unsigned)width);
  return 0;
}
uint64_t opaque(void *s,void *ctx) {
  unsigned char *p=s; for(unsigned i=0;i<64;i++) p[i]^=(unsigned char)(i*11);
  uint32_t v;memcpy(&v,ctx,4);v^=0x98765432U;memcpy(ctx,&v,4);
  return (get(s,0)&1) ? UINT64_C(0xfedcba9876543210) : 0;
}
int main(void) {
 for(unsigned k=0;k<4096;k++) {
  unsigned char a[80],b[80]; for(unsigned i=0;i<80;i++) a[i]=(unsigned char)(k*7+i*13);
  unsigned offset=k&15;uint64_t x=(uint64_t)k*0x192ad76fULL;put(a+offset,0,x,8);
  memcpy(b,a,80);uint32_t ga=k,gb=k;
  active_guest=&ga;uint64_t ra=before(a+offset,&ga);
  active_guest=&gb;uint64_t rb=after(b+offset,&gb);
  if(ra!=rb || ga!=gb || memcmp(a,b,80)) return 1;
 }
 return 0;
})";
  for (const char *Level : {"-O0", "-O2"}) {
    auto Executable =
        tmpFile(std::string("oracle") + Level + test::executableSuffix());
    auto Built = exec(NEVERD_TEST_CLANG,
                      {"-std=c11", Level, "-fsanitize=undefined",
                       "-fsanitize-trap=all", Original.string(), After.string(),
                       Harness.string(), "-o", Executable.string()});
    ASSERT_TRUE(Built.ok()) << Built.err;
    auto Ran = exec(Executable.string(), {});
    EXPECT_TRUE(Ran.ok()) << Ran.exitCode << " " << Ran.err;
  }
}

std::string stringProgram() {
  return R"(
declare i64 @gateway(ptr, ptr)
define i64 @work(ptr %state, ptr %context) {
  %base = load i64, ptr %state, align 1
  %name = add i64 %base, 3
  %arg = getelementptr i8, ptr %state, i64 8
  %p = getelementptr i8, ptr %state, i64 16
  %sel = getelementptr i8, ptr %state, i64 32
  %off = getelementptr i8, ptr %state, i64 40
  %width = getelementptr i8, ptr %state, i64 48
  %key = getelementptr i8, ptr %state, i64 64
  store i64 16, ptr %arg, align 1
  store i64 %name, ptr %p, align 1
  store i64 7, ptr %sel, align 1
  store i64 24, ptr %off, align 1
  store i64 8, ptr %width, align 1
  store i64 90, ptr %key, align 1
  %status = call i64 @gateway(ptr %state, ptr %context)
  ret i64 %status
})";
}
StateSourceContract stringContract() {
  auto C = contract();
  C.StateBytes = 80;
  auto &G = C.Gateways[0];
  G.ArgumentCount.reset();
  G.Arguments = {{8, 8}, {24, 8}};
  G.IndirectArguments = {0};
  G.Writes.push_back({{16, 8}, 0});
  G.Imports = {{7,
                "lookup_item",
                {StateSourceType::Pointer, StateSourceType::U64},
                StateSourceType::Pointer}};
  G.Strings = {{0, 0, true, StateSourceSlot{56, 8}, StateSourceSlot{64, 8}, 1}};
  G.ExcludedArguments = {{1, UINT64_MAX}};
  StateSourceImage I;
  I.Base = {0, 8};
  I.Readable = true;
  I.Bytes = {1, 2, 3};
  for (unsigned char B : std::string("example.dll"))
    I.Bytes.push_back(B ^ 90);
  I.Bytes.push_back(90);
  C.Images = {I};
  return C;
}

TEST(StateSource, RejectsIncompleteStringAndIndirectContractsBeforeMutation) {
  llvm::LLVMContext C;
  auto M = cell_test::parse(C, stringProgram());
  ASSERT_TRUE(M);
  const auto Before = cell_test::print(*M);
  for (unsigned Case = 0; Case < 9; ++Case) {
    auto Contract = stringContract();
    auto &G = Contract.Gateways[0];
    if (Case == 0)
      Contract.Images[0].Readable = false;
    if (Case == 1)
      Contract.Images[0].Base.Bytes = 4;
    if (Case == 2)
      Contract.Images[0].Bytes.clear();
    if (Case == 3)
      G.Strings[0].ByValue = false;
    if (Case == 4)
      G.Strings[0].Mode.reset();
    if (Case == 5)
      G.Strings[0].Image = 1;
    if (Case == 6)
      G.IndirectArguments = {0, 0};
    if (Case == 7)
      G.ExcludedArguments[0].Argument = 2;
    if (Case == 8)
      G.Imports[0].Parameters[0] = StateSourceType::U64;
    auto R = prepareStateSourceRecovery(*M, Contract);
    ASSERT_FALSE(bool(R)) << Case;
    llvm::consumeError(R.takeError());
    EXPECT_EQ(cell_test::print(*M), Before) << Case;
  }
}

TEST(StateSource, StringCandidatesRequireProvenOffsetsKeysAndTerminators) {
  for (bool Big : {false, true})
    for (unsigned Case = 0; Case < 8; ++Case) {
      llvm::LLVMContext C;
      auto Text = stringProgram();
      auto Contract = stringContract();
      if (Big)
        Contract.Gateways[0].Selector = {36, 4};
      if (Case == 1)
        cell_test::replace(Text, "store i64 16, ptr %arg",
                           "store i64 %base, ptr %arg");
      if (Case == 2)
        cell_test::replace(Text, "store i64 16, ptr %arg",
                           "store i64 79, ptr %arg");
      if (Case == 3)
        cell_test::replace(Text, "store i64 90, ptr %key",
                           "store i64 %base, ptr %key");
      if (Case == 4)
        Contract.Images[0].Bytes.back() = 91;
      if (Case == 5)
        cell_test::replace(Text, "%name = add i64 %base, 3",
                           "%name = add i64 %base, 99");
      if (Case >= 6) {
        for (size_t I = 3; I < Contract.Images[0].Bytes.size(); ++I)
          Contract.Images[0].Bytes[I] ^= 90;
        if (Case == 6)
          cell_test::replace(Text, "store i64 90, ptr %key",
                             "store i64 0, ptr %key");
        else
          cell_test::replace(Text, "store i64 90, ptr %key",
                             "%mode = getelementptr i8, ptr %state, i64 56\n"
                             "  store i64 0, ptr %mode, align 1\n"
                             "  store i64 90, ptr %key");
      }
      auto M = cell_test::parse(C, Text, Big ? "E-p:64:64" : "e-p:64:64");
      ASSERT_TRUE(M);
      M->setTargetTriple(llvm::Triple(Big ? "aarch64_be-unknown-linux-gnu"
                                          : "x86_64-pc-windows-msvc"));
      auto R = prepareStateSourceRecovery(*M, Contract);
      ASSERT_TRUE(bool(R)) << llvm::toString(R.takeError());
      optimize(*M);
      auto E = finishStateSourceRecovery(*M, *R);
      ASSERT_FALSE(E) << llvm::toString(std::move(E));
      EXPECT_EQ(R->RewrittenCalls, unsigned(Case == 0 || Case >= 6))
          << cell_test::print(*M);
      EXPECT_EQ(R->GuardedCalls, unsigned(Case == 0 || Case >= 6));
      optimize(*M);
      const auto IR = cell_test::print(*M);
      EXPECT_NE(IR.find("call i64 @gateway"), std::string::npos);
      EXPECT_EQ(IR.find("__neverd_state_probe_"), std::string::npos);
      if (Case == 0 || Case >= 6) {
        EXPECT_NE(IR.find("example.dll"), std::string::npos);
        EXPECT_NE(IR.find("call ptr @lookup_item"), std::string::npos);
      }
    }
}

TEST_F(StateSourceCompiled,
       StringGuardsKeepChangedBytesModesHandlesAndFullState) {
  if (!hasCrossTargetClang())
    GTEST_SKIP() << "Configured Clang is unavailable";
  llvm::LLVMContext C;
  auto M = cell_test::parse(C, stringProgram());
  ASSERT_TRUE(M);
  M->getFunction("work")->setName("before");
  auto Original = tmpFile("string-before.ll");
  std::ofstream(Original) << test::printHostCompilerFixture(*M);
  M->getFunction("before")->setName("after");
  auto R = prepareStateSourceRecovery(*M, stringContract());
  ASSERT_TRUE(bool(R)) << llvm::toString(R.takeError());
  optimize(*M);
  auto E = finishStateSourceRecovery(*M, *R);
  ASSERT_FALSE(E) << llvm::toString(std::move(E));
  ASSERT_EQ(R->RewrittenCalls, 1u) << cell_test::print(*M);
  ASSERT_EQ(R->GuardedCalls, 1u);
  optimize(*M);
  std::string Source;
  llvm::raw_string_ostream OS(Source);
  CEmitterOptions Options;
  Options.PreserveLLVMFunctionTypes = true;
  Options.UseUnalignedPointers = true;
  Options.ScalarPointers = CEmitterOptions::ScalarPointerSpelling::AliasTypes;
  ASSERT_TRUE(LLVMCEmitter().emit(*M, OS, Options));
  EXPECT_NE(Source.find("lookup_item(\"example.dll\""), std::string::npos)
      << Source;
  auto After = tmpFile("string-after.c");
  std::ofstream(After) << Source;
  auto Harness = tmpFile("string-oracle.c");
  std::ofstream(Harness) << R"(
#include <stdint.h>
#include <string.h>
extern uint64_t before(void *, void *), after(void *, void *);
static uint64_t get(const void *s,unsigned o) { uint64_t v;memcpy(&v,(const char*)s+o,8);return v; }
static void put(void *s,unsigned o,uint64_t v) { memcpy((char*)s+o,&v,8); }
static unsigned calls, gateways;
static uint64_t trace;
static unsigned char result, *guest;
void *lookup_item(const char *s, uint64_t handle) {
  ++calls;trace=handle;
  while (*s) trace=trace*131+(unsigned char)*s++;
  guest[0]^=(unsigned char)trace;
  return handle & 1 ? &result : 0;
}
uint64_t gateway(void *s,void *context) {
  (void)context;++gateways;
  uint64_t handle=get(s,24);
  if (handle==UINT64_MAX) { put(s,72,12345);return UINT64_C(0xfedcba9876543210); }
  const char *p=(const char*)(uintptr_t)get(s,(unsigned)get(s,8));
  char text[64];unsigned i=0;
  unsigned char key=get(s,56)==1 && get(s,64) ? (unsigned char)get(s,64) : 0;
  do { text[i]=(unsigned char)p[i]^key; } while (text[i++] && i<sizeof(text));
  uint64_t offset=get(s,40);
  put(s,56,0);put(s,16,0);
  put(s,(unsigned)offset,(uintptr_t)lookup_item(text,handle));
  return 0;
}
int main(void) {
 unsigned fast=0,slow=0;
 for (unsigned k=0;k<1536;++k) {
  unsigned mode=k%3, encoding=(k/3)%2, changed=(k/6)%16, off=(k/96)%16;
  unsigned char image[40]={1,2,3}, a[96],b[96], ga[8],gb[8];
  memset(image+3,'X',36);image[39]=0;
  memcpy(image+3,"example.dll",12);
  if (changed<12) image[3+changed]^=1;
  if (encoding) for(unsigned j=3;j<40;++j) image[j]^=90;
  /* Always supply a terminator for the gateway's selected normalization. */
  image[39]=mode==1 ? 90 : 0;
  for(unsigned j=0;j<96;++j) a[j]=(unsigned char)(j*13+k);
  memset(ga,17,8);memcpy(gb,ga,8);
  put(a+off,0,(uintptr_t)image);put(a+off,56,mode);
  put(a+off,24,k%5==0 ? UINT64_MAX : (uint64_t)k*913);
  memcpy(b,a,96);
  guest=ga;calls=gateways=0;trace=0;
  uint64_t ra=before(a+off,0), ta=trace;unsigned ca=calls;
  guest=gb;calls=gateways=0;trace=0;
  uint64_t rb=after(b+off,0);
  if(ra!=rb || ta!=trace || ca!=calls || memcmp(a,b,96) || memcmp(ga,gb,8)) return 1;
  if(gateways) ++slow;else ++fast;
 }
 return !(fast && slow);
})";
  for (const char *Level : {"-O0", "-O2"}) {
    auto Executable = tmpFile(std::string("string-oracle") + Level +
                              test::executableSuffix());
    auto Built = exec(NEVERD_TEST_CLANG,
                      {"-std=c11", Level, "-fsanitize=undefined",
                       "-fsanitize-trap=all", Original.string(), After.string(),
                       Harness.string(), "-o", Executable.string()});
    ASSERT_TRUE(Built.ok()) << Built.err;
    auto Ran = exec(Executable.string(), {});
    EXPECT_TRUE(Ran.ok()) << Ran.exitCode << " " << Ran.err;
  }
}
std::string pointerProgram() {
  auto Text = program();
  cell_test::replace(Text, "declare i64 @opaque(ptr, ptr)",
                     "declare i64 @opaque(ptr, ptr)\n"
                     "declare i64 @clobber(ptr, ptr)");
  cell_test::replace(Text, "store i64 7, ptr %sel, align 1", R"(
  %image_slot = getelementptr i8, ptr %state, i64 24
  %image = load i64, ptr %image_slot, align 1
  %address = add i64 %image, 5
  %pointer = inttoptr i64 %address to ptr
  %target = load i64, ptr %pointer, align 1
  store i64 %target, ptr %sel, align 1
  call i64 @clobber(ptr %state, ptr %context))");
  cell_test::replace(Text, "store i64 1, ptr %count, align 1", "");
  return Text;
}
StateSourceContract pointerContract() {
  auto C = contract();
  auto &G = C.Gateways[0];
  G.FunctionPointer = StateSourceSlot{32, 8};
  G.Selector.reset();
  G.Imports[0].Selector = 0;
  G.Imports[0].Parameters = {StateSourceType::I32};
  G.Imports[0].PointerSource = StateSourcePointerSource{0, 5};
  G.Writes.push_back({{32, 8}, 0});
  C.Images = {{{24, 8}, true, std::vector<uint8_t>(24, 0)}};
  return C;
}

TEST(StateSource, RejectsAmbiguousOrIncompletePointerContractsBeforeMutation) {
  for (unsigned Case = 0; Case < 10; ++Case) {
    llvm::LLVMContext C;
    auto M = cell_test::parse(C, pointerProgram());
    ASSERT_TRUE(M);
    auto Contract = pointerContract();
    auto &G = Contract.Gateways[0];
    if (Case == 0)
      G.Selector = StateSourceSlot{32, 8};
    if (Case == 1)
      G.FunctionPointer->Bytes = 4;
    if (Case == 2)
      G.Imports[0].PointerSource.reset();
    if (Case == 3)
      G.Imports[0].PointerSource->Image = 1;
    if (Case == 4)
      G.Imports[0].PointerSource->Offset = 17;
    if (Case == 5)
      G.Imports.push_back(G.Imports[0]);
    if (Case == 6)
      G.Imports[0].Selector = 7;
    if (Case == 7)
      G.Imports[0].Name = "_Atomic";
    if (Case == 8)
      G.Imports[0].VariadicFixedParameters = 0;
    if (Case == 9)
      G.Imports[0].VariadicFixedParameters = 2;
    auto Before = cell_test::print(*M);
    auto R = prepareStateSourceRecovery(*M, Contract);
    EXPECT_FALSE(bool(R));
    if (!R)
      llvm::consumeError(R.takeError());
    EXPECT_EQ(Before, cell_test::print(*M));
  }
}

TEST(StateSource, PointerCandidatesRequireFullLoadsAndRetainAddressGuards) {
  for (const char *Triple :
       {"x86_64-pc-windows-msvc", "aarch64-unknown-linux-gnu"})
    for (unsigned Case = 0; Case < 5; ++Case) {
      llvm::LLVMContext C;
      auto Text = pointerProgram();
      if (Case == 1)
        cell_test::replace(Text, "%target = load i64, ptr %pointer, align 1",
                           "%narrow = load i32, ptr %pointer, align 1\n"
                           "%target = zext i32 %narrow to i64");
      if (Case == 2)
        cell_test::replace(Text, "add i64 %image, 5", "add i64 %image, 6");
      if (Case == 3)
        cell_test::replace(Text, "add i64 %image, 5", "add i64 %image, %x");
      if (Case == 4) {
        cell_test::replace(Text, "call i64 @clobber(ptr %state, ptr %context)",
                           "");
        cell_test::replace(Text, "%status = call i64 @gateway",
                           "call i64 @clobber(ptr %state, ptr %context)\n"
                           "%status = call i64 @gateway");
      }
      auto M = cell_test::parse(C, Text);
      ASSERT_TRUE(M);
      M->setTargetTriple(llvm::Triple(Triple));
      auto R = prepareStateSourceRecovery(*M, pointerContract());
      ASSERT_TRUE(bool(R)) << llvm::toString(R.takeError());
      optimize(*M);
      auto Before = cell_test::print(*M);
      auto E = finishStateSourceRecovery(*M, *R);
      ASSERT_FALSE(E) << llvm::toString(std::move(E));
      EXPECT_EQ(R->RewrittenCalls, unsigned(Case == 0)) << Before;
      EXPECT_EQ(R->GuardedCalls, unsigned(Case == 0));
      EXPECT_EQ(R->UnresolvedCalls, unsigned(Case != 0));
      optimize(*M);
      auto IR = cell_test::print(*M);
      EXPECT_NE(IR.find("call i64 @gateway"), std::string::npos);
      if (Case == 0) {
        EXPECT_NE(IR.find("ptrtoint (ptr @example_query to i64)"),
                  std::string::npos)
            << IR;
        EXPECT_NE(IR.find("call i32 @example_query"), std::string::npos);
      }
    }
}

TEST_F(StateSourceCompiled,
       PointerGuardsPreserveClobbersNullsAndFailureStatus) {
  if (!hasCrossTargetClang())
    GTEST_SKIP() << "Configured Clang is unavailable";
  llvm::LLVMContext C;
  auto M = cell_test::parse(C, pointerProgram());
  ASSERT_TRUE(M);
  M->getFunction("work")->setName("before");
  auto Original = tmpFile("pointer-before.ll");
  std::ofstream(Original) << test::printHostCompilerFixture(*M);
  M->getFunction("before")->setName("after");
  auto R = prepareStateSourceRecovery(*M, pointerContract());
  ASSERT_TRUE(bool(R)) << llvm::toString(R.takeError());
  optimize(*M);
  auto E = finishStateSourceRecovery(*M, *R);
  ASSERT_FALSE(E) << llvm::toString(std::move(E));
  ASSERT_EQ(R->RewrittenCalls, 1u) << cell_test::print(*M);
  ASSERT_EQ(R->GuardedCalls, 1u);
  optimize(*M);
  std::string Source;
  llvm::raw_string_ostream OS(Source);
  CEmitterOptions Options;
  Options.PreserveLLVMFunctionTypes = true;
  Options.UseUnalignedPointers = true;
  Options.ScalarPointers = CEmitterOptions::ScalarPointerSpelling::AliasTypes;
  ASSERT_TRUE(LLVMCEmitter().emit(*M, OS, Options));
  auto After = tmpFile("pointer-after.c");
  // Platform headers can overload a signed import with an unsigned variant.
  // Address guards must choose the declared signature, just as calls do.
  std::ofstream(After) << "#include <stdint.h>\n"
                          "int32_t example_query(uint32_t) "
                          "__attribute__((overloadable, enable_if(1, \"\")));\n"
                       << Source;
  auto Harness = tmpFile("pointer-oracle.c");
  std::ofstream(Harness) << R"(
#include <stdint.h>
#include <string.h>
extern uint64_t before(void *,void *), after(void *,void *);
static uint64_t get(const void *s,unsigned o) { uint64_t v;memcpy(&v,(const char*)s+o,8);return v; }
static void put(void *s,unsigned o,uint64_t v,unsigned n) { memcpy((char*)s+o,&v,n); }
static unsigned mode,calls,gateways;
static uint64_t trace;
static uint32_t *guest;
int32_t example_query(int32_t x) { ++calls;trace=trace*131+(uint32_t)x;*guest^=(uint32_t)x;return x^0x12345678U; }
static int32_t alternate(int32_t x) { ++calls;trace=trace*31+(uint32_t)x;*guest+=(uint32_t)x;return x^0x98765432U; }
uint64_t clobber(void *s,void *ctx) {
 (void)ctx;
 if(mode==1) put(s,32,(uintptr_t)alternate,8);
 if(mode==2) put(s,32,0,8);
 /* The cache changes after capture. Do not reload it to decide the call. */
 put((void*)(uintptr_t)get(s,24),5,(uintptr_t)alternate,8);
 return 0;
}
uint64_t gateway(void *s,void *ctx) {
 (void)ctx;++gateways;
 uint64_t target=get(s,32),x=get(s,8),off=get(s,40),width=get(s,48);
 if((uint32_t)get(s,56)!=1) return 19;
 if(!target) { put(s,16,0x11223344,4);return UINT64_C(0xfedcba9876543210); }
 put(s,56,0,8);put(s,32,0,8);
 int32_t v=((int32_t(*)(int32_t))(uintptr_t)target)((int32_t)x);
 put(s,(unsigned)off,(uint64_t)(int64_t)v,(unsigned)width);
 return 0;
}
uint64_t opaque(void *s,void *ctx) { (void)ctx;return get(s,0)&3; }
int main(void) {
 unsigned fast=0,slow=0;
 for(unsigned k=0;k<2048;k++) {
  unsigned char image[24],snapshot[24],a[80],b[80];
  for(unsigned j=0;j<80;j++) a[j]=(unsigned char)(k*7+j*13);
  memset(image,0x5a,sizeof(image));unsigned off=k&15;
  put(a+off,0,(uint64_t)k*0x192ad76fULL,8);
  put(a+off,24,(uintptr_t)image,8);put(a+off,56,(k/16)%3,8);
  memcpy(b,a,sizeof(a));mode=(k/48)%3;
  uintptr_t target=k&1 ? (uintptr_t)example_query : (uintptr_t)alternate;
  put(image,5,target,8);uint32_t ga=k,gb=k;
  guest=&ga;calls=gateways=0;trace=0;
  uint64_t ra=before(a+off,&ga),ta=trace;unsigned ca=calls;
  memcpy(snapshot,image,sizeof(image));put(image,5,target,8);
  guest=&gb;calls=gateways=0;trace=0;
  uint64_t rb=after(b+off,&gb);
  if(ra!=rb || ta!=trace || ca!=calls || ga!=gb || memcmp(a,b,sizeof(a)) || memcmp(image,snapshot,sizeof(image))) return 1;
  if(gateways) ++slow;else ++fast;
 }
 return !(fast && slow);
})";
  for (const char *Level : {"-O0", "-O2"}) {
    auto Executable = tmpFile(std::string("pointer-oracle") + Level +
                              test::executableSuffix());
    auto Built = exec(NEVERD_TEST_CLANG,
                      {"-std=c11", Level, "-fsanitize=undefined",
                       "-fsanitize-trap=all", Original.string(), After.string(),
                       Harness.string(), "-o", Executable.string()});
    ASSERT_TRUE(Built.ok()) << Built.err;
    auto Ran = exec(Executable.string(), {});
    EXPECT_TRUE(Ran.ok()) << Ran.exitCode << " " << Ran.err;
  }
}
std::string forwardedProgram(unsigned Width = 8) {
  std::string Text = R"(
declare i64 @gateway(ptr, ptr)
declare i64 @change_target(ptr, ptr)
define i64 @work(ptr %state, ptr %context) {
  %sel = getelementptr i8, ptr %state, i64 32
  %off = getelementptr i8, ptr %state, i64 40
  %width = getelementptr i8, ptr %state, i64 48
  store i32 7, ptr %sel, align 1
  call i64 @change_target(ptr %state, ptr %context)
  store i64 24, ptr %off, align 1
  store i64 WIDTH, ptr %width, align 1
  %status = call i64 @gateway(ptr %state, ptr %context)
  ret i64 %status
})";
  cell_test::replace(Text, "WIDTH", std::to_string(Width));
  return Text;
}
StateSourceContract forwardedContract(unsigned Target = 0) {
  auto C = contract();
  auto &G = C.Gateways[0];
  G.Arguments = {{0, 8}, {8, 8}, {16, 8}};
  G.Writes = {{{0, 8}, 0}, {{8, 8}, 0}, {{16, 8}, 0}, {{56, 8}, 0}};
  auto &I = G.Imports[0];
  I.Name.clear();
  I.TargetArgument = Target;
  I.Parameters = {StateSourceType::Pointer, StateSourceType::I8,
                  StateSourceType::Pointer, StateSourceType::I16};
  // The last operand has no bank slot. It is always supplied by the contract.
  I.Defaults = {0x8123};
  I.Result = StateSourceType::I16;
  std::swap(G.Arguments[0], G.Arguments[Target]);
  std::swap(I.Parameters[0], I.Parameters[Target]);
  return C;
}
TEST(StateSource, ForwardedCallsRejectAmbiguousTargetsBeforeMutation) {
  for (unsigned Case = 0; Case != 10; ++Case) {
    llvm::LLVMContext C;
    auto M = cell_test::parse(C, forwardedProgram());
    ASSERT_TRUE(M);
    auto Contract = forwardedContract();
    auto &G = Contract.Gateways[0];
    auto &I = G.Imports[0];
    if (Case == 0)
      I.Name = "invented_symbol";
    if (Case == 1)
      I.TargetArgument = 4;
    if (Case == 2)
      I.TargetArgument = 3;
    if (Case == 3)
      I.Parameters[0] = StateSourceType::U64;
    if (Case == 4)
      I.Defaults.clear();
    if (Case == 5)
      I.VariadicFixedParameters = 3;
    if (Case == 6)
      G.Arguments[0].Bytes = 4;
    if (Case == 7)
      I.PointerSource = StateSourcePointerSource{0, 0};
    if (Case == 8) {
      Contract.Images = {{{16, 8}, true, {0}}};
      G.Strings = {{0, 0, true}};
    }
    if (Case == 9)
      I.TargetArgument.reset();
    auto Before = cell_test::print(*M);
    auto R = prepareStateSourceRecovery(*M, Contract);
    EXPECT_FALSE(bool(R));
    if (!R)
      llvm::consumeError(R.takeError());
    EXPECT_EQ(Before, cell_test::print(*M));
  }
}
TEST(StateSource, ForwardedCallsKeepCapturedTargetsAndSignedSignatures) {
  for (unsigned Target = 0; Target != 3; ++Target)
    for (bool Big : {false, true}) {
      SCOPED_TRACE(::testing::Message()
                   << "target=" << Target << " big=" << Big);
      llvm::LLVMContext C;
      auto M = cell_test::parse(C, forwardedProgram());
      ASSERT_TRUE(M);
      if (Big)
        M->setDataLayout("E-p:64:64-i64:64");
      M->setTargetTriple(llvm::Triple(Big ? "aarch64_be-unknown-linux-gnu"
                                          : "x86_64-pc-windows-msvc"));
      auto R = prepareStateSourceRecovery(*M, forwardedContract(Target));
      ASSERT_TRUE(bool(R)) << llvm::toString(R.takeError());
      optimize(*M);
      auto E = finishStateSourceRecovery(*M, *R);
      ASSERT_FALSE(E) << llvm::toString(std::move(E));
      ASSERT_EQ(R->RewrittenCalls, 1u);
      ASSERT_EQ(R->GuardedCalls, 1u);
      ASSERT_EQ(R->UnresolvedCalls, 0u);
      optimize(*M);
      std::string Source;
      llvm::raw_string_ostream OS(Source);
      CEmitterOptions Options;
      Options.PreserveLLVMFunctionTypes = true;
      Options.UseUnalignedPointers = true;
      ASSERT_TRUE(LLVMCEmitter().emit(*M, OS, Options));
      auto Signature = Target == 2 ? "int16_t (*)(void*, int8_t, int16_t)"
                                   : "int16_t (*)(int8_t, void*, int16_t)";
      EXPECT_NE(Source.find(Signature), std::string::npos) << Source;
      EXPECT_NE(Source.find("gateway(state, context)"), std::string::npos);
      EXPECT_EQ(Source.find("invented_symbol"), std::string::npos);
    }
}
TEST(StateSource, ContextForwardingRejectsMissingOrIncompatibleGatewayABI) {
  for (unsigned Case = 0; Case != 6; ++Case) {
    SCOPED_TRACE(Case);
    llvm::LLVMContext C;
    auto Text = program();
    auto Contract = contract();
    Contract.Gateways[0].Imports[0].PassContext = true;
    if (Case < 5) {
      const char *Declarations[] = {"ptr", "ptr, i64", "ptr, ptr addrspace(1)",
                                    "ptr, ptr, ptr", "ptr, ..."};
      const char *Operands[] = {"ptr %state_pointer",
                                "ptr %state_pointer, i64 0",
                                "ptr %state_pointer, ptr addrspace(1) null",
                                "ptr %state_pointer, ptr %context, ptr null",
                                "ptr %state_pointer, ptr %context"};
      cell_test::replace(Text, "@gateway(ptr, ptr)",
                         std::string("@gateway(") + Declarations[Case] + ")");
      cell_test::replace(Text, "@gateway(ptr %state_pointer, ptr %context)",
                         std::string("@gateway(") + Operands[Case] + ")");
      if (Case == 4)
        cell_test::replace(Text, "call i64 @gateway",
                           "call i64 (ptr, ...) @gateway");
    } else {
      auto Other = Contract.Gateways[0].Imports[0];
      Other.Selector = 8;
      Other.Parameters = {StateSourceType::I64};
      Contract.Gateways[0].Imports.push_back(Other);
    }
    auto M = cell_test::parse(C, Text);
    ASSERT_TRUE(M);
    auto Before = cell_test::print(*M);
    auto R = prepareStateSourceRecovery(*M, Contract);
    ASSERT_FALSE(bool(R));
    llvm::consumeError(R.takeError());
    EXPECT_EQ(cell_test::print(*M), Before);
  }
}

TEST(StateSource, ContextForwardingKeepsPointerStringAndSignedArgumentIndices) {
  for (unsigned Case = 0; Case != 4; ++Case)
    for (const char *Triple :
         {"x86_64-pc-windows-msvc", "aarch64-unknown-linux-gnu"}) {
      SCOPED_TRACE(::testing::Message() << Case << " " << Triple);
      llvm::LLVMContext C;
      auto Text = Case == 0   ? program()
                  : Case == 1 ? forwardedProgram()
                  : Case == 2 ? pointerProgram()
                              : stringProgram();
      auto Contract = Case == 0   ? contract()
                      : Case == 1 ? forwardedContract()
                      : Case == 2 ? pointerContract()
                                  : stringContract();
      auto &I = Contract.Gateways[0].Imports[0];
      I.PassContext = true;
      if (Case == 0)
        I.Parameters[0] = StateSourceType::I32;
      auto M = cell_test::parse(C, Text);
      ASSERT_TRUE(M);
      M->setTargetTriple(llvm::Triple(Triple));
      auto R = prepareStateSourceRecovery(*M, Contract);
      ASSERT_TRUE(bool(R)) << llvm::toString(R.takeError());
      optimize(*M);
      auto E = finishStateSourceRecovery(*M, *R);
      ASSERT_FALSE(E) << llvm::toString(std::move(E));
      ASSERT_EQ(R->RewrittenCalls, 1u);
      optimize(*M);
      std::string Source;
      llvm::raw_string_ostream OS(Source);
      CEmitterOptions Options;
      Options.PreserveLLVMFunctionTypes = true;
      Options.UseUnalignedPointers = true;
      ASSERT_TRUE(LLVMCEmitter().emit(*M, OS, Options));
      if (Case == 0)
        EXPECT_NE(Source.find("int32_t example_query(void*, int32_t)"),
                  std::string::npos)
            << Source;
      if (Case == 1)
        EXPECT_NE(Source.find("int16_t (*)(void*, int8_t, void*, int16_t)"),
                  std::string::npos)
            << Source;
      if (Case == 3)
        EXPECT_NE(Source.find("lookup_item(context, \"example.dll\""),
                  std::string::npos)
            << Source;
    }
}

TEST_F(StateSourceCompiled,
       ContextForwardingPreservesReceiversAliasesAndReentry) {
  if (!hasCrossTargetClang())
    GTEST_SKIP() << "Configured Clang is unavailable";
  std::vector<std::string> Files;
  for (unsigned Mode = 0; Mode != 3; ++Mode) {
    llvm::LLVMContext C;
    auto M = cell_test::parse(C, forwardedProgram());
    ASSERT_TRUE(M);
    std::string Suffix = std::to_string(Mode);
    M->getFunction("work")->setName("before" + Suffix);
    auto Original = tmpFile("context-before" + Suffix + ".ll");
    std::ofstream(Original) << test::printHostCompilerFixture(*M);
    Files.push_back(Original.string());
    M->getFunction("before" + Suffix)->setName("after" + Suffix);
    auto Contract = forwardedContract();
    auto &G = Contract.Gateways[0];
    auto &I = G.Imports[0];
    I.PassContext = true;
    if (Mode != 1) {
      I.Name = Mode == 0 ? "receiver_query" : "receiver_variadic";
      I.TargetArgument.reset();
      G.Arguments = {{8, 8}, {16, 8}};
      I.Parameters = {StateSourceType::I8, StateSourceType::Pointer,
                      StateSourceType::I16};
      I.Defaults = {0, 0x8123};
      if (Mode == 2) {
        G.Arguments.push_back({24, 8});
        I.Parameters = {StateSourceType::I32, StateSourceType::Pointer,
                        StateSourceType::I32};
        I.Defaults.clear();
        I.VariadicFixedParameters = 1;
      }
    } else
      I.Defaults = {0, 0x8123};
    auto R = prepareStateSourceRecovery(*M, Contract);
    ASSERT_TRUE(bool(R)) << llvm::toString(R.takeError());
    optimize(*M);
    auto E = finishStateSourceRecovery(*M, *R);
    ASSERT_FALSE(E) << llvm::toString(std::move(E));
    ASSERT_EQ(R->RewrittenCalls, 1u);
    ASSERT_EQ(R->GuardedCalls, 1u);
    optimize(*M);
    std::string Source;
    llvm::raw_string_ostream OS(Source);
    CEmitterOptions Options;
    Options.PreserveLLVMFunctionTypes = true;
    Options.UseUnalignedPointers = true;
    Options.ScalarPointers = CEmitterOptions::ScalarPointerSpelling::AliasTypes;
    ASSERT_TRUE(LLVMCEmitter().emit(*M, OS, Options));
    auto After = tmpFile("context-after" + Suffix + ".c");
    std::ofstream(After) << Source;
    Files.push_back(After.string());
  }
  auto Harness = tmpFile("context-oracle.c");
  std::ofstream(Harness) << R"(
#include <stdint.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
extern uint64_t before0(void*,void*),before1(void*,void*),before2(void*,void*);
extern uint64_t after0(void*,void*),after1(void*,void*),after2(void*,void*);
static uint64_t get(const void *s,unsigned o) { uint64_t v;memcpy(&v,(const char*)s+o,8);return v; }
static void put(void *s,unsigned o,uint64_t v,unsigned n) { memcpy((char*)s+o,&v,n); }
struct receiver { uint64_t identity,value,trace; unsigned calls,depth; };
static struct receiver contexts[2];
static unsigned mode,change,calls,gateways;
static uint64_t trace;
static uint64_t (*run)(void*,void*);
static int16_t query(void *ctx,int32_t x,void *p,int32_t pad);
int16_t receiver_query(void *ctx,int8_t x,void *p,int16_t pad) { return query(ctx,x,p,pad); }
int16_t receiver_variadic(void *ctx,int32_t x,...) {
 va_list ap;va_start(ap,x);void *p=va_arg(ap,void*);int32_t pad=va_arg(ap,int);va_end(ap);
 return query(ctx,x,p,pad);
}
static int16_t query(void *ctx,int32_t x,void *p,int32_t pad) {
 ++calls;trace=trace*131+(uint64_t)(int64_t)x+(uint64_t)(int64_t)pad;
 struct receiver *r=ctx;
 if(r) {
  ++r->calls;r->trace=r->trace*31+r->identity;r->value+=(uint32_t)x;
  if(r->depth) {
   r->depth=0;
   unsigned char nested[64]={0};put(nested,0,(uintptr_t)receiver_query,8);
   put(nested,8,77,8);put(nested,24,0x8123,8);put(nested,56,mode==0 ? 2 : 3,8);
   struct receiver *other=r==contexts ? contexts+1 : contexts;
   uint64_t status=run(nested,other);r->trace^=status^get(nested,24);
  }
 }
 if(p) { uint64_t v;memcpy(&v,p,8);v^=(uint32_t)pad;memcpy(p,&v,8); }
 return (int16_t)((uint32_t)x^(uint32_t)pad^(r ? r->value : 0x9153));
}
uint64_t change_target(void *s,void *ctx) {
 if(ctx) ((struct receiver*)ctx)->value^=0xabc;
 if(change==1) put(s,32,9,8);
 if(change==2) put(s,0,0,8);
 return 0;
}
uint64_t gateway(void *s,void *ctx) {
 ++gateways;
 uint64_t target=get(s,0),x=get(s,8),p=get(s,16),off=get(s,40),width=get(s,48),pad=get(s,24);
 uint32_t n=(uint32_t)get(s,56);
 if((uint32_t)get(s,32)!=7 || (mode==0 && (n<1 || n>2)) ||
    (mode==1 && (n<2 || n>3 || !target)) || (mode==2 && n!=3)) {
  put(s,24,0x123456,3);return UINT64_C(0xfedcba9876543210);
 }
 put(s,0,0,8);put(s,8,0,8);put(s,16,0,8);put(s,56,0,8);
 int16_t v;
 if(mode==0) v=receiver_query(ctx,(int8_t)x,n==2 ? (void*)(uintptr_t)p : 0,(int16_t)0x8123);
 else if(mode==1) v=((int16_t(*)(void*,int8_t,void*,int16_t))(uintptr_t)target)(ctx,(int8_t)x,n==3 ? (void*)(uintptr_t)p : 0,(int16_t)0x8123);
 else v=receiver_variadic(ctx,(int32_t)x,(void*)(uintptr_t)p,(int32_t)pad);
 put(s,(unsigned)off,(uint64_t)(int64_t)v,(unsigned)width);return 0;
}
int main(void) {
 uint64_t (*before[])(void*,void*)={before0,before1,before2};
 uint64_t (*after[])(void*,void*)={after0,after1,after2};
 unsigned fast=0,slow=0,reentry=0;
 for(mode=0;mode<3;mode++) for(unsigned k=0;k<2048;k++) {
  unsigned char a[80],b[80];for(unsigned j=0;j<80;j++) a[j]=(unsigned char)(k*7+j*13);
  unsigned off=k&15;change=(k/16)%3;
  struct receiver initial[2]={{17,k,0,0,(k/2)&1},{83,k+3,0,0,0}},observed[2];
  void *ctx=k%5 ? contexts+(k%2) : 0;
  put(a+off,0,(uintptr_t)receiver_query,8);put(a+off,8,(uint64_t)k*0x192ad76fULL,8);
  put(a+off,16,(uintptr_t)&contexts[(k/2)%2].value,8);put(a+off,24,0x8123,8);
  put(a+off,56,(k/64)%5,8);memcpy(b,a,80);memcpy(contexts,initial,sizeof(contexts));
  run=before[mode];calls=gateways=0;trace=0;uint64_t ra=run(a+off,ctx),ta=trace;
  unsigned ca=calls;memcpy(observed,contexts,sizeof(contexts));
  run=after[mode];calls=gateways=0;trace=0;memcpy(contexts,initial,sizeof(contexts));
  uint64_t rb=run(b+off,ctx);
  if(ra!=rb || ta!=trace || ca!=calls || memcmp(contexts,observed,sizeof(contexts)) || memcmp(a,b,80)) {
   fprintf(stderr,"mode=%u case=%u status=%llu/%llu trace=%llu/%llu calls=%u/%u context=%d bank=%d\n",
     mode,k,(unsigned long long)ra,(unsigned long long)rb,(unsigned long long)ta,(unsigned long long)trace,
     ca,calls,memcmp(contexts,observed,sizeof(contexts)),memcmp(a,b,80));return 1;
  }
  if(gateways) ++slow;else ++fast;if(calls>1) ++reentry;
 }
 return !(fast && slow && reentry);
})";
  for (const char *Level : {"-O0", "-O2"}) {
    auto Executable = tmpFile(std::string("context-oracle") + Level +
                              test::executableSuffix());
    std::vector<std::string> Args = {"-std=c11", Level, "-Werror=uninitialized",
                                     "-fsanitize=undefined",
                                     "-fsanitize-trap=all"};
    Args.insert(Args.end(), Files.begin(), Files.end());
    Args.insert(Args.end(), {Harness.string(), "-o", Executable.string()});
    auto Built = exec(NEVERD_TEST_CLANG, Args);
    ASSERT_TRUE(Built.ok()) << Built.err;
    auto Ran = exec(Executable.string(), {});
    EXPECT_TRUE(Ran.ok()) << Ran.exitCode << " " << Ran.err;
  }
}
TEST_F(StateSourceCompiled,
       ForwardedCallsPreservePaddingSnapshotsAndFullFailureEffects) {
  if (!hasCrossTargetClang())
    GTEST_SKIP() << "Configured Clang is unavailable";
  std::vector<std::string> Files;
  for (unsigned Width : {1, 2, 4, 8}) {
    llvm::LLVMContext C;
    auto M = cell_test::parse(C, forwardedProgram(Width));
    ASSERT_TRUE(M);
    auto Suffix = std::to_string(Width);
    M->getFunction("work")->setName("before" + Suffix);
    auto Original = tmpFile("forwarded-before" + Suffix + ".ll");
    std::ofstream(Original) << test::printHostCompilerFixture(*M);
    Files.push_back(Original.string());
    M->getFunction("before" + Suffix)->setName("after" + Suffix);
    auto Contract = forwardedContract();
    // The guest pointer is optional as well as the always-present padding.
    Contract.Gateways[0].Imports[0].Defaults = {0, 0x8123};
    auto R = prepareStateSourceRecovery(*M, Contract);
    ASSERT_TRUE(bool(R)) << llvm::toString(R.takeError());
    optimize(*M);
    auto E = finishStateSourceRecovery(*M, *R);
    ASSERT_FALSE(E) << llvm::toString(std::move(E));
    ASSERT_EQ(R->RewrittenCalls, 1u);
    ASSERT_EQ(R->GuardedCalls, 1u);
    optimize(*M);
    std::string Source;
    llvm::raw_string_ostream OS(Source);
    CEmitterOptions Options;
    Options.PreserveLLVMFunctionTypes = true;
    Options.UseUnalignedPointers = true;
    Options.ScalarPointers = CEmitterOptions::ScalarPointerSpelling::AliasTypes;
    ASSERT_TRUE(LLVMCEmitter().emit(*M, OS, Options));
    auto After = tmpFile("forwarded-after" + Suffix + ".c");
    std::ofstream(After) << Source;
    Files.push_back(After.string());
  }
  auto Harness = tmpFile("forwarded-oracle.c");
  std::ofstream(Harness) << R"(
#include <stdint.h>
#include <string.h>
extern uint64_t before1(void*,void*),before2(void*,void*),before4(void*,void*),before8(void*,void*);
extern uint64_t after1(void*,void*),after2(void*,void*),after4(void*,void*),after8(void*,void*);
static uint64_t get(const void *s,unsigned o) { uint64_t v;memcpy(&v,(const char*)s+o,8);return v; }
static void put(void *s,unsigned o,uint64_t v,unsigned n) { memcpy((char*)s+o,&v,n); }
static unsigned mode,calls,gateways;
static uint64_t trace;
static int16_t first(int8_t x,void *p,int16_t pad) {
 ++calls;trace=trace*131+(uint64_t)(int64_t)x+(uint64_t)(int64_t)pad;
 if(p) { uint32_t v;memcpy(&v,p,4);v^=(uint32_t)x;memcpy(p,&v,4); }
 return (int16_t)((uint16_t)pad^(uint16_t)x);
}
static int16_t second(int8_t x,void *p,int16_t pad) {
 ++calls;trace=trace*31+(uint64_t)(int64_t)x;
 if(p) { uint32_t v;memcpy(&v,p,4);v+=(uint32_t)pad;memcpy(p,&v,4); }
 return (int16_t)((uint16_t)x+(uint16_t)pad);
}
uint64_t change_target(void *s,void *ctx) {
 (void)ctx;
 if(mode==1) put(s,0,(uintptr_t)second,8);
 if(mode==2) put(s,0,0,8);
 if(mode==3) put(s,32,9,8);
 return 0;
}
uint64_t gateway(void *s,void *ctx) {
 (void)ctx;++gateways;
 uint64_t target=get(s,0),x=get(s,8),p=get(s,16),off=get(s,40),width=get(s,48);
 uint32_t count=(uint32_t)get(s,56);
 if((uint32_t)get(s,32)!=7 || count<2 || count>3 || !target) {
  put(s,24,0x123456,3);return UINT64_C(0xfedcba9876543210);
 }
 put(s,0,0,8);put(s,8,0,8);put(s,16,0,8);put(s,56,0,8);
 int16_t value=((int16_t(*)(int8_t,void*,int16_t))(uintptr_t)target)(
   (int8_t)x,count==3 ? (void*)(uintptr_t)p : 0,(int16_t)0x8123);
 put(s,(unsigned)off,(uint64_t)(int64_t)value,(unsigned)width);
 return 0;
}
int main(void) {
 uint64_t (*a_fn[])(void*,void*)={before1,before2,before4,before8};
 uint64_t (*b_fn[])(void*,void*)={after1,after2,after4,after8};
 unsigned fast=0,slow=0;
 for(unsigned w=0;w<4;w++) for(unsigned k=0;k<2048;k++) {
  unsigned char a[80],b[80];uint32_t guest=k,ga;
  for(unsigned j=0;j<80;j++) a[j]=(unsigned char)(k*7+j*13);
  unsigned off=k&15;mode=(k/16)%4;
  put(a+off,0,k&1 ? (uintptr_t)first : (uintptr_t)second,8);
  put(a+off,8,(uint64_t)k*0x192ad76fULL,8);
  put(a+off,16,(uintptr_t)&guest,8);put(a+off,56,(k/64)%5,8);
  memcpy(b,a,sizeof(a));calls=gateways=0;trace=0;
  uint64_t ra=a_fn[w](a+off,&guest),ta=trace;unsigned ca=calls;ga=guest;
  guest=k;calls=gateways=0;trace=0;
  uint64_t rb=b_fn[w](b+off,&guest);
  if(ra!=rb || ta!=trace || ca!=calls || ga!=guest || memcmp(a,b,sizeof(a))) return 1;
  if(gateways) ++slow;else ++fast;
 }
 return !(fast && slow);
})";
  for (const char *Level : {"-O0", "-O2"}) {
    auto Executable = tmpFile(std::string("forwarded-oracle") + Level +
                              test::executableSuffix());
    std::vector<std::string> Args = {"-std=c11", Level, "-fsanitize=undefined",
                                     "-fsanitize-trap=all"};
    Args.insert(Args.end(), Files.begin(), Files.end());
    Args.insert(Args.end(), {Harness.string(), "-o", Executable.string()});
    auto Built = exec(NEVERD_TEST_CLANG, Args);
    ASSERT_TRUE(Built.ok()) << Built.err;
    auto Ran = exec(Executable.string(), {});
    EXPECT_TRUE(Ran.ok()) << Ran.exitCode << " " << Ran.err;
  }
}
TEST_F(StateSourceCompiled,
       VariadicCallsKeepPromotedWidthsSignsAndPointerTypes) {
  if (!hasCrossTargetClang())
    GTEST_SKIP() << "Configured Clang is unavailable";
  llvm::LLVMContext C;
  auto Text = program();
  cell_test::replace(Text, "store i64 1, ptr %count",
                     "store i64 6, ptr %count");
  cell_test::replace(
      Text, "%status = call i64 @gateway",
      "store i64 1, ptr %r, align 1\n  %status = call i64 @gateway");
  auto M = cell_test::parse(C, Text);
  ASSERT_TRUE(M);
  M->getFunction("work")->setName("before");
  auto Original = tmpFile("variadic-before.ll");
  std::ofstream(Original) << test::printHostCompilerFixture(*M);
  M->getFunction("before")->setName("after");
  auto Contract = contract();
  auto &G = Contract.Gateways[0];
  G.Arguments = {{8, 8}, {8, 8}, {8, 8}, {0, 8}, {16, 8}, {24, 8}};
  auto &Import = G.Imports[0];
  Import.Name = "example_variadic";
  Import.Parameters = {StateSourceType::I32, StateSourceType::I32,
                       StateSourceType::U32, StateSourceType::I64,
                       StateSourceType::U64, StateSourceType::Pointer};
  Import.VariadicFixedParameters = 1;
  auto R = prepareStateSourceRecovery(*M, Contract);
  ASSERT_TRUE(bool(R)) << llvm::toString(R.takeError());
  optimize(*M);
  auto E = finishStateSourceRecovery(*M, *R);
  ASSERT_FALSE(E) << llvm::toString(std::move(E));
  ASSERT_EQ(R->RewrittenCalls, 1u);
  optimize(*M);
  std::string Source;
  llvm::raw_string_ostream OS(Source);
  CEmitterOptions Options;
  Options.PreserveLLVMFunctionTypes = true;
  Options.UseUnalignedPointers = true;
  Options.ScalarPointers = CEmitterOptions::ScalarPointerSpelling::AliasTypes;
  ASSERT_TRUE(LLVMCEmitter().emit(*M, OS, Options));
  EXPECT_NE(Source.find("int32_t example_variadic(int32_t, ...)"),
            std::string::npos);
  EXPECT_TRUE(Source.find("(uint64_t)(1)") != std::string::npos ||
              Source.find("1ull") != std::string::npos)
      << Source;
  auto After = tmpFile("variadic-after.c");
  std::ofstream(After) << Source;
  auto Harness = tmpFile("variadic-oracle.c");
  std::ofstream(Harness) << R"(
#include <stdint.h>
#include <stdarg.h>
#include <string.h>
extern uint64_t before(void *,void *), after(void *,void *);
static uint64_t get(const void *s,unsigned o) { uint64_t v;memcpy(&v,(const char*)s+o,8);return v; }
static void put(void *s,unsigned o,uint64_t v,unsigned n) { memcpy((char*)s+o,&v,n); }
static unsigned token,calls;
static uint32_t *guest;
static uint64_t trace;
int32_t example_variadic(int32_t tag, ...) {
 va_list ap;va_start(ap,tag);
 int32_t s=va_arg(ap,int32_t);uint32_t u=va_arg(ap,uint32_t);
 int64_t w=va_arg(ap,int64_t);uint64_t z=va_arg(ap,uint64_t);
 void *p=va_arg(ap,void*);va_end(ap);
 ++calls;trace=(uint64_t)(int64_t)s ^ ((uint64_t)u<<32) ^ (uint64_t)w ^ z;
 *guest+=(uint32_t)trace + (p==&token ? 71 : 123);
 return (int32_t)((uint32_t)tag ^ (uint32_t)(trace>>32));
}
uint64_t gateway(void *s,void *ctx) {
 (void)ctx;uint64_t x=get(s,8),p=get(s,24),off=get(s,40),width=get(s,48);
 put(s,56,0,8);
 int32_t v=example_variadic((int32_t)x,(int32_t)x,(uint32_t)x,(int64_t)get(s,0),get(s,16),(void*)(uintptr_t)p);
 put(s,(unsigned)off,(uint64_t)(int64_t)v,(unsigned)width);return 0;
}
uint64_t opaque(void *s,void *ctx) { (void)ctx;return get(s,0)&3; }
int main(void) {
 for(unsigned k=0;k<1024;k++) {
  unsigned char a[80],b[80];for(unsigned j=0;j<80;j++) a[j]=(unsigned char)(k*7+j*13);
  unsigned off=k&15;put(a+off,0,UINT64_MAX-(uint64_t)k*0x192ad76f,8);
  put(a+off,24,(uintptr_t)&token,8);memcpy(b,a,sizeof(a));
  uint32_t ga=k,gb=k;guest=&ga;trace=0;calls=0;
  uint64_t ra=before(a+off,&ga),ta=trace;unsigned ca=calls;
  guest=&gb;trace=0;calls=0;uint64_t rb=after(b+off,&gb);
  if(ra!=rb || ta!=trace || ca!=calls || ca!=1 || ga!=gb || memcmp(a,b,sizeof(a))) return 1;
 }
 return 0;
})";
  for (const char *Level : {"-O0", "-O2"}) {
    auto Executable = tmpFile(std::string("variadic-oracle") + Level +
                              test::executableSuffix());
    auto Built = exec(NEVERD_TEST_CLANG,
                      {"-std=c11", Level, "-fsanitize=undefined",
                       "-fsanitize-trap=all", Original.string(), After.string(),
                       Harness.string(), "-o", Executable.string()});
    ASSERT_TRUE(Built.ok()) << Built.err;
    auto Ran = exec(Executable.string(), {});
    EXPECT_TRUE(Ran.ok()) << Ran.exitCode << " " << Ran.err;
  }
}
StateSourceContract narrowContract() {
  auto C = contract();
  auto &G = C.Gateways[0];
  G.Selector = {32, 8};
  G.ArgumentCount = {56, 8};
  G.Arguments = {{8, 8}, {8, 8}, {8, 8}, {8, 8}, {24, 8}};
  G.Imports.clear();
  unsigned N = 0;
  for (auto Result : {StateSourceType::I8, StateSourceType::U8,
                      StateSourceType::I16, StateSourceType::U16}) {
    StateSourceImport I;
    I.Selector = 7 + N;
    I.Name = "narrow_query" + std::to_string(N++);
    I.Parameters = {StateSourceType::I8, StateSourceType::U8,
                    StateSourceType::I16, StateSourceType::U16,
                    StateSourceType::Pointer};
    I.Result = Result;
    I.Defaults = {0xd3, 0x8001, 0xabcd, 0};
    G.Imports.push_back(std::move(I));
  }
  return C;
}
std::string narrowProgram(unsigned Selector, unsigned Bytes) {
  auto Text = program();
  cell_test::replace(Text, "store i64 1, ptr %count, align 1", "");
  cell_test::replace(Text, "store i64 7, ptr %sel",
                     "store i64 " + std::to_string(Selector) + ", ptr %sel");
  cell_test::replace(Text, "store i64 4, ptr %width",
                     "store i64 " + std::to_string(Bytes) + ", ptr %width");
  Text.resize(Text.find("next:\n") + 6);
  return Text + "  %value = load i64, ptr %r, align 1\nret i64 %value\n}\n";
}

TEST(StateSource, NarrowSignaturesAndDefaultsKeepTheirDeclaredTypes) {
  const char *Types[] = {"int8_t", "uint8_t", "int16_t", "uint16_t"};
  for (bool BigEndian : {false, true})
    for (unsigned N = 0; N < 4; ++N) {
      llvm::LLVMContext C;
      auto M = cell_test::parse(C, narrowProgram(7 + N, 8));
      ASSERT_TRUE(M);
      M->setTargetTriple(llvm::Triple(BigEndian ? "aarch64_be-unknown-linux-gnu"
                                                : "x86_64-pc-windows-msvc"));
      M->setDataLayout(BigEndian ? "E-p:64:64" : "e-p:64:64");
      auto R = prepareStateSourceRecovery(*M, narrowContract());
      ASSERT_TRUE(bool(R)) << llvm::toString(R.takeError());
      optimize(*M);
      auto E = finishStateSourceRecovery(*M, *R);
      ASSERT_FALSE(E) << llvm::toString(std::move(E));
      EXPECT_EQ(R->RewrittenCalls, 1u);
      EXPECT_EQ(R->GuardedCalls, 1u);
      EXPECT_FALSE(llvm::verifyModule(*M));
      optimize(*M);
      std::string Source;
      llvm::raw_string_ostream OS(Source);
      CEmitterOptions Options;
      Options.PreserveLLVMFunctionTypes = true;
      ASSERT_TRUE(LLVMCEmitter().emit(*M, OS, Options));
      EXPECT_NE(Source.find(std::string(Types[N]) + " narrow_query" +
                            std::to_string(N) +
                            "(int8_t, uint8_t, int16_t, uint16_t, void*)"),
                std::string::npos)
          << Source;
      EXPECT_NE(Source.find("gateway(state, context)"), std::string::npos);
    }
}

TEST_F(StateSourceCompiled, NarrowDefaultsPreserveSignedResultsAndWholeBanks) {
  if (!hasCrossTargetClang())
    GTEST_SKIP() << "Configured Clang is unavailable";
  std::string Text, Harness;
  llvm::raw_string_ostream H(Harness);
  H << "#include <stdint.h>\n#include <string.h>\n";
  unsigned Index = 0;
  for (unsigned T = 0; T < 4; ++T)
    for (unsigned Bytes : {1u, 2u, 4u, 8u}) {
      auto F = narrowProgram(7 + T, Bytes);
      if (Index)
        F.erase(0, F.find("define i64"));
      cell_test::replace(F, "@work(", "@before" + std::to_string(Index) + "(");
      Text += F;
      H << "extern uint64_t before" << Index << "(void*,void*), after" << Index
        << "(void*,void*);\n";
      ++Index;
    }
  llvm::LLVMContext C;
  auto M = cell_test::parse(C, Text);
  ASSERT_TRUE(M);
  auto Original = tmpFile("narrow-before.ll");
  std::ofstream(Original) << test::printHostCompilerFixture(*M);
  for (unsigned N = 0; N < Index; ++N)
    M->getFunction("before" + std::to_string(N))
        ->setName("after" + std::to_string(N));
  auto R = prepareStateSourceRecovery(*M, narrowContract());
  ASSERT_TRUE(bool(R)) << llvm::toString(R.takeError());
  optimize(*M);
  auto E = finishStateSourceRecovery(*M, *R);
  ASSERT_FALSE(E) << llvm::toString(std::move(E));
  ASSERT_EQ(R->RewrittenCalls, Index);
  ASSERT_EQ(R->GuardedCalls, Index);
  optimize(*M);
  std::string Source;
  llvm::raw_string_ostream OS(Source);
  CEmitterOptions Options;
  Options.PreserveLLVMFunctionTypes = true;
  Options.UseUnalignedPointers = true;
  Options.ScalarPointers = CEmitterOptions::ScalarPointerSpelling::AliasTypes;
  ASSERT_TRUE(LLVMCEmitter().emit(*M, OS, Options));
  auto After = tmpFile("narrow-after.c");
  std::ofstream(After) << Source;
  H << R"(
static uint64_t get(const void *s,unsigned o) { uint64_t v;memcpy(&v,(const char*)s+o,8);return v; }
static void put(void *s,unsigned o,uint64_t v,unsigned n) { memcpy((char*)s+o,&v,n); }
static unsigned calls,gateways,token;
static uint64_t trace;
static uint32_t *guest;
static uint64_t arguments(int8_t a,uint8_t b,int16_t c,uint16_t d,void *p) {
 ++calls;trace=(uint64_t)(int64_t)a ^ ((uint64_t)b<<8) ^ ((uint64_t)(int64_t)c<<16) ^ ((uint64_t)d<<32);
 trace^=p==&token ? UINT64_C(0x7fa304d19):0;
 *guest+=(uint32_t)trace;return trace ^ (trace>>23);
}
int8_t narrow_query0(int8_t a,uint8_t b,int16_t c,uint16_t d,void *p) { return (int8_t)arguments(a,b,c,d,p); }
uint8_t narrow_query1(int8_t a,uint8_t b,int16_t c,uint16_t d,void *p) { return (uint8_t)arguments(a,b,c,d,p); }
int16_t narrow_query2(int8_t a,uint8_t b,int16_t c,uint16_t d,void *p) { return (int16_t)arguments(a,b,c,d,p); }
uint16_t narrow_query3(int8_t a,uint8_t b,int16_t c,uint16_t d,void *p) { return (uint16_t)arguments(a,b,c,d,p); }
uint64_t gateway(void *s,void *ctx) {
 (void)ctx;++gateways;
 uint64_t x=get(s,8),n=get(s,56),off=get(s,40),width=get(s,48),which=get(s,32);
 if(n<1 || n>5) { put(s,(unsigned)off,UINT64_C(0x97115243eabcd821),(unsigned)width);*guest+=731;return UINT64_C(0x8000000070000019); }
 int8_t a=(int8_t)x;uint8_t b=n>1 ? (uint8_t)x:0xd3;
 int16_t c=n>2 ? (int16_t)x:(int16_t)0x8001;uint16_t d=n>3 ? (uint16_t)x:0xabcd;
 void *p=n>4 ? (void*)(uintptr_t)get(s,24):0;put(s,56,0,8);
 uint64_t v;
 switch(which) {
 case 7:v=(uint64_t)(int64_t)narrow_query0(a,b,c,d,p);break;
 case 8:v=narrow_query1(a,b,c,d,p);break;
 case 9:v=(uint64_t)(int64_t)narrow_query2(a,b,c,d,p);break;
 case 10:v=narrow_query3(a,b,c,d,p);break;
 default:return 91;
 }
 put(s,(unsigned)off,v,(unsigned)width);return 0;
}
typedef uint64_t (*run)(void*,void*);
)";
  for (const char *Prefix : {"before", "after"}) {
    H << "static run " << Prefix << "_functions[]={";
    for (unsigned N = 0; N < Index; ++N)
      H << (N ? "," : "") << Prefix << N;
    H << "};\n";
  }
  H << R"(
int main(void) {
 unsigned fast=0,slow=0;
 for(unsigned f=0;f<16;f++) for(unsigned k=0;k<512;k++) {
  unsigned char a[80],b[80];for(unsigned j=0;j<80;j++) a[j]=(unsigned char)(k*7+j*13);
  unsigned off=k&15;put(a+off,0,UINT64_MAX-(uint64_t)k*0x192ad76f,8);
  put(a+off,24,(uintptr_t)&token,8);put(a+off,56,(k/16)%8,8);memcpy(b,a,sizeof(a));
  uint32_t ga=k,gb=k;guest=&ga;calls=gateways=0;trace=0;
  uint64_t ra=before_functions[f](a+off,&ga),ta=trace;unsigned ca=calls;
  guest=&gb;calls=gateways=0;trace=0;uint64_t rb=after_functions[f](b+off,&gb);
  if(ra!=rb || ta!=trace || ca!=calls || ga!=gb || memcmp(a,b,sizeof(a))) return 1;
  if(gateways) ++slow;else ++fast;
 }
 return !(fast && slow);
})";
  auto Oracle = tmpFile("narrow-oracle.c");
  std::ofstream(Oracle) << Harness;
  for (const char *Level : {"-O0", "-O2"}) {
    auto Executable = tmpFile(std::string("narrow-oracle") + Level +
                              test::executableSuffix());
    auto Built = exec(NEVERD_TEST_CLANG,
                      {"-std=c11", Level, "-fsanitize=undefined",
                       "-fsanitize-trap=all", Original.string(), After.string(),
                       Oracle.string(), "-o", Executable.string()});
    ASSERT_TRUE(Built.ok()) << Built.err;
    auto Ran = exec(Executable.string(), {});
    EXPECT_TRUE(Ran.ok()) << Ran.exitCode << " " << Ran.err;
  }
}

std::string voidResultProgram(llvm::StringRef Body) {
  auto Text = program();
  Text.resize(Text.find("next:\n") + 6);
  return Text + Body.str() + "\n}\n";
}
StateSourceContract voidResultContract() {
  auto C = contract();
  // The fixture writes full words; read the same words on both byte orders.
  C.Gateways[0].Selector = {32, 8};
  C.Gateways[0].ArgumentCount = {56, 8};
  auto &I = C.Gateways[0].Imports[0];
  I.Name = "example_release";
  I.Result = StateSourceType::Void;
  I.VoidResult = StateSourceVoidResult::Unspecified;
  return C;
}

TEST(StateSource, UnspecifiedVoidResultsRequireCompleteOverwriteOnEveryPath) {
  const std::pair<const char *, bool> Cases[] = {
      {"store i64 123, ptr %r, align 1\nret i64 0", true},
      {R"(store i16 123, ptr %r, align 1
          %upper = getelementptr i8, ptr %r, i64 2
          store i16 456, ptr %upper, align 1
          ret i64 0)",
       true},
      {R"(%other = load i64, ptr %a, align 1
          store i64 %other, ptr %r, align 1
          ret i64 0)",
       true},
      {R"(%choose = icmp eq i64 %x, 0
          br i1 %choose, label %one, label %two
        one: store i32 111, ptr %r, align 1
          ret i64 0
        two: store i32 222, ptr %r, align 1
          ret i64 0)",
       true},
      {R"(%choose = icmp eq i64 %x, 0
          br i1 %choose, label %one, label %two
        one: store i32 111, ptr %r, align 1
          ret i64 0
        two: ret i64 0)",
       false},
      {"store i16 123, ptr %r, align 1\nret i64 0", false},
      {R"(%seen = load i32, ptr %r, align 1
          store i32 %seen, ptr %context, align 1
          store i32 0, ptr %r, align 1
          ret i64 0)",
       false},
      {R"(%seen = call i64 @opaque(ptr %state, ptr %context)
          store i32 0, ptr %r, align 1
          ret i64 %seen)",
       false},
      {R"(%choose = icmp eq i64 %x, 0
          %address = select i1 %choose, ptr %r, ptr %context
          store i32 0, ptr %address, align 1
          ret i64 0)",
       false},
      {R"(br label %again
        again:
          %choose = icmp eq i64 %x, 0
          br i1 %choose, label %again, label %done
        done: store i32 0, ptr %r, align 1
          ret i64 0)",
       false},
      {"ret i64 0", false}};
  for (auto Target : {std::pair{"x86_64-pc-windows-msvc", "e-p:64:64"},
                      std::pair{"aarch64_be-unknown-linux-gnu", "E-p:64:64"}})
    for (auto [Body, Expected] : Cases) {
      SCOPED_TRACE(Body);
      llvm::LLVMContext C;
      auto M = cell_test::parse(C, voidResultProgram(Body));
      ASSERT_TRUE(M);
      M->setTargetTriple(llvm::Triple(Target.first));
      M->setDataLayout(Target.second);
      auto R = prepareStateSourceRecovery(*M, voidResultContract());
      ASSERT_TRUE(bool(R)) << llvm::toString(R.takeError());
      optimize(*M);
      auto E = finishStateSourceRecovery(*M, *R);
      ASSERT_FALSE(E) << llvm::toString(std::move(E));
      EXPECT_EQ(R->RewrittenCalls, unsigned(Expected)) << cell_test::print(*M);
      EXPECT_EQ(R->UnresolvedCalls, unsigned(!Expected));
    }
}

TEST_F(StateSourceCompiled, DeadVoidCaptureKeepsFullBankAndFailureResults) {
  if (!hasCrossTargetClang())
    GTEST_SKIP() << "Configured Clang is unavailable";
  auto Text = voidResultProgram(R"(
  %low = trunc i64 %x to i16
  store i16 %low, ptr %r, align 1
  %upper = getelementptr i8, ptr %r, i64 2
  store i16 7123, ptr %upper, align 1
  %value = load i64, ptr %r, align 1
  ret i64 %value)");
  cell_test::replace(Text, "store i64 1, ptr %count, align 1", "");
  llvm::LLVMContext C;
  auto M = cell_test::parse(C, Text);
  ASSERT_TRUE(M);
  M->getFunction("work")->setName("before");
  auto Original = tmpFile("void-before.ll");
  std::ofstream(Original) << test::printHostCompilerFixture(*M);
  M->getFunction("before")->setName("after");
  auto R = prepareStateSourceRecovery(*M, voidResultContract());
  ASSERT_TRUE(bool(R)) << llvm::toString(R.takeError());
  optimize(*M);
  auto E = finishStateSourceRecovery(*M, *R);
  ASSERT_FALSE(E) << llvm::toString(std::move(E));
  ASSERT_EQ(R->RewrittenCalls, 1u);
  ASSERT_EQ(R->GuardedCalls, 1u);
  optimize(*M);
  std::string Source;
  llvm::raw_string_ostream OS(Source);
  CEmitterOptions Options;
  Options.PreserveLLVMFunctionTypes = true;
  Options.UseUnalignedPointers = true;
  Options.ScalarPointers = CEmitterOptions::ScalarPointerSpelling::AliasTypes;
  ASSERT_TRUE(LLVMCEmitter().emit(*M, OS, Options));
  auto After = tmpFile("void-after.c");
  std::ofstream(After) << Source;
  auto Harness = tmpFile("void-oracle.c");
  std::ofstream(Harness) << R"(
#include <stdint.h>
#include <string.h>
extern uint64_t before(void *,void *), after(void *,void *);
static uint64_t get(const void *s,unsigned o) { uint64_t v;memcpy(&v,(const char*)s+o,8);return v; }
static void put(void *s,unsigned o,uint64_t v,unsigned n) { memcpy((char*)s+o,&v,n); }
static unsigned calls,gateways;
static uint64_t capture,trace;
static uint32_t *guest;
void example_release(uint64_t x) { ++calls;trace=x;*guest+=(uint32_t)x; }
uint64_t gateway(void *s,void *ctx) {
 (void)ctx;++gateways;
 uint64_t x=get(s,8),off=get(s,40),width=get(s,48);
 if((uint32_t)get(s,56)!=1) { put(s,(unsigned)off,capture,(unsigned)width);return 19; }
 put(s,56,0,8);example_release(x);
 /* An unspecified physical return value, not a fabricated void zero. */
 put(s,(unsigned)off,capture,(unsigned)width);return 0;
}
int main(void) {
 unsigned fast=0,slow=0;
 for(unsigned k=0;k<2048;k++) {
  unsigned char a[80],b[80];for(unsigned j=0;j<80;j++) a[j]=(unsigned char)(k*7+j*13);
  unsigned off=k&15;put(a+off,0,UINT64_MAX-(uint64_t)k*0x192ad76f,8);
  put(a+off,56,(k/16)%3,8);memcpy(b,a,sizeof(a));
  capture=UINT64_C(0xeda1704b99ab1653) ^ ((uint64_t)k*7937);
  uint32_t ga=k,gb=k;guest=&ga;calls=gateways=0;trace=0;
  uint64_t ra=before(a+off,&ga),ta=trace;unsigned ca=calls;
  guest=&gb;calls=gateways=0;trace=0;uint64_t rb=after(b+off,&gb);
  if(ra!=rb || ta!=trace || ca!=calls || ga!=gb || memcmp(a,b,sizeof(a))) return 1;
  if(gateways) ++slow;else ++fast;
 }
 return !(fast && slow);
})";
  for (const char *Level : {"-O0", "-O2"}) {
    auto Executable =
        tmpFile(std::string("void-oracle") + Level + test::executableSuffix());
    auto Built = exec(NEVERD_TEST_CLANG,
                      {"-std=c11", Level, "-fsanitize=undefined",
                       "-fsanitize-trap=all", Original.string(), After.string(),
                       Harness.string(), "-o", Executable.string()});
    ASSERT_TRUE(Built.ok()) << Built.err;
    auto Ran = exec(Executable.string(), {});
    EXPECT_TRUE(Ran.ok()) << Ran.exitCode << " " << Ran.err;
  }
}

TEST_F(StateSourceCompiled,
       VoidArgumentResultKeepsCapturedBitsAndNativeEffects) {
  if (!hasCrossTargetClang())
    GTEST_SKIP() << "Configured Clang is unavailable";
  std::string Text, Harness;
  llvm::raw_string_ostream H(Harness);
  H << "#include <stdint.h>\n#include <string.h>\n";
  auto Contract = voidResultContract();
  auto &G = Contract.Gateways[0];
  G.Arguments.push_back({24, 8});
  G.Writes.insert(G.Writes.end(), {{{8, 8}, 0}, {{24, 8}, 0}});
  G.Imports[0].Parameters = {StateSourceType::I8, StateSourceType::Pointer};
  G.Imports[0].VoidResult = StateSourceVoidResult::Zero;
  G.Imports[0].VoidResultArgument = 0;
  auto Second = G.Imports[0];
  Second.Selector = 8;
  Second.VoidResultArgument = 1;
  G.Imports.push_back(Second);
  unsigned Index = 0;
  for (unsigned Argument : {0u, 1u})
    for (unsigned Bytes : {1u, 2u, 4u, 8u}) {
      auto F = voidResultProgram(
          "%value = load i64, ptr %r, align 1\nret i64 %value");
      cell_test::replace(F, "store i64 1, ptr %count, align 1", "");
      cell_test::replace(F, "store i64 7, ptr %sel",
                         "store i64 " + std::to_string(7 + Argument) +
                             ", ptr %sel");
      cell_test::replace(F, "store i64 4, ptr %width",
                         "store i64 " + std::to_string(Bytes) + ", ptr %width");
      // Both byte orders admit the same explicitly captured operand contract.
      for (const char *Layout : {"e-p:64:64", "E-p:64:64"}) {
        llvm::LLVMContext C;
        auto M = cell_test::parse(C, F);
        ASSERT_TRUE(M);
        M->setDataLayout(Layout);
        auto R = prepareStateSourceRecovery(*M, Contract);
        ASSERT_TRUE(bool(R)) << llvm::toString(R.takeError());
        optimize(*M);
        auto E = finishStateSourceRecovery(*M, *R);
        ASSERT_FALSE(E) << llvm::toString(std::move(E));
        EXPECT_EQ(R->RewrittenCalls, 1u);
        EXPECT_EQ(R->GuardedCalls, 1u);
        EXPECT_FALSE(llvm::verifyModule(*M, &llvm::errs()));
      }
      if (Index)
        F.erase(0, F.find("define i64"));
      cell_test::replace(F, "@work(", "@before" + std::to_string(Index) + "(");
      Text += F;
      H << "extern uint64_t before" << Index << "(void*,void*), after" << Index
        << "(void*,void*);\n";
      ++Index;
    }
  llvm::LLVMContext C;
  auto M = cell_test::parse(C, Text);
  ASSERT_TRUE(M);
  auto Original = tmpFile("void-argument-before.ll");
  std::ofstream(Original) << test::printHostCompilerFixture(*M);
  for (unsigned I = 0; I < Index; ++I)
    M->getFunction("before" + std::to_string(I))
        ->setName("after" + std::to_string(I));
  auto R = prepareStateSourceRecovery(*M, Contract);
  ASSERT_TRUE(bool(R)) << llvm::toString(R.takeError());
  optimize(*M);
  auto E = finishStateSourceRecovery(*M, *R);
  ASSERT_FALSE(E) << llvm::toString(std::move(E));
  ASSERT_EQ(R->RewrittenCalls, Index);
  ASSERT_EQ(R->GuardedCalls, Index);
  optimize(*M);
  std::string Source;
  llvm::raw_string_ostream OS(Source);
  CEmitterOptions Options;
  Options.PreserveLLVMFunctionTypes = true;
  Options.UseUnalignedPointers = true;
  Options.ScalarPointers = CEmitterOptions::ScalarPointerSpelling::AliasTypes;
  ASSERT_TRUE(LLVMCEmitter().emit(*M, OS, Options));
  auto After = tmpFile("void-argument-after.c");
  std::ofstream(After) << Source;
  H << R"(
static uint64_t get(const void *s,unsigned o) { uint64_t v;memcpy(&v,(const char*)s+o,8);return v; }
static void put(void *s,unsigned o,uint64_t v,unsigned n) { memcpy((char*)s+o,&v,n); }
static unsigned calls,gateways,token;
static uint64_t trace;
static uint32_t *guest;
void example_release(int8_t x,void *p) { ++calls;trace=(uint64_t)(int64_t)x ^ (uintptr_t)p;*guest+=(uint32_t)trace; }
uint64_t gateway(void *s,void *ctx) {
 (void)ctx;++gateways;
 uint64_t x=get(s,8),p=get(s,24),off=get(s,40),width=get(s,48),which=get(s,32);
 if(get(s,56)!=2) { put(s,(unsigned)off,UINT64_C(0x97115243eabcd821),(unsigned)width);*guest+=731;return UINT64_C(0x8000000070000019); }
 put(s,56,0,8);put(s,8,0,8);put(s,24,0,8);
 example_release((int8_t)x,(void*)(uintptr_t)p);
 put(s,(unsigned)off,which==7 ? x:p,(unsigned)width);return 0;
}
typedef uint64_t (*run)(void*,void*);
)";
  for (const char *Prefix : {"before", "after"}) {
    H << "static run " << Prefix << "_functions[]={";
    for (unsigned I = 0; I < Index; ++I)
      H << (I ? "," : "") << Prefix << I;
    H << "};\n";
  }
  H << R"(
int main(void) {
 unsigned fast=0,slow=0;
 for(unsigned f=0;f<8;f++) for(unsigned k=0;k<512;k++) {
  unsigned char a[80],b[80];for(unsigned j=0;j<80;j++) a[j]=(unsigned char)(k*7+j*13);
  unsigned off=k&15;put(a+off,0,UINT64_MAX-(uint64_t)k*0x192ad76f,8);
  put(a+off,24,(uintptr_t)&token,8);put(a+off,56,(k/16)%4,8);memcpy(b,a,sizeof(a));
  uint32_t ga=k,gb=k;guest=&ga;calls=gateways=0;trace=0;
  uint64_t ra=before_functions[f](a+off,&ga),ta=trace;unsigned ca=calls;
  guest=&gb;calls=gateways=0;trace=0;uint64_t rb=after_functions[f](b+off,&gb);
  if(ra!=rb || ta!=trace || ca!=calls || ga!=gb || memcmp(a,b,sizeof(a))) return 1;
  if(gateways) ++slow;else ++fast;
 }
 return !(fast && slow);
})";
  auto Oracle = tmpFile("void-argument-oracle.c");
  std::ofstream(Oracle) << Harness;
  for (const char *Level : {"-O0", "-O2"}) {
    auto Executable = tmpFile(std::string("void-argument-oracle") + Level +
                              test::executableSuffix());
    auto Built = exec(NEVERD_TEST_CLANG,
                      {"-std=c11", Level, "-fsanitize=undefined",
                       "-fsanitize-trap=all", Original.string(), After.string(),
                       Oracle.string(), "-o", Executable.string()});
    ASSERT_TRUE(Built.ok()) << Built.err;
    auto Ran = exec(Executable.string(), {});
    EXPECT_TRUE(Ran.ok()) << Ran.exitCode << " " << Ran.err;
  }
}

TEST_F(StateSourceCompiled, LocalBankPreservesEveryByteAndOpaqueBoundaries) {
  llvm::LLVMContext C;
  auto M = cell_test::parse(C, R"(
declare i64 @observe_bank(ptr, ptr)
declare void @observe_guest(ptr, i64)
define i64 @work(ptr %state, ptr %context) {
entry:
  %n = load i8, ptr %context, align 1
  %bound = and i8 %n, 15
  %word = getelementptr i8, ptr %state, i64 3
  %tail = getelementptr i8, ptr %state, i64 58
  br label %loop
loop:
  %i = phi i8 [0, %entry], [%next, %continue]
  %more = icmp ult i8 %i, %bound
  br i1 %more, label %body, label %done
body:
  %old = load i64, ptr %state, align 1
  %slice = trunc i64 %old to i16
  %changed = xor i16 %slice, 1903
  store i16 %changed, ptr %word, align 1
  %wide = load i64, ptr %state, align 1
  %narrow = trunc i64 %wide to i32
  store i32 %narrow, ptr %tail, align 1
  call void @observe_guest(ptr %context, i64 %wide)
  %status = call i64 @observe_bank(ptr %state, ptr %context)
  %bad = icmp ne i64 %status, 0
  br i1 %bad, label %failed, label %continue
continue:
  %after = load i64, ptr %state, align 1
  %adjusted = add i64 %after, 1
  store i64 %adjusted, ptr %state, align 1
  %next = add i8 %i, 1
  br label %loop
done:
  %result = load i64, ptr %state, align 1
  ret i64 %result
failed:
  ret i64 %status
}
define i64 @patch(ptr %state, ptr %context) {
entry:
  %seed = load i64, ptr %state, align 1
  %input = load i32, ptr %context, align 1
  %out = getelementptr i8, ptr %state, i64 8
  %half = getelementptr i8, ptr %state, i64 10
  %byte = getelementptr i8, ptr %state, i64 14
  %sum = add i64 %seed, 19
  store i64 %sum, ptr %out, align 1
  store i32 %input, ptr %out, align 1
  %upper = lshr i64 %seed, 37
  %h = trunc i64 %upper to i16
  store i16 %h, ptr %half, align 1
  %b = trunc i64 %seed to i8
  store i8 %b, ptr %byte, align 1
  %result = load i64, ptr %out, align 1
  ret i64 %result
})");
  ASSERT_TRUE(M);
  M->getFunction("work")->setName("before");
  M->getFunction("patch")->setName("before_patch");
  auto Original = tmpFile("local-bank-before.ll");
  std::ofstream(Original) << test::printHostCompilerFixture(*M);
  M->getFunction("before")->setName("after");
  M->getFunction("before_patch")->setName("after_patch");
  StateSourceContract Contract;
  Contract.StateBytes = 64;
  Contract.IsolatedBank = true;
  Contract.LocalizeBank = true;
  auto R = prepareStateSourceRecovery(*M, Contract);
  ASSERT_TRUE(bool(R)) << llvm::toString(R.takeError());
  optimize(*M);
  auto E = finishStateSourceRecovery(*M, *R);
  ASSERT_FALSE(E) << llvm::toString(std::move(E));
  EXPECT_GE(R->LocalizedFunctions, 1u);
  optimize(*M);
  std::string Source;
  llvm::raw_string_ostream OS(Source);
  CEmitterOptions Options;
  Options.PreserveLLVMFunctionTypes = true;
  Options.UseUnalignedPointers = true;
  Options.ScalarPointers = CEmitterOptions::ScalarPointerSpelling::AliasTypes;
  ASSERT_TRUE(LLVMCEmitter().emit(*M, OS, Options));
  auto After = tmpFile("local-bank-after.c");
  std::ofstream(After) << Source;
  auto Harness = tmpFile("local-bank-oracle.c");
  std::ofstream(Harness) << R"(
#include <stdint.h>
#include <string.h>
extern uint64_t before(void *,void *), after(void *,void *);
extern uint64_t before_patch(void *,void *), after_patch(void *,void *);
static uint64_t trace[64];
static unsigned calls;
static void *expected_bank;
static int bad_identity;
void observe_guest(void *ctx, uint64_t x) {
 unsigned char *p=ctx;
 trace[calls++]=x;
 p[1]^=(unsigned char)x;
}
uint64_t observe_bank(void *s, void *ctx) {
 unsigned char *p=s, *c=ctx;
 if(s!=expected_bank) bad_identity=1;
 uint64_t hash=17;
 for(unsigned j=0;j<64;j++) hash=(hash*131)^p[j];
 trace[calls++]=hash;
 /* Mutate both cached and otherwise untouched bytes at an opaque boundary. */
 for(unsigned j=0;j<64;j++) p[j]^=(unsigned char)(j*13+c[1]);
 c[2]++;
 return c[3] && c[2]>=c[3] ? UINT64_C(0x8000000000000017) : 0;
}
int main(void) {
 for(unsigned k=0;k<2048;k++) {
  unsigned char a[96], b[96], ca[8], cb[8];
  for(unsigned j=0;j<96;j++) a[j]=(unsigned char)(k*29+j*71);
  for(unsigned j=0;j<8;j++) ca[j]=(unsigned char)(k*7+j);
  ca[0]=k&15; ca[2]=0; ca[3]=(k>>4)&7;
  memcpy(b,a,96); memcpy(cb,ca,8);
  unsigned off=1+(k&15);uint64_t saved[64];
  calls=0; expected_bank=a+off;
  uint64_t ra=before(a+off,ca);unsigned n=calls;
  memcpy(saved,trace,n*8);
  calls=0; expected_bank=b+off;
  uint64_t rb=after(b+off,cb);
  if(bad_identity || ra!=rb || calls!=n || memcmp(saved,trace,n*8) ||
     memcmp(a,b,96) || memcmp(ca,cb,8)) return 1;
  if(before_patch(a+off,ca)!=after_patch(b+off,cb) || memcmp(a,b,96)) return 2;
 }
 return 0;
})";
  for (const char *Level : {"-O0", "-O2"}) {
    auto Executable = tmpFile(std::string("local-bank-oracle") + Level +
                              test::executableSuffix());
    auto Built = exec(NEVERD_TEST_CLANG,
                      {"-std=c11", Level, "-fsanitize=undefined",
                       "-fsanitize-trap=all", Original.string(), After.string(),
                       Harness.string(), "-o", Executable.string()});
    ASSERT_TRUE(Built.ok()) << Built.err << '\n' << Source;
    auto Ran = exec(Executable.string(), {});
    EXPECT_TRUE(Ran.ok()) << Ran.exitCode << " " << Ran.err << '\n' << Source;
  }
}

TEST(StateSource, LocalBankRefusalsRetainTheOriginalStateImplementation) {
  for (bool Enabled : {false, true}) {
    llvm::LLVMContext C;
    auto M = cell_test::parse(C, R"(
define i64 @work(ptr %state, i1 %choose) {
entry:
  %other = getelementptr i8, ptr %state, i64 8
  %selected = select i1 %choose, ptr %state, ptr %other
  store i64 7, ptr %selected, align 1
  ret i64 0
})");
    ASSERT_TRUE(M);
    StateSourceContract Contract;
    Contract.StateBytes = 16;
    Contract.IsolatedBank = true;
    Contract.LocalizeBank = Enabled;
    auto R = prepareStateSourceRecovery(*M, Contract);
    ASSERT_TRUE(bool(R)) << llvm::toString(R.takeError());
    const auto Before = cell_test::print(*M);
    auto E = finishStateSourceRecovery(*M, *R);
    ASSERT_FALSE(E) << llvm::toString(std::move(E));
    EXPECT_EQ(R->LocalizedFunctions, 0u);
    EXPECT_EQ(cell_test::print(*M), Before);
  }
}

TEST_F(StateSourceCompiled, BankEffectsPreserveCallsLoopsAndCompleteState) {
  // Three contracts: an unconstrained fallback, a bounded partial result,
  // and a fixed-width fallback different from the captured descriptor.
  for (unsigned Mode = 0; Mode != 3; ++Mode) {
    llvm::LLVMContext C;
    auto M = cell_test::parse(C, R"(
declare i64 @gateway(ptr, ptr)
declare i64 @opaque(ptr, ptr)
define i64 @step(ptr %state, ptr %context, i32 %depth) {
entry:
  %old = load i64, ptr %state, align 1
  %cell = getelementptr i8, ptr %state, i64 24
  %small = trunc i32 %depth to i16
  store i16 %small, ptr %cell, align 1
  %more = icmp ne i32 %depth, 0
  br i1 %more, label %recurse, label %done
recurse:
  %next = sub i32 %depth, 1
  %ignored = call i64 @step(ptr %state, ptr %context, i32 %next)
  br label %done
done:
  %again = load i64, ptr %state, align 1
  %delta = xor i64 %again, %old
  ret i64 %delta
}
define i64 @work(ptr %state, ptr %context) {
entry:
  %arg = getelementptr i8, ptr %state, i64 8
  %result = getelementptr i8, ptr %state, i64 16
  %sel = getelementptr i8, ptr %state, i64 32
  %off = getelementptr i8, ptr %state, i64 40
  %width = getelementptr i8, ptr %state, i64 48
  %count = getelementptr i8, ptr %state, i64 56
  %seed = load i64, ptr %state, align 1
  store i64 %seed, ptr %arg, align 1
  %n = load i8, ptr %context, align 1
  %short = and i8 %n, 3
  %depth = zext i8 %short to i32
  %d = call i64 @step(ptr %state, ptr %context, i32 %depth)
  %stable = load i64, ptr %state, align 1
  %copy = load i64, ptr %arg, align 1
  store i64 %copy, ptr %arg, align 1
  store i64 99, ptr %sel, align 1
  store i64 16, ptr %off, align 1
  store i64 4, ptr %width, align 1
  store i64 1, ptr %count, align 1
  %oldresult = load i64, ptr %result, align 1
  %status = call i64 @gateway(ptr %state, ptr %context)
  %newresult = load i64, ptr %result, align 1
  %after = load i64, ptr %state, align 1
  store i64 %seed, ptr %state, align 1
  %guest = getelementptr i8, ptr %context, i64 8
  %v0 = xor i64 %oldresult, %newresult
  %v1 = xor i64 %stable, %after
  %v2 = xor i64 %v0, %v1
  %v3 = xor i64 %v2, %d
  store i64 %v3, ptr %guest, align 1
  %bad = icmp ne i64 %status, 0
  br i1 %bad, label %failed, label %loop
failed:
  ret i64 %status
loop:
  %i = phi i8 [0, %entry], [%next, %join]
  %value = load i64, ptr %state, align 1
  %flag = and i8 %n, 4
  %choose = icmp ne i8 %flag, 0
  br i1 %choose, label %left, label %right
left:
  %incremented = add i64 %value, 17
  store i64 %incremented, ptr %state, align 1
  br label %join
right:
  %part = getelementptr i8, ptr %state, i64 4
  %word = zext i8 %n to i32
  store i32 %word, ptr %part, align 1
  br label %join
join:
  %joined = load i64, ptr %state, align 1
  store i64 %joined, ptr %arg, align 1
  %next = add i8 %i, 1
  %more = icmp ult i8 %next, %short
  br i1 %more, label %loop, label %boundary
boundary:
  %o = call i64 @opaque(ptr %state, ptr %context)
  %last = load i64, ptr %state, align 1
  %combined = xor i64 %last, %o
  ret i64 %combined
})");
    ASSERT_TRUE(M);
    M->getFunction("work")->setName("before");
    M->getFunction("step")->setName("before_step");
    auto Original = tmpFile("bank-effects-before.ll");
    std::ofstream(Original) << test::printHostCompilerFixture(*M);
    M->getFunction("before")->setName("after");
    M->getFunction("before_step")->setName("after_step");
    auto Contract = contract();
    Contract.LocalizeBank = true;
    Contract.Gateways[0].Selector->Bytes = 8;
    Contract.Gateways[0].ArgumentCount->Bytes = 8;
    if (Mode)
      Contract.Gateways[0].FallbackWrites = StateSourceFallbackWrites{};
    if (Mode == 2)
      Contract.Gateways[0].FallbackWrites->ResultBytes = 8;
    auto R = prepareStateSourceRecovery(*M, Contract);
    ASSERT_TRUE(bool(R)) << llvm::toString(R.takeError());
    optimize(*M);
    auto E = finishStateSourceRecovery(*M, *R);
    ASSERT_FALSE(E) << llvm::toString(std::move(E));
    EXPECT_GE(R->ForwardedStateLoads, 3u);
    if (Mode)
      EXPECT_GE(R->RedundantStateStores, 1u);
    EXPECT_GE(R->BoundedStateCalls, Mode ? 3u : 2u);
    optimize(*M);
    std::string Source;
    llvm::raw_string_ostream OS(Source);
    CEmitterOptions Options;
    Options.PreserveLLVMFunctionTypes = true;
    Options.UseUnalignedPointers = true;
    Options.ScalarPointers = CEmitterOptions::ScalarPointerSpelling::AliasTypes;
    ASSERT_TRUE(LLVMCEmitter().emit(*M, OS, Options));
    auto After = tmpFile("bank-effects-after.c");
    std::ofstream(After) << Source;
    auto Harness = tmpFile("bank-effects-oracle.c");
    std::ofstream(Harness) << "#define MODE " << Mode << R"(
#include <stdint.h>
#include <string.h>
extern uint64_t before(void *,void *), after(void *,void *);
static uint64_t trace[8];
static unsigned calls;
static void *expected_bank;
static int bad_identity;
uint64_t gateway(void *s, void *ctx) {
 unsigned char *p=s, *c=ctx;
 if(s!=expected_bank) bad_identity=1;
 uint64_t hash=17, value=UINT64_C(0xfedcba9876543210), zero=0;
 /* Even a write-bounded gateway may observe every bank byte. */
 for(unsigned j=0;j<64;j++) hash=(hash*131)^p[j];
 trace[calls++]=hash;
 value^=hash;
 memcpy(p+16,&value,MODE==2?8:4);memcpy(p+56,&zero,8);
 if(MODE==0) { p[0]^=c[0];p[63]^=91; }
 c[1]^=(unsigned char)hash;
 return c[0]&8 ? UINT64_C(0x8000000000000017) : 0;
}
uint64_t opaque(void *s, void *ctx) {
 unsigned char *p=s, *c=ctx;
 if(s!=expected_bank) bad_identity=1;
 uint64_t hash=19;
 for(unsigned j=0;j<64;j++) hash=(hash*131)^p[j];
 trace[calls++]=hash;
 for(unsigned j=0;j<64;j++) p[j]^=(unsigned char)(j*13+c[1]);
 c[2]++;
 return hash;
}
int main(void) {
 uint64_t rng=UINT64_C(0xb46f295347cd01e3);
 for(unsigned k=0;k<2048;k++) {
  unsigned char a[96],b[96],ca[24],cb[24];
  for(unsigned j=0;j<96;j++) {
   rng^=rng<<13;rng^=rng>>7;rng^=rng<<17;
   a[j]=(unsigned char)rng;
  }
  for(unsigned j=0;j<24;j++) ca[j]=(unsigned char)(k*7+j);
  ca[0]=k&15;
  memcpy(b,a,96);memcpy(cb,ca,24);
  unsigned off=1+(k&15);uint64_t saved[8];
  calls=0;expected_bank=a+off;
  uint64_t ra=before(a+off,ca);unsigned n=calls;
  memcpy(saved,trace,n*8);
  calls=0;expected_bank=b+off;
  uint64_t rb=after(b+off,cb);
  if(bad_identity || ra!=rb || calls!=n || memcmp(saved,trace,n*8) ||
     memcmp(a,b,96) || memcmp(ca,cb,24)) return 1;
 }
 return 0;
})";
    for (const char *Level : {"-O0", "-O2"}) {
      auto Executable = tmpFile(std::string("bank-effects-oracle") + Level +
                                test::executableSuffix());
      auto Built =
          exec(NEVERD_TEST_CLANG,
               {"-std=c11", Level, "-fsanitize=undefined",
                "-fsanitize-trap=all", Original.string(), After.string(),
                Harness.string(), "-o", Executable.string()});
      ASSERT_TRUE(Built.ok()) << Built.err << '\n' << Source;
      auto Ran = exec(Executable.string(), {});
      EXPECT_TRUE(Ran.ok())
          << "mode=" << Mode << " " << Ran.exitCode << " " << Ran.err << '\n'
          << Source;
    }
  }
}

TEST(StateSource, BankEffectsDoNotTrustInterposableOrIndirectCallees) {
  for (bool Indirect : {false, true}) {
    llvm::LLVMContext C;
    std::string IR = R"(
define weak i64 @replaceable(ptr %state, ptr %context) {
  ret i64 0
}
define i64 @work(ptr %state, ptr %context, ptr %callback) {
  %old = load i64, ptr %state, align 1
  %status = call i64 CALLEE(ptr %state, ptr %context)
  %new = load i64, ptr %state, align 1
  %delta = xor i64 %old, %new
  ret i64 %delta
})";
    IR.replace(IR.find("CALLEE"), 6, Indirect ? "%callback" : "@replaceable");
    auto M = cell_test::parse(C, IR);
    ASSERT_TRUE(M);
    StateSourceContract Contract;
    Contract.StateBytes = 64;
    Contract.IsolatedBank = true;
    Contract.LocalizeBank = true;
    auto R = prepareStateSourceRecovery(*M, Contract);
    ASSERT_TRUE(bool(R)) << llvm::toString(R.takeError());
    auto E = finishStateSourceRecovery(*M, *R);
    ASSERT_FALSE(E) << llvm::toString(std::move(E));
    EXPECT_EQ(R->ForwardedStateLoads, 0u);
    EXPECT_EQ(R->RedundantStateStores, 0u);
    EXPECT_EQ(R->BoundedStateCalls, 0u);
  }
}

} // namespace
