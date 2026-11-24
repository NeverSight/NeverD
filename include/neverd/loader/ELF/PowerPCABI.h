//===- PowerPCABI.h - PowerPC ELF ABI layout ----------------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Expose the ELF constants shared by the loader and the PowerPC ABI.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_LOADER_ELF_POWERPCABI_H
#define NEVERD_LOADER_ELF_POWERPCABI_H

namespace neverd::ppc_elf {
#include "neverd/loader/ELF/PowerPCABI.inc"
} // namespace neverd::ppc_elf

#endif // NEVERD_LOADER_ELF_POWERPCABI_H
