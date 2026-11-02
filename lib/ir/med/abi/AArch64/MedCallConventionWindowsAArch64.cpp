//===- MedCallConventionWindowsAArch64.cpp - Windows ARM64 call arguments -===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
//
// Windows on ARM64 passes a routine's fixed arguments as AAPCS64 does (X0-X7,
// V0-V7, then the stack).  The setup each caller writes gives a call its
// arguments, as for AAPCS64, but an import's prototype, as a header library
// declares it, counts the integer arguments its calls pass: no register a
// call happens to hold past them is one, and a register the caller passes on
// untouched is one of them.  A call through a pointer to a fixed C prototype
// of scalars passes and returns them as it declares.
//
//===----------------------------------------------------------------------===//

#include "neverd/ir/TargetRegInfo.h"
#include "neverd/ir/med/MedCallConvention.h"

#include "llvm/ADT/STLExtras.h"

namespace neverd {
namespace {
/// A declaration of no convention or `__cdecl` returning nothing or one
/// scalar, and taking scalars only: an integer of up to eight bytes (X0-X7),
/// a float or double (V0-V7), or a pointer.  Every other declared convention
/// keeps its unprototyped call.
bool windowsAArch64ScalarPrototypeCall(const NdType &Prototype) {
  const auto Scalar = [](const TypeRef &Type) {
    if (!Type)
      return false;
    if (Type->Kind == NdTypeKind::Ptr)
      return Type->Size == getTargetRegInfo(Arch::AArch64).PointerSize;
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

extern const CallArgumentConvention WindowsAArch64CallArguments;
const CallArgumentConvention WindowsAArch64CallArguments = {
    .TheArch = Arch::AArch64,
    .Format = BinaryFormat::COFF,
    .ImportArgumentsFromPrototype = true,
    .ArgumentsFromCallSetup = true,
    .ScalarPrototypeCall = windowsAArch64ScalarPrototypeCall,
};

} // namespace neverd
