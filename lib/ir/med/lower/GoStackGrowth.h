//===- GoStackGrowth.h - A Go function's stack-growth path -----*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Leave out the stack-growth path that starts every Go function that has a
/// frame: it grows the stack and starts the function again, so it changes
/// nothing the function computes.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_LIB_IR_MED_LOWER_GOSTACKGROWTH_H
#define NEVERD_LIB_IR_MED_LOWER_GOSTACKGROWTH_H

#include "neverd/Common.h"

namespace neverd {
struct MedFunc;

/// Make \p Func enter its body directly: its entry block, which compares the
/// stack pointer with the goroutine's stack guard, branches straight to the
/// block after it, and the growth block at \p GrowthStart, which spills the
/// register arguments, calls the runtime's morestack and starts the function
/// again (GoStackGrowth::Restarts), is emptied for simplifyCfg to drop.
/// False, leaving \p Func as it was, where the entry does not branch to that
/// block or the body.
bool elideGoStackGrowth(MedFunc &Func, va_t GrowthStart);
} // namespace neverd

#endif // NEVERD_LIB_IR_MED_LOWER_GOSTACKGROWTH_H
