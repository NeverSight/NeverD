//===- GoCurrentGoroutine.h - The goroutine Go code runs on ----*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Read the goroutine register of Go's internal ABI as the goroutine it holds.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_LIB_IR_MED_LOWER_GOCURRENTGOROUTINE_H
#define NEVERD_LIB_IR_MED_LOWER_GOCURRENTGOROUTINE_H

#include <cstdint>

namespace neverd {
struct MedFunc;

/// Give every read of the goroutine register \p Register in \p Func, a
/// function in SSA form that Go's compiler compiled for its internal ABI, the
/// goroutine the register holds (Intrinsic::GoCurrentGoroutine).  The
/// compiler keeps the current goroutine there ("R14 | Current goroutine |
/// Same | Same" in abi-internal.md, R28 on arm64) and never writes it, so a
/// read at the entry reads the current goroutine, and so does a read after a
/// call, which may have changed the goroutine (`needm` installs another).
/// The incoming value and each call's clobber of the register that something
/// reads become such a read: one after the entry's copies, and one just after
/// the call.  Dead code elimination drops one nothing comes to use.
void defineGoCurrentGoroutine(MedFunc &Func, uint64_t Register);
} // namespace neverd

#endif // NEVERD_LIB_IR_MED_LOWER_GOCURRENTGOROUTINE_H
