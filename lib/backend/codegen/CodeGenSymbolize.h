//===- CodeGenSymbolize.h - Original image storage in patch IR -*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#ifndef NEVERD_LIB_BACKEND_CODEGEN_CODEGENSYMBOLIZE_H
#define NEVERD_LIB_BACKEND_CODEGEN_CODEGENSYMBOLIZE_H

namespace llvm {
class GlobalVariable;
}

namespace neverd {
/// Bind a lifted pointer-table mirror to its preserved original storage.
void declareOriginalPointerMirror(llvm::GlobalVariable &Mirror);
} // namespace neverd

#endif // NEVERD_LIB_BACKEND_CODEGEN_CODEGENSYMBOLIZE_H
