//===- KernelProcessData.h - Process-owned kernel data exports ------------===//
// NeverD Decompiler
///
/// \file
/// Process-owned kernel data exports.
///

#ifndef NEVERD_EMULATION_KERNEL_PROCESS_DATA_H
#define NEVERD_EMULATION_KERNEL_PROCESS_DATA_H
#include "KernelObjectTypes.h"
namespace neverd::emulation::process_data {
inline constexpr llvm::StringLiteral InitialSystemProcess =
    "PsInitialSystemProcess";
// Share the read-only export page while preserving the existing object-type
// slots and opaque identity tokens. This is a pointer slot, not an object type.
inline constexpr uint64_t InitialSystemProcessSlot =
    profile::ObjectTypeDataBase +
    object_type::Entries.size() * profile::PointerSize;
static_assert(InitialSystemProcessSlot + profile::PointerSize <=
              object_type::identity(object_type::Kind::Thread));
inline bool find(llvm::StringRef Name) { return Name == InitialSystemProcess; }
} // namespace neverd::emulation::process_data
#endif
