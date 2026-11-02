//===- MedCallConventionWin64.cpp - Microsoft x64 call arguments ---------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
//
// Microsoft x64: RCX, RDX, R8 and R9, or XMM0-XMM3 in the same positions.
// The caller reserves a 32-byte home area for them below the stack
// arguments.  A variadic callee spills its variadic registers to their home
// slots, and a Control Flow Guard dispatcher (`_guard_dispatch_icall`)
// jumps to RAX with the caller's argument registers.  A C runtime import
// with a fixed prototype reads exactly its parameters, whether the code
// calls it through its import address slot (`call [__imp_free]`) or a thunk
// that jumps there (`jmp [__imp_free]`); a WDK routine's prototype fixes each
// of its operands.  A call returns one register value at most: a larger
// record travels through caller-owned memory.  A call through a pointer to a
// fixed C prototype of scalars passes and returns them as it declares.
//
//===----------------------------------------------------------------------===//

#include "neverd/ir/med/MedCallConvention.h"
#include "neverd/libc/LibCNames.h"
#include "neverd/lift/X86/X86Regs.h"

#include "llvm/ADT/STLExtras.h"

#include <vector>

namespace neverd {

namespace {
/// A WDK prototype fixes a kernel routine's argument count.
std::optional<size_t> wdkPrototypeArgCount(llvm::StringRef Name) {
  if (const libc::WindowsKernelPrototype *Proto =
          libc::windowsKernelPrototype(Name))
    return Proto->ArgCount;
  return std::nullopt;
}

/// It also authenticates each operand's width, the stack operands' too:
/// counts alone lose those when recovery moves RSP between a private spill
/// and the call's outgoing area.  It publishes arguments only, never a result
/// type.
std::optional<FixedIntegerCallArguments>
wdkPrototypeIntegerArguments(llvm::StringRef Name) {
  const libc::WindowsKernelPrototype *Proto =
      libc::windowsKernelPrototype(Name);
  if (!Proto)
    return std::nullopt;
  std::vector<TypeRef> Parameters;
  for (unsigned I = 0; I < Proto->ArgCount; ++I)
    Parameters.push_back(NdType::makeInt(Proto->ArgWidths[I], false));
  return fixedIntegerCallArguments(Arch::X64, BinaryFormat::COFF,
                                   NdType::makeInt(8, false), Parameters);
}

/// A declaration of no convention or `__cdecl` returning nothing or one
/// scalar, and taking scalars only: an integer of up to eight bytes, a float
/// or double, or a pointer.  Every other declared convention keeps its
/// unprototyped call.
bool win64ScalarPrototypeCall(const NdType &Prototype) {
  const auto Scalar = [](const TypeRef &Type) {
    if (!Type)
      return false;
    if (Type->Kind == NdTypeKind::Ptr)
      return Type->Size == getTargetRegInfo(Arch::X64).PointerSize;
    if (Type->Kind == NdTypeKind::Float)
      return Type->Size == 4 || Type->Size == 8;
    return Type->Kind == NdTypeKind::Int &&
           (Type->Size == 1 || Type->Size == 2 || Type->Size == 4 ||
            Type->Size == 8);
  };
  return Prototype.Kind == NdTypeKind::Func && Prototype.RetType &&
         (Prototype.FunctionConvention ==
              types::CallingConvention::Unspecified ||
          Prototype.FunctionConvention == types::CallingConvention::Cdecl) &&
         (Prototype.RetType->Kind == NdTypeKind::Void ||
          Scalar(Prototype.RetType)) &&
         llvm::all_of(Prototype.ParamTypes, Scalar);
}
} // namespace

extern const CallArgumentConvention Win64CallArguments;
const CallArgumentConvention Win64CallArguments = {
    .TheArch = Arch::X64,
    .Format = BinaryFormat::COFF,
    .RegisterArgumentsFromCalleeSummary = true,
    .VectorArgumentsFromCalleeSummary = true,
    .ImportArgumentsFromPrototype = true,
    .DispatcherTargetRegister = x86reg::RAX,
    .VariadicFromSummary = true,
    .StackArgumentSummary = true,
    .PositionalArgumentSlots = true,
    .ReservedOutgoingArea = true,
    .ParametersFromIncomingReads = true,
    .PrototypeArgCount = wdkPrototypeArgCount,
    .PrototypeIntegerArguments = wdkPrototypeIntegerArguments,
    .SingleRegisterReturns = true,
    .ScalarPrototypeCall = win64ScalarPrototypeCall,
};

} // namespace neverd
