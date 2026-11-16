//===- LLVMCAllocaTests.cpp - LLVM allocation C semantics ----------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Execute original LLVM and generated C with independently observed storage.
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "neverd/backend/c/LLVMC/LLVMCEmitter.h"

#include "llvm/ADT/SmallString.h"
#include "llvm/AsmParser/Parser.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Verifier.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/FileUtilities.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/Program.h"
#include "llvm/Support/SourceMgr.h"
#include "llvm/TargetParser/Host.h"
#include "llvm/TargetParser/Triple.h"
#include "llvm/Transforms/Utils/Cloning.h"

using namespace neverd;

namespace {

std::string moduleText(const llvm::Module &Module) {
  std::string Text;
  llvm::raw_string_ostream(Text) << Module;
  return Text;
}

void comparePrograms(llvm::StringRef IR, llvm::StringRef Driver,
                     bool PreserveTypes = false,
                     llvm::StringRef SelectedFunction = {},
                     bool RequireRuntimeAllocation = true) {
  llvm::LLVMContext Context;
  llvm::SMDiagnostic Diagnostic;
  auto Module = llvm::parseAssemblyString(IR, Diagnostic, Context);
  ASSERT_TRUE(Module) << Diagnostic.getMessage().str();
  Module->setTargetTriple(llvm::Triple(llvm::sys::getDefaultTargetTriple()));
  ASSERT_FALSE(llvm::verifyModule(*Module, &llvm::errs()));
  const std::string Before = moduleText(*Module);
  CEmitterOptions Options;
  Options.TheArch = Arch::X64;
  Options.EmitIncludes = false;
  Options.PreserveLLVMFunctionTypes = PreserveTypes;
  std::string Text;
  llvm::raw_string_ostream Stream(Text);
  const auto *Only = SelectedFunction.empty()
                         ? nullptr
                         : Module->getFunction(SelectedFunction);
  ASSERT_TRUE(SelectedFunction.empty() || Only);
  ASSERT_TRUE(
      LLVMCEmitter().emit(*Module, Stream, Options, nullptr, nullptr, Only));
  EXPECT_EQ(moduleText(*Module), Before) << "C projection mutated original IR";
  // Do not execute the known underallocation produced by the old emitter.
  if (RequireRuntimeAllocation)
    ASSERT_NE(Text.find("__builtin_alloca("), std::string::npos) << Text;
  auto Original = llvm::CloneModule(*Module);
  for (auto &Global : Original->globals())
    if (Global.hasInitializer())
      Global.setName("original_" + Global.getName());
  for (auto &Function : *Original)
    if (!Function.isDeclaration())
      Function.setName("original_" + Function.getName());

  auto Compiler = llvm::sys::findProgramByName("clang");
  ASSERT_TRUE(Compiler) << "clang is required for native allocation oracles";
  llvm::SmallString<128> Input, LLVMInput, Output, Errors;
  ASSERT_FALSE(llvm::sys::fs::createTemporaryFile("neverd-alloca", "c", Input));
  ASSERT_FALSE(
      llvm::sys::fs::createTemporaryFile("neverd-alloca", "ll", LLVMInput));
  ASSERT_FALSE(
      llvm::sys::fs::createTemporaryFile("neverd-alloca", "exe", Output));
  ASSERT_FALSE(
      llvm::sys::fs::createTemporaryFile("neverd-alloca", "err", Errors));
  llvm::FileRemover RemoveInput(Input), RemoveLLVM(LLVMInput),
      RemoveOutput(Output), RemoveErrors(Errors);
  std::error_code EC;
  {
    llvm::raw_fd_ostream File(Input, EC);
    ASSERT_FALSE(EC);
    File << "#include <stdint.h>\n#include <stdbool.h>\n#include <stddef.h>\n"
         << Text << Driver;
  }
  {
    llvm::raw_fd_ostream File(LLVMInput, EC);
    ASSERT_FALSE(EC);
    File << *Original;
  }
  for (llvm::StringRef Optimization : {"-O0", "-O2"}) {
    const llvm::StringRef Args[] = {*Compiler,
                                    "-std=c11",
                                    Optimization,
                                    "-Werror=incompatible-pointer-types",
                                    "-Werror=int-conversion",
                                    "-fsanitize=undefined",
                                    "-fsanitize-trap=undefined",
                                    Input,
                                    LLVMInput,
                                    "-o",
                                    Output};
    const std::optional<llvm::StringRef> Redirects[] = {
        std::nullopt, std::nullopt, Errors.str()};
    std::string Error;
    const int Status = llvm::sys::ExecuteAndWait(*Compiler, Args, std::nullopt,
                                                 Redirects, 60, 0, &Error);
    auto Log = llvm::MemoryBuffer::getFile(Errors);
    ASSERT_EQ(Status, 0) << Error << (Log ? (*Log)->getBuffer().str() : "")
                         << Text;
    EXPECT_EQ(llvm::sys::ExecuteAndWait(Output, {Output}, std::nullopt, {}, 30,
                                        0, &Error),
              0)
        << Error << Text << Driver.str();
  }
}

} // namespace

