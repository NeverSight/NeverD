//===- MedCallConventionI386.cpp - i386 call arguments -------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
//
// i386 cdecl and stdcall pass every argument on the stack.  Compilers give a
// directly called static function a regparm convention instead, whose stack
// arguments follow the registers it uses: Clang's fastcall order (ECX, EDX),
// or GCC's regparm order (EAX, EDX, ECX).  A function-pointer call or an
// imported function always uses the stack, as many arguments as its
// prototype has.
//
//===----------------------------------------------------------------------===//

#include "neverd/ir/med/MedCallConvention.h"
#include "neverd/lift/X86/X86Regs.h"
#include "neverd/loader/BinaryImage.h"

namespace neverd {

namespace {
/// GCC's regparm(3) order, which it gives a local function.
const uint64_t I386RegparmOrder[] = {x86reg::RAX, x86reg::RDX, x86reg::RCX};

/// A Windows import may be __fastcall (ECX, EDX); an ELF or Mach-O import
/// takes every argument on the stack.
bool importsMayTakeRegisterArguments(const BinaryImage &Img) {
  return Img.abiFormat() == BinaryFormat::COFF;
}
} // namespace

extern const CallArgumentConvention I386CallArguments;
const CallArgumentConvention I386CallArguments = {
    .TheArch = Arch::X86,
    .ImportArgumentsFromPrototype = true,
    .StackArgumentSummary = true,
    .RegparmOnlyForInternalCalls = true,
    .ImportsMayTakeRegisterArguments = importsMayTakeRegisterArguments,
    .StackArgumentsFollowUsedRegisters = true,
    .StackOnlyVariadicCallees = true,
    .RegisterArgumentsFillInOrder = true,
    .AlternateRegisterOrder = I386RegparmOrder,
    .ArgumentsFromCallSetup = true,
};

} // namespace neverd
