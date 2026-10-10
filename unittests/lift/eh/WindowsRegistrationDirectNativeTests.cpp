//===- WindowsRegistrationDirectNativeTests.cpp - Direct PE32 catches -----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "neverd/backend/codegen/CodeGen.h"

#include "llvm/IR/Module.h"
#include "llvm/Support/FormatVariadic.h"
#include "llvm/Support/raw_ostream.h"

#include <cstdlib>

using namespace neverd;

namespace {
// An independent canonical MSVC frame exercises the direct EBP coordinate.
// The runtime dispatches through the real CRT. No catch object is allocated
// or read, and the continuation observes an actual write to parent storage.
void emitDirectCallback(bool Reference, bool CatchAll) {
  llvm::LLVMContext Context;
  llvm::Module Module("direct-unbound-parent", Context);
  Module.setTargetTriple(llvm::Triple("i686-pc-windows-msvc"));
  Module.setDataLayout("e-m:x-p:32:32-i64:64-n8:16:32-S32");
  Module.appendModuleInlineAsm(llvm::formatv(R"(
.text
.def _callback_parent; .scl 2; .type 32; .endef
.globl _callback_parent
.p2align 4
_callback_parent:
  pushl %ebp
  movl %esp, %ebp
  pushl $-1
  pushl $direct_handler
  movl %fs:0, %eax
  pushl %eax
  movl %esp, %fs:0
  subl $64, %esp
  pushl %ebx
  pushl %esi
  pushl %edi
  movl %esp, -16(%ebp)
  movl $0, -4(%ebp)
  calll _callback_throw
  ud2
direct_resume:
  movl -24(%ebp), %eax
  movl -12(%ebp), %ecx
  movl %ecx, %fs:0
  popl %edi
  popl %esi
  popl %ebx
  movl %ebp, %esp
  popl %ebp
  retl
direct_catch:
  movl $7, _callback_caught
  movl $7, -24(%ebp)
  movl $direct_resume, %eax
  retl
.def direct_handler; .scl 3; .type 32; .endef
direct_handler:
  movl $direct_info, %eax
  jmp ___CxxFrameHandler3
.safeseh direct_handler
.section .xdata,"dr"
.p2align 2
direct_info:
  .long 0x19930522, 2, direct_unwind, 1, direct_try, 0, 0, 0, 1
direct_unwind:
  .long -1, 0, -1, 0
direct_try:
  .long 0, 0, 1, 1, direct_catches
direct_catches:
  .long {0}, {1}, 0, direct_catch
.section .drectve,"yn"
  .ascii " /EXPORT:_callback_parent"
)",
                                             CatchAll    ? 64
                                             : Reference ? 8
                                                         : 0,
                                             CatchAll ? "0" : "\"??_R0H@8\"")
                                   .str());
  auto Object = Codegen().compile(Module, Arch::X86, BinaryFormat::COFF);
  ASSERT_TRUE(Object.Success);
  if (const auto *Path = std::getenv("NEVERD_REGISTRATION_REALIGNED_OBJECT")) {
    std::error_code Error;
    llvm::raw_fd_ostream Out(Path, Error);
    ASSERT_FALSE(Error) << Error.message();
    Out.write(reinterpret_cast<const char *>(Object.ObjectData.data()),
              Object.ObjectData.size());
    Out.close();
    ASSERT_FALSE(Out.has_error());
  }
}

TEST(WindowsRegistrationRealignedNative, EmitsUnnamedValueFixedCallback) {
  emitDirectCallback(false, false);
}

TEST(WindowsRegistrationRealignedNative, EmitsUnnamedReferenceFixedCallback) {
  emitDirectCallback(true, false);
}

TEST(WindowsRegistrationRealignedNative, EmitsCatchAllFixedCallback) {
  emitDirectCallback(false, true);
}
} // namespace