TEST(LLVMCAlloca, FloatingViewsOfIntegerStoragePreserveObjectBits) {
  comparePrograms(R"IR(
declare void @observe(ptr)
define i64 @float_bits(double %value) {
  %slot = alloca i64, align 8
  store double %value, ptr %slot, align 8
  call void @observe(ptr %slot)
  %bits = load i64, ptr %slot, align 8
  ret i64 %bits
}
define double @bits_float(i64 %bits) {
  %slot = alloca i64, align 8
  store i64 %bits, ptr %slot, align 8
  call void @observe(ptr %slot)
  %value = load double, ptr %slot, align 8
  ret double %value
}
define i32 @single_bits(float %value) {
  %slot = alloca float, align 4
  store float %value, ptr %slot, align 4
  call void @observe(ptr %slot)
  %bits = load i32, ptr %slot, align 4
  ret i32 %bits
}
define float @bits_single(i32 %bits) {
  %slot = alloca float, align 4
  store i32 %bits, ptr %slot, align 4
  call void @observe(ptr %slot)
  %value = load float, ptr %slot, align 4
  ret float %value
}
)IR",
                  R"C(
void observe(void *p) { __asm__ volatile("" : : "r"(p) : "memory"); }
extern uint64_t original_float_bits(double);
extern double original_bits_float(uint64_t);
extern uint32_t original_single_bits(float);
extern float original_bits_single(uint32_t);
int main(void) {
  uint64_t values[] = {0, 1, UINT64_C(0x8000000000000000),
    UINT64_C(0x3ff4000000000000), UINT64_C(0x7ff8123456789abc)};
  for (unsigned i = 0; i < sizeof(values) / sizeof(values[0]); ++i) {
    double value; __builtin_memcpy(&value, &values[i], 8);
    if (float_bits(value) != original_float_bits(value)) return 1;
    double actual = bits_float(values[i]);
    double expected = original_bits_float(values[i]);
    if (__builtin_memcmp(&actual, &expected, 8)) return 2;
  }
  uint32_t singles[] = {0, 1, 0x80000000u, 0x3fa00000u, 0x7fc12345u};
  for (unsigned i = 0; i < sizeof(singles) / sizeof(singles[0]); ++i) {
    float value; __builtin_memcpy(&value, &singles[i], 4);
    if (single_bits(value) != original_single_bits(value)) return 3;
    float actual = bits_single(singles[i]);
    float expected = original_bits_single(singles[i]);
    if (__builtin_memcmp(&actual, &expected, 4)) return 4;
  }
  return 0;
}
)C",
                  true, {}, false);
}

