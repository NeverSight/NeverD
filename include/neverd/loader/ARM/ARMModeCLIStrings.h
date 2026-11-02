//===- ARMModeCLIStrings.h - AArch32 mode CLI spelling --------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#ifndef NEVERD_LOADER_ARM_ARMMODECLISTRINGS_H
#define NEVERD_LOADER_ARM_ARMMODECLISTRINGS_H

namespace neverd::arm_mode_cli {

#define NEVERD_ARM_MODE_CLI_STRING(Name, Spelling)                             \
  inline constexpr char Name[] = Spelling;
#include "neverd/loader/ARM/ARMModeCLIStrings.def"
#undef NEVERD_ARM_MODE_CLI_STRING

} // namespace neverd::arm_mode_cli

#endif // NEVERD_LOADER_ARM_ARMMODECLISTRINGS_H
