//===- section_attribs.h - Section annotations for type extraction --------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
// Placement in a linker section does not change a function's declaration.
// This generation-only shim avoids pulling ReactOS NDK implementation
// helpers into the public DDK type input.

#ifndef NEVERD_SCRIPTS_WINDOWS_TYPES_SECTION_ATTRIBS_H
#define NEVERD_SCRIPTS_WINDOWS_TYPES_SECTION_ATTRIBS_H

#define CODE_SEG(Name)
#define DATA_SEG(Name)

#endif // NEVERD_SCRIPTS_WINDOWS_TYPES_SECTION_ATTRIBS_H
