#ifndef NEVERD_LOADER_MACHO_SOURCEREGISTERCOPY_H
#define NEVERD_LOADER_MACHO_SOURCEREGISTERCOPY_H

#include "neverd/ir/low/SourceRegisterCopy.h"

namespace neverd {
struct BinaryImage;
struct LowFunc;

/// Exact final-register effects of the same bounded local machine leaf.
/// This does not qualify a caller occurrence or declare a C calling convention.
std::optional<std::map<uint64_t, uint64_t>>
sourceRegisterCopyLeafRegisters(const BinaryImage &Image, va_t Entry);

/// Authenticate bounded, direct AArch64 BL occurrences and their complete
/// local MOV64/RET bodies in a linked immutable Mach-O image. Original LowIR
/// stays unchanged, including BL's real LR write and instruction boundaries.
/// Missing, conflicting or incomplete evidence supplies no projection.
SourceRegisterCopies sourceRegisterCopies(const BinaryImage &Image,
                                          const LowFunc &Caller);

/// Rebuild every stored receipt from current bytes and caller boundaries.
/// An unprojected candidate is allowed; a stale, extra or moved receipt is not.
bool validateSourceRegisterCopies(const BinaryImage &Image,
                                  const LowFunc &Caller,
                                  const SourceRegisterCopies &Receipts);
} // namespace neverd
#endif