TEST(LLVMCAlloca, StaticObjectsPreserveExplicitAlignment) {
  for (bool Preserve : {false, true})
    comparePrograms(R"(
target datalayout = "e-p:64:64-i64:64-i128:128-n8:16:32:64-S128"
%packed = type <{ i8, i64 }>
declare void @observe_static(ptr, i64, i64)
define i64 @static_objects() {
entry:
  %byte = alloca i8, align 64
  %word = alloca i16, align 64
  %wide = alloca i64, align 128
  %pointer = alloca ptr, align 64
  %real = alloca double, align 32
  %array = alloca [4 x i16], align 64
  %record = alloca %packed, align 64
  %natural = alloca i64, align 1
  call void @observe_static(ptr %byte, i64 1, i64 64)
  call void @observe_static(ptr %word, i64 2, i64 64)
  call void @observe_static(ptr %wide, i64 8, i64 128)
  call void @observe_static(ptr %pointer, i64 8, i64 64)
  call void @observe_static(ptr %real, i64 8, i64 32)
  call void @observe_static(ptr %array, i64 8, i64 64)
  call void @observe_static(ptr %record, i64 9, i64 64)
  call void @observe_static(ptr %natural, i64 8, i64 1)
  ret i64 17
}
)",
                    R"(
extern uint64_t original_static_objects(void);
static unsigned calls, bad;
static void *saved[8];
static uint64_t lengths[8];
void observe_static(void *address, uint64_t count, uint64_t alignment) {
  if (((uintptr_t)address & (alignment-1)) != 0) bad=1;
  for (unsigned j=0; j<calls; ++j) {
    if (saved[j] == address) bad=1;
    for (uint64_t i=0; i<lengths[j]; ++i)
      if (((uint8_t*)saved[j])[i] != (uint8_t)(i+j*19+7)) bad=1;
  }
  saved[calls]=address;
  lengths[calls]=count;
  for (uint64_t i=0; i<count; ++i) ((uint8_t*)address)[i]=(uint8_t)(i+calls*19+7);
  ++calls;
}
int main(void) {
  calls=bad=0;
  uint64_t expected=original_static_objects();
  if (bad || calls != 8) return 1;
  calls=bad=0;
  uint64_t actual=static_objects();
  return (bad || calls != 8 || actual != expected) ? 2 : 0;
}
)",
                    Preserve, {}, false);
}

TEST(LLVMCAlloca, GlobalObjectsPreserveExplicitAlignment) {
  for (bool Selected : {false, true})
    comparePrograms(R"(
target datalayout = "e-p:64:64-i64:64-i128:128-n8:16:32:64-S128"
@aligned_byte = global i8 0, align 64
@aligned_word = internal global i16 0, align 64
@aligned_wide = global i64 0, align 128
@aligned_array = global [4 x i16] zeroinitializer, align 64
@aligned_string = private constant [4 x i8] c"abc\00", align 64
declare void @observe_global(i64, i64)
define i64 @global_objects() {
entry:
  %byte = ptrtoint ptr @aligned_byte to i64
  %word = ptrtoint ptr @aligned_word to i64
  %wide = ptrtoint ptr @aligned_wide to i64
  %array = ptrtoint ptr @aligned_array to i64
  %string = ptrtoint ptr @aligned_string to i64
  call void @observe_global(i64 %byte, i64 64)
  call void @observe_global(i64 %word, i64 64)
  call void @observe_global(i64 %wide, i64 128)
  call void @observe_global(i64 %wide, i64 128)
  call void @observe_global(i64 ptrtoint (ptr @aligned_byte to i64), i64 64)
  call void @observe_global(i64 %array, i64 64)
  call void @observe_global(i64 %string, i64 64)
  ret i64 23
}
)",
                    R"(
extern uint64_t original_global_objects(void);
static unsigned calls, bad;
void observe_global(uint64_t address, uint64_t alignment) {
  if (!address || (address & (alignment-1)) != 0) bad=1;
  ++calls;
}
int main(void) {
  calls=bad=0;
  uint64_t expected=original_global_objects();
  if (bad || calls != 7) return 1;
  calls=bad=0;
  uint64_t actual=global_objects();
  return (bad || calls != 7 || actual != expected) ? 2 : 0;
}
)",
                    false, Selected ? "global_objects" : "", false);
}

