#ifndef NEVERD_LOADER_OBJC_OBJCCLASSGETTERCALLS_H
#define NEVERD_LOADER_OBJC_OBJCCLASSGETTERCALLS_H

#include "neverd/ir/low/SourceClassGetterCall.h"

namespace neverd {
struct BinaryImage;
struct LowFunc;
/// Fresh exact caller/leaf/import facts, not source declarations or permission
/// to remove calls. Only local ADRP x8; LDR x0,[x8,#imm]; RET x30 is admitted.
SourceClassGetterCalls sourceClassGetterCalls(const BinaryImage &Image,
                                              const LowFunc &Caller);
bool validateSourceClassGetterCalls(const BinaryImage &Image,
                                    const LowFunc &Caller,
                                    const SourceClassGetterCalls &Receipts);
} // namespace neverd
#endif
