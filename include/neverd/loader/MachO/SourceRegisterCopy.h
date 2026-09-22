#ifndef NEVERD_LOADER_MACHO_SOURCEREGISTERCOPY_H
#define NEVERD_LOADER_MACHO_SOURCEREGISTERCOPY_H

#include "neverd/ir/low/SourceRegisterCopy.h"

namespace neverd {
struct BinaryImage;
struct LowFunc;

/// Complete machine effects, including any caller-SP store. This authenticates
/// the leaf only; callers must separately prove their current private frame.
std::optional<SourceRegisterCopy>
sourceRegisterCopyLeafEffects(const BinaryImage &Image, va_t Entry);

/// Exact final-register effects of the same bounded local machine leaf.
/// This does not qualify a caller occurrence or declare a C calling convention.
std::optional<SourceRegisterValues>
sourceRegisterCopyLeafRegisters(const BinaryImage &Image, va_t Entry);

/// Authenticate bounded, direct AArch64 BL occurrences and their complete
/// local MOV64/ADRP/ADD/RET bodies in a linked immutable Mach-O image. Complete
/// address outputs are restricted to authenticated constant-string objects.
/// Original LowIR
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