TEST(LLVMCAlloca, StringStorageRetainsTrailingZeroExtent) {
  for (bool Selected : {false, true})
    comparePrograms(R"(
target datalayout = "e-p:64:64-i64:64-i128:128-n8:16:32:64-S128"
@padded_text = internal constant [8 x i8] c"ab\00\00\00\00\00\00", align 8
declare i64 @read_last(i64, i64)
define i64 @string_extent() {
entry:
  %address = ptrtoint ptr @padded_text to i64
  %result = call i64 @read_last(i64 %address, i64 7)
  ret i64 %result
}
)",
                    R"(
_Static_assert(sizeof(padded_text) == 8, "complete LLVM string storage");
extern uint64_t original_string_extent(void);
uint64_t read_last(uint64_t address, uint64_t offset) {
  return ((const uint8_t*)(uintptr_t)address)[offset];
}
int main(void) { return string_extent() != original_string_extent(); }
)",
                    false, Selected ? "string_extent" : "", false);
}

TEST(LLVMCAlloca, ScalarGlobalPointersRetainAddressIdentity) {
  for (bool Preserve : {false, true})
    comparePrograms(R"(
target datalayout = "e-p:64:64-i64:64-i128:128-n8:16:32:64-S128"
@global_value = global i64 5, align 8
@global_pointer = global ptr @global_value, align 8
declare i64 @identify_pointer(ptr)
define ptr @returned_pointer() {
  ret ptr @global_value
}
define i64 @pointer_values(i1 %which) {
  %selected = select i1 %which, ptr @global_value, ptr @global_pointer
  %first = call i64 @identify_pointer(ptr @global_value)
  %second = call i64 @identify_pointer(ptr %selected)
  %loaded = load ptr, ptr @global_pointer
  %third = call i64 @identify_pointer(ptr %loaded)
  %partial = add i64 %first, %second
  %result = add i64 %partial, %third
  ret i64 %result
}
)",
                    R"(
extern uint64_t original_global_value;
extern void *original_global_pointer;
extern void *original_returned_pointer(void);
extern uint64_t original_pointer_values(bool);
uint64_t identify_pointer(void *value) {
  if (value == &global_value || value == &original_global_value) return 11;
  if (value == &global_pointer || value == &original_global_pointer) return 19;
  return 0;
}
int main(void) {
  if (original_returned_pointer() != &original_global_value) return 1;
  if (returned_pointer() != &global_value) return 2;
  for (unsigned i=0; i!=2; ++i) {
    uint64_t expected=original_pointer_values(i != 0);
    if (expected != (i ? 33 : 41)) return 3;
    if (pointer_values(i != 0) != expected) return 4;
  }
  return 0;
}
)",
                    Preserve, {}, false);
}

TEST(LLVMCAlloca, DynamicBytesKeepExtentAlignmentAndIndependentInstances) {
  comparePrograms(R"(
target datalayout = "e-p:64:64-i64:64-i128:128-n8:16:32:64-S128"
declare void @observe_bytes(ptr, i64, i64)
define i64 @dynamic_bytes(i64 %count) {
entry:
  %first = alloca i8, i64 %count, align 64
  %second = alloca i8, i64 %count, align 16
  %last_offset = sub i64 %count, 1
  %last = getelementptr i8, ptr %first, i64 %last_offset
  call void @observe_bytes(ptr %first, i64 %count, i64 64)
  call void @observe_bytes(ptr %second, i64 %count, i64 16)
  %value = load i8, ptr %last, align 1
  %result = zext i8 %value to i64
  ret i64 %result
}
)",
                  R"(
extern uint64_t original_dynamic_bytes(uint64_t);
static unsigned calls, bad;
void observe_bytes(void *address, uint64_t count, uint64_t alignment) {
  uint8_t *bytes = address;
  if (((uintptr_t)address & (alignment-1)) != 0) bad = 1;
  ++calls;
  for (uint64_t i=0; i<count; ++i) bytes[i] = (uint8_t)(i + calls*19);
}
int main(void) {
  const uint64_t sizes[] = {1, 2, 3, 7, 15, 16, 17, 63, 64, 65, 511, 4097};
  for (unsigned i=0; i<sizeof(sizes)/sizeof(sizes[0]); ++i) {
    calls = bad = 0;
    uint64_t expected = original_dynamic_bytes(sizes[i]);
    if (bad || calls != 2) return 1;
    calls = bad = 0;
    uint64_t actual = dynamic_bytes(sizes[i]);
    if (bad || calls != 2 || actual != expected) return 2;
  }
  return 0;
}
)");
}

