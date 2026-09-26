//===- DriverProfile.h - Shared driver execution profile ------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Named constants shared by image loading, Windows models, and CPU execution.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_EMULATION_DRIVERPROFILE_H
#define NEVERD_EMULATION_DRIVERPROFILE_H

#include <cstdint>

namespace neverd::emulation::profile {
#define NEVERD_DRIVER_PROFILE(Name, Value)                                     \
  inline constexpr uint64_t Name = Value;
#include "neverd/emulation/DriverProfile.def"
#undef NEVERD_DRIVER_PROFILE

#define NEVERD_DRIVER_PROFILE_STRING(Name, Value)                              \
  inline constexpr char Name[] = Value;
#include "neverd/emulation/DriverProfileStrings.def"
#undef NEVERD_DRIVER_PROFILE_STRING
} // namespace neverd::emulation::profile

#endif // NEVERD_EMULATION_DRIVERPROFILE_H
