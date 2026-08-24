//===- SBFCEmitterDetail.h - Private SBF C backend helpers ------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Rendering helpers shared by the SBF C backend's translation units: the
/// module driver (SBFCEmitter.cpp), per-instruction rendering
/// (SBFCEmitterInstruction.cpp), and structured control flow
/// (SBFCEmitterStructured.cpp).
///
/// This header is an implementation detail of the sbf/emit library and should
/// NOT be included by code outside lib/sbf/emit/.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_SBF_EMIT_SBFCEMITTERDETAIL_H
#define NEVERD_SBF_EMIT_SBFCEMITTERDETAIL_H

#include "neverd/sbf/analysis/SBFStructuredCFG.h"
#include "neverd/sbf/emit/SBFCEmitter.h"

#include "llvm/ADT/StringRef.h"
#include "llvm/Support/Compiler.h"
#include "llvm/Support/raw_ostream.h"

#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace neverd::sbf {
namespace LLVM_LIBRARY_VISIBILITY_NAMESPACE c_emitter_detail {

/// A 64-bit literal in the generated C dialect.
std::string word(uint64_t Value);

/// The C expression for the comparison \p Instruction performs.
std::string comparison(const MedInstruction &Instruction);

/// Render an instruction that falls through to the next slot. Returns false
/// for the control-flow forms, which the caller renders itself.
bool emitLinearInstruction(llvm::raw_ostream &OS,
                           const MedInstruction &Instruction,
                           llvm::StringRef Indent);

/// Render one arm of the dispatch switch, control flow included.
void emitInstruction(llvm::raw_ostream &OS, const MedInstruction &Instruction,
                     const SBFProgram &Program);

/// Render a validated structured control-flow plan. Returns false when the
/// plan contains a node this backend cannot render.
bool emitStructuredNodes(llvm::raw_ostream &OS, const SBFProgram &Program,
                         const std::map<size_t, const MedInstruction *> &BySlot,
                         const StructuredControlFlow &Plan,
                         llvm::StringRef Indent);

} // namespace LLVM_LIBRARY_VISIBILITY_NAMESPACE c_emitter_detail
} // namespace neverd::sbf

#endif // NEVERD_SBF_EMIT_SBFCEMITTERDETAIL_H