TEST(LLVMCAlloca, ConstantElementCountsUseAllocationStride) {
  comparePrograms(R"(
target datalayout = "e-p:64:64-i24:32-i64:64-i128:128-n8:16:32:64-S128"
declare void @observe_bytes(ptr, i64, i64)
define i64 @constant_array() {
entry:
  %storage = alloca [3 x i24], i64 7, align 32
  %last = getelementptr i8, ptr %storage, i64 83
  call void @observe_bytes(ptr %storage, i64 84, i64 32)
  %value = load i8, ptr %last, align 1
  %result = zext i8 %value to i64
  ret i64 %result
}
)",
                  R"(
extern uint64_t original_constant_array(void);
static unsigned bad;
void observe_bytes(void *address, uint64_t count, uint64_t alignment) {
  if (((uintptr_t)address & (alignment-1)) != 0) bad = 1;
  for (uint64_t i=0; i<count; ++i) ((uint8_t*)address)[i] = (uint8_t)(i*3+5);
}
int main(void) {
  uint64_t expected = original_constant_array();
  uint64_t actual = constant_array();
  return bad || expected != actual;
}
)");
}

TEST(LLVMCAlloca, LoopAllocationsRetainEarlierStorageThroughReturn) {
  comparePrograms(R"(
target datalayout = "e-p:64:64-i64:64-i128:128-n8:16:32:64-S128"
declare void @observe_loop(ptr, ptr, i64)
define i64 @loop_storage(i64 %count) {
entry:
  br label %loop
loop:
  %i = phi i64 [0, %entry], [%next, %loop]
  %previous = phi ptr [null, %entry], [%storage, %loop]
  %storage = alloca i8, i64 %count, align 64
  call void @observe_loop(ptr %previous, ptr %storage, i64 %i)
  %next = add i64 %i, 1
  %more = icmp ult i64 %next, 8
  br i1 %more, label %loop, label %exit
exit:
  %value = load i8, ptr %storage, align 1
  %result = zext i8 %value to i64
  ret i64 %result
}
)",
                  R"(
extern uint64_t original_loop_storage(uint64_t);
static void *saved[8];
static unsigned bad;
void observe_loop(void *previous, void *current, uint64_t iteration) {
  if (((uintptr_t)current & 63) != 0) bad = 1;
  for (uint64_t i=0; i<iteration; ++i)
    if (saved[i] == current || *(uint8_t*)saved[i] != (uint8_t)(i+23)) bad=1;
  if (iteration && previous != saved[iteration-1]) bad=1;
  saved[iteration] = current;
  *(uint8_t*)current = (uint8_t)(iteration+23);
}
int main(void) {
  for (uint64_t count=1; count<80; ++count) {
    bad=0;
    uint64_t expected=original_loop_storage(count);
    if (bad) return 1;
    uint64_t actual=loop_storage(count);
    if (bad || actual != expected) return 2;
  }
  return 0;
}
)");
}

TEST(LLVMCAlloca, CountProducerIsEvaluatedOnceWithNarrowUnsignedCount) {
  for (bool Preserve : {false, true})
    comparePrograms(R"(
target datalayout = "e-p:64:64-i64:64-i128:128-n8:16:32:64-S128"
declare i32 @read_count(i32)
declare void @observe_bytes(ptr, i64, i64)
define i64 @produced_count(i32 %requested) {
entry:
  %count = call i32 @read_count(i32 %requested)
  %storage = alloca i16, i32 %count, align 32
  %wide = zext i32 %count to i64
  %bytes = mul i64 %wide, 2
  call void @observe_bytes(ptr %storage, i64 %bytes, i64 32)
  %offset = sub i64 %bytes, 1
  %last = getelementptr i8, ptr %storage, i64 %offset
  %value = load i8, ptr %last, align 1
  %result = zext i8 %value to i64
  ret i64 %result
}
)",
                    R"(
extern uint64_t original_produced_count(uint32_t);
static unsigned calls, bad;
uint32_t read_count(uint32_t count) { ++calls; return count; }
void observe_bytes(void *address, uint64_t count, uint64_t alignment) {
  if (((uintptr_t)address & (alignment-1)) != 0) bad=1;
  for (uint64_t i=0; i<count; ++i) ((uint8_t*)address)[i]=(uint8_t)(i*7+3);
}
int main(void) {
  for (uint32_t i=1; i<=128; ++i) {
    calls=bad=0;
    uint64_t expected=original_produced_count(i);
    if (calls != 1 || bad) return 1;
    calls=bad=0;
    uint64_t actual=produced_count(i);
    if (calls != 1 || bad || actual != expected) return 2;
  }
  return 0;
}
)",
                    Preserve);
}

