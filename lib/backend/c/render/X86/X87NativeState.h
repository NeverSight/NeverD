//===- X87NativeState.h - Native x87 state C calls ------------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Render native x87 state operations and their exact instruction fallbacks.
//===----------------------------------------------------------------------===//

#ifndef NEVERD_BACKEND_C_RENDER_X86_X87NATIVESTATE_H
#define NEVERD_BACKEND_C_RENDER_X86_X87NATIVESTATE_H

#include "neverd/backend/llvm/X86/LLVMX86X87StateAsm.h"
#include "neverd/backend/llvm/X86/LLVMX87NativeState.h"

#include "llvm/ADT/StringRef.h"

#include <string>

namespace neverd {

inline std::string renderX87NativeState(const X87NativeStateSpec &Spec,
                                        llvm::StringRef Address = {},
                                        llvm::StringRef Result = {},
                                        llvm::StringRef Segment = {}) {
  std::string Text = "\n#if __has_builtin(" + std::string(Spec.Builtin) + ")\n";
  if (Spec.ReturnsStatus && !Result.empty())
    Text += Result.str() + " = ";
  Text += std::string(Spec.Builtin) + "(";
  if (Spec.HasPointer) {
    const bool Read =
        Spec.ReadsImage || Spec.Kind == X87NativeStateKind::WriteControl;
    Text += std::string(Read ? "(const " : "(") +
            (Spec.ImageBytes ? "void *)(" : "unsigned short *)(") +
            Address.str() + ")";
  }
  Text += ");\n#else\n__asm__ volatile(\"" + std::string(Spec.Mnemonic);
  // The pointer interfaces use the flat process address space. Preserve an
  // explicit FS/GS override in the instruction fallback.
  if (!Segment.empty())
    Text = "\n__asm__ volatile(\"" + std::string(Spec.Mnemonic);
  if (Spec.HasPointer)
    Text += " " + (Segment.empty() ? "" : "%%" + Segment.str() + ":") +
            "(%0)\" :: \"r\"(" + Address.str() +
            ") : " + x87StateCClobbers(x87NativeStateEffect(Spec)).str() + ");";
  else if (Spec.ReturnsStatus)
    Text += Result.empty()
                ? " %%ax\" ::: \"ax\", \"memory\");"
                : " %%ax\" : \"=a\"(" + Result.str() + ") :: \"memory\");";
  else
    Text +=
        "\" ::: " + x87StateCClobbers(x87NativeStateEffect(Spec)).str() + ");";
  return Text + (Segment.empty() ? "\n#endif\n" : "\n");
}

inline std::string renderX87ControlAccess(bool Write, llvm::StringRef Address) {
  return renderX87NativeState(
      x87NativeStateSpec(Write ? X87NativeStateKind::WriteControl
                               : X87NativeStateKind::ReadControl),
      Address);
}

} // namespace neverd

#endif // NEVERD_BACKEND_C_RENDER_X86_X87NATIVESTATE_H
