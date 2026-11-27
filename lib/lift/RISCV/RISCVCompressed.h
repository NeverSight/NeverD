//===- RISCVCompressed.h - Integer RVC expansion ----------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Expand integer C encodings into the equivalent base word. The caller retains
/// the compressed PC and physical size for links and fallthrough.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_LIB_LIFT_RISCV_RISCVCOMPRESSED_H
#define NEVERD_LIB_LIFT_RISCV_RISCVCOMPRESSED_H
#include "neverd/lift/RISCV/RISCVProfile.h"

#include <cstdint>
#include <optional>

namespace neverd::riscv {
std::optional<uint32_t> expandCompressed(uint16_t Half, bool RV64,
                                         RISCVProfile Profile);
}
#endif // NEVERD_LIB_LIFT_RISCV_RISCVCOMPRESSED_H