TEST(LLVMCAlloca, NonEntryUnitAllocationsKeepDistinctLoopInstances) {
  comparePrograms(R"(
target datalayout = "e-p:64:64-i64:64-i128:128-n8:16:32:64-S128"
declare void @observe_loop(ptr, ptr, i64)
define i64 @unit_storage() {
entry:
  br label %loop
loop:
  %i = phi i64 [0, %entry], [%next, %loop]
  %previous = phi ptr [null, %entry], [%storage, %loop]
  %storage = alloca i8, align 64
  call void @observe_loop(ptr %previous, ptr %storage, i64 %i)
  %next = add i64 %i, 1
  %more = icmp ult i64 %next, 8
  br i1 %more, label %loop, label %exit
exit:
  %value = load i8, ptr %storage, align 1
  %result = zext i8 %value to i64
  ret i64 %result
}
)",
                  R"(
extern uint64_t original_unit_storage(void);
static void *saved[8];
static unsigned bad;
void observe_loop(void *previous, void *current, uint64_t iteration) {
  if (((uintptr_t)current & 63) != 0) bad=1;
  for (uint64_t i=0; i<iteration; ++i)
    if (saved[i] == current || *(uint8_t*)saved[i] != (uint8_t)(i+23)) bad=1;
  if (iteration && previous != saved[iteration-1]) bad=1;
  saved[iteration]=current;
  *(uint8_t*)current=(uint8_t)(iteration+23);
}
int main(void) {
  uint64_t expected=original_unit_storage();
  uint64_t actual=unit_storage();
  return bad || actual != expected;
}
)");
}

TEST(LLVMCAlloca, NonEntryUnitAllocationOutsideACycleStaysOneObject) {
  // A block that runs at most once per call allocates one object for the
  // rest of the call, which is what a function-scope C object provides.
  constexpr llvm::StringLiteral IR = R"(
target datalayout = "e-p:64:64-i64:64-i128:128-n8:16:32:64-S128"
declare void @observe_once(ptr)
define i64 @branch_storage(i32 %take) {
entry:
  %slow.path = icmp ne i32 %take, 0
  br i1 %slow.path, label %slow, label %exit
slow:
  %storage = alloca i32, align 16
  store i32 23, ptr %storage, align 4
  call void @observe_once(ptr %storage)
  %value = load i32, ptr %storage, align 4
  %wide = zext i32 %value to i64
  br label %exit
exit:
  %result = phi i64 [ %wide, %slow ], [ 7, %entry ]
  ret i64 %result
}
)";
  llvm::LLVMContext Context;
  llvm::SMDiagnostic Diagnostic;
  auto Module = llvm::parseAssemblyString(IR, Diagnostic, Context);
  ASSERT_TRUE(Module) << Diagnostic.getMessage().str();
  CEmitterOptions Options;
  Options.TheArch = Arch::X64;
  Options.EmitIncludes = false;
  std::string Text;
  llvm::raw_string_ostream Stream(Text);
  ASSERT_TRUE(LLVMCEmitter().emit(*Module, Stream, Options));
  EXPECT_EQ(Text.find("__builtin_alloca("), std::string::npos) << Text;
  comparePrograms(IR, R"(
extern uint64_t original_branch_storage(uint32_t);
static unsigned bad, seen;
void observe_once(void *storage) {
  if (((uintptr_t)storage & 15) != 0 || *(uint32_t*)storage != 23) bad=1;
  ++seen;
  *(uint32_t*)storage=41;
}
int main(void) {
  uint64_t expected=original_branch_storage(1)+original_branch_storage(0);
  uint64_t actual=branch_storage(1)+branch_storage(0);
  return bad || seen != 2 || actual != expected;
}
)",
                  /*PreserveTypes=*/false, /*SelectedFunction=*/{},
                  /*RequireRuntimeAllocation=*/false);
}

