//===- HighCWin64OpenCall.h - Untyped Win64 external calls -----*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Spell an open integer call boundary in a Win64 original-state C export.
//===----------------------------------------------------------------------===//

#ifndef NEVERD_LIB_BACKEND_C_HIGHC_COFF_HIGHCWIN64OPENCALL_H
#define NEVERD_LIB_BACKEND_C_HIGHC_COFF_HIGHCWIN64OPENCALL_H

#include "neverd/debug/DebugContext.h"
#include "neverd/ir/high/HighIR.h"

#include "llvm/ADT/STLExtras.h"
#include "llvm/Support/MathExtras.h"
#include "llvm/Support/raw_ostream.h"

namespace neverd::c_detail {

inline bool hasOnlyWin64SymbolName(const FunctionSym &Sym) {
  return !Sym.ReturnType && Sym.Params.empty() && !Sym.IsVariadic &&
         Sym.PlatformConvention && Sym.ParamEntryLocations.empty() &&
         Sym.CallConv == DebugCallConv::Unknown &&
         !Sym.ExplicitEntryDeclaration && !Sym.PrototypeFromHeader &&
         !Sym.PrototypeFromCLibrary && Sym.PrototypeLibrary.empty() &&
         Sym.PrototypeDeclaration.empty();
}

inline bool isWin64IntegerCall(const HighExpr &Call) {
  // Unprototyped and variadic Win64 calls agree on integer and pointer slots.
  // FP, vector and aggregate placement needs its own calling-convention proof.
  const auto IntegerSlot = [](const TypeRef &Type) {
    if (!Type)
      return false;
    if (Type->Kind == NdTypeKind::Ptr)
      return Type->Size == sizeof(uint64_t);
    return Type->Kind == NdTypeKind::Int && Type->Size <= sizeof(uint64_t) &&
           llvm::isPowerOf2_32(Type->Size);
  };
  return !Call.IsIndirectCall && !Call.FloatingCallSignature &&
         !Call.SourceCallHint && !Call.SourceCalleeType &&
         (!Call.Type || Call.Type->Kind == NdTypeKind::Void ||
          IntegerSlot(Call.Type)) &&
         llvm::all_of(Call.Operands, [&](const ExprPtr &Arg) {
           return Arg && IntegerSlot(Arg->Type);
         });
}

class Win64OpenCallDeclarations {
public:
  void write(llvm::raw_ostream &OS, llvm::StringRef Result,
             llvm::StringRef Name, llvm::StringRef LinkLabel, bool NoReturn,
             llvm::StringRef LinkComment) {
    if (!Emitted) {
      // C23 changed f() to f(void). Pre-C23 Clang permits the open list on
      // an overloadable declaration; the explicit assembler label keeps the
      // binary symbol, including where a header already holds its C name.
      // GCC 13+ can form the C23 function type in an unevaluated operand
      // even in C11 mode, with strict prototype checking enabled.
      OS << R"(/* External Win64 integer calls with no recorded parameter types. */
#if defined(__cplusplus) || (defined(__STDC_VERSION__) && __STDC_VERSION__ >= 202311L)
#define __NEVERD_WIN64_OPEN(result, name) extern result name(...)
#elif defined(__clang__)
#define __NEVERD_WIN64_OPEN(result, name) extern result name(...) __attribute__((overloadable))
#elif defined(__GNUC__) && __GNUC__ >= 13
#define __NEVERD_WIN64_OPEN(result, name) __extension__ extern __typeof__(*(result (*)(...))0) name
#else
#define __NEVERD_WIN64_OPEN(result, name) extern result name()
#endif
)";
      Emitted = true;
    }
    OS << "__NEVERD_WIN64_OPEN(" << Result << ", " << Name << ")";
    if (LinkLabel.empty())
      OS << " __asm__(\"" << Name << "\")";
    else
      OS << LinkLabel;
    if (NoReturn)
      OS << " __attribute__((noreturn))";
    OS << ";" << LinkComment << "\n";
  }

  void finish(llvm::raw_ostream &OS) const {
    if (Emitted)
      OS << "#undef __NEVERD_WIN64_OPEN\n";
  }

private:
  bool Emitted = false;
};

} // namespace neverd::c_detail

#endif // NEVERD_LIB_BACKEND_C_HIGHC_COFF_HIGHCWIN64OPENCALL_H
