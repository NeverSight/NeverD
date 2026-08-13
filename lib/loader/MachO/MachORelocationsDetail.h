//===- MachORelocationsDetail.h - Private Mach-O reloc helpers --*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Implementation detail shared between the Mach-O relocation translation
/// units in `lib/loader/MachO`.  Nothing outside that directory may include
/// this header; the unit-testable i386 arithmetic lives in the public
/// `neverd/loader/MachO/MachORelocations.h` instead.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_LIB_LOADER_MACHO_MACHORELOCATIONSDETAIL_H
#define NEVERD_LIB_LOADER_MACHO_MACHORELOCATIONSDETAIL_H

#include "neverd/loader/BinaryImage.h"

#include "llvm/Object/MachO.h"

namespace neverd::macho_loader {

/// Apply the i386 `GENERIC_RELOC_*` relocations of every section of \p Obj.
/// Defined in MachOI386Relocations.cpp.
void applyI386ObjectRelocations(const llvm::object::MachOObjectFile &Obj,
                                BinaryImage &Img);

} // namespace neverd::macho_loader

#endif // NEVERD_LIB_LOADER_MACHO_MACHORELOCATIONSDETAIL_H