TEST(LLVMCAlloca, UnsupportedStackContractsRefuseBeforeDeadCodeRemoval) {
  const char *Inputs[] = {
      R"(target datalayout = "e-p:64:64"
define void @bad(i128 %count) {
  %storage = alloca i8, i128 %count
  ret void
})",
      R"(target datalayout = "e-p:64:64"
define void @bad() {
  %storage = alloca i64, i64 18446744073709551615
  ret void
})",
      R"(target datalayout = "e-p:64:64-A5"
define void @bad(i64 %count) {
  %storage = alloca i8, i64 %count, addrspace(5)
  ret void
})",
      R"(target datalayout = "e-p:64:64"
define void @bad(i64 %count) {
  %storage = alloca inalloca i8, i64 %count
  ret void
})",
      R"(target datalayout = "e-p:64:64"
define void @bad() {
  %storage = alloca swifterror ptr
  ret void
})",
      R"(target datalayout = "e-p:64:64"
define void @bad(i64 %count) {
  %storage = alloca <vscale x 4 x i32>, i64 %count
  ret void
})",
      R"(target datalayout = "e-p:64:64"
declare ptr @llvm.stacksave.p0()
define void @bad() {
  %saved = call ptr @llvm.stacksave.p0()
  ret void
})",
      R"(target datalayout = "e-p:64:64"
declare void @llvm.stackrestore.p0(ptr)
define void @bad(ptr %saved) {
  call void @llvm.stackrestore.p0(ptr %saved)
  ret void
})",
      R"(target datalayout = "e-p:64:64"
declare ptr @forged(i64, i64, i64) #0
define void @bad() { ret void }
attributes #0 = { "neverd.c.stack-allocation" })"};
  for (const char *Input : Inputs) {
    SCOPED_TRACE(Input);
    llvm::LLVMContext Context;
    llvm::SMDiagnostic Diagnostic;
    auto Module = llvm::parseAssemblyString(Input, Diagnostic, Context);
    ASSERT_TRUE(Module) << Diagnostic.getMessage().str();
    ASSERT_FALSE(llvm::verifyModule(*Module, &llvm::errs()));
    const auto Before = moduleText(*Module);
    std::string Text;
    llvm::raw_string_ostream Stream(Text);
    CEmitterOptions Options;
    Options.TheArch = Arch::X64;
    EXPECT_THROW(LLVMCEmitter().emit(*Module, Stream, Options),
                 std::runtime_error);
    EXPECT_EQ(moduleText(*Module), Before);
    EXPECT_TRUE(Text.empty());
  }
}

TEST(LLVMCAlloca, ZeroExtentAndProviderNameCollisionKeepOrdinaryCalls) {
  comparePrograms(R"(
target datalayout = "e-p:64:64-i64:64-i128:128-n8:16:32:64-S128"
declare i64 @nd_c_stack_allocation(i64)
declare void @observe_zero(ptr, i64)
define i64 @zero_storage(i64 %count) {
entry:
  %storage = alloca i8, i64 %count, align 64
  call void @observe_zero(ptr %storage, i64 %count)
  %result = call i64 @nd_c_stack_allocation(i64 13)
  ret i64 %result
}
)",
                  R"(
extern uint64_t original_zero_storage(uint64_t);
static unsigned calls, bad;
uint64_t nd_c_stack_allocation(uint64_t value) { ++calls; return value+71; }
void observe_zero(void *address, uint64_t count) {
  if (count != 0 || ((uintptr_t)address & 63) != 0) bad=1;
  ++calls;
}
int main(void) {
  calls=bad=0;
  uint64_t expected=original_zero_storage(0);
  if (bad || calls != 2) return 1;
  calls=bad=0;
  uint64_t actual=zero_storage(0);
  return bad || calls != 2 || actual != expected;
}
)",
                  false, "zero_storage");
}
