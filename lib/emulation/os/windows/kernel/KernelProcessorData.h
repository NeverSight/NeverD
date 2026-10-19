//===- KernelProcessorData.h - Typed processor data export identity
//--------===//
// NeverD Decompiler
///
/// \file
/// Typed processor data export identity.
///

#ifndef NEVERD_KERNEL_PROCESSOR_DATA_H
#define NEVERD_KERNEL_PROCESSOR_DATA_H
#include "KernelProcessData.h"
#include "KernelProcessorTopology.h"
namespace neverd::emulation::processor {
inline constexpr llvm::StringLiteral CountExport = "KeNumberProcessors";
// Public PE export KeNumberProcessors is a legacy CCHAR, distinct from the
// private ULONG with the same PDB spelling used by the count-query body.
inline constexpr uint32_t CountBytes = 1;
inline constexpr uint64_t CountSlot = profile::ObjectTypeDataBase + 0x40;
static_assert(CountSlot + CountBytes <=
              object_type::identity(object_type::Kind::Thread));
inline bool isData(llvm::StringRef Name) { return Name == CountExport; }
} // namespace neverd::emulation::processor
#endif
