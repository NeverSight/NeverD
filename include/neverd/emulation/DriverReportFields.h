//===- DriverReportFields.h - Shared report vocabulary -------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Field names and stop spellings shared by JSON producers and consumers.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_EMULATION_DRIVERREPORTFIELDS_H
#define NEVERD_EMULATION_DRIVERREPORTFIELDS_H

namespace neverd::emulation {
namespace field {
#define NEVERD_DRIVER_REPORT_FIELD(Name, Spelling)                             \
  inline constexpr char Name[] = Spelling;
#include "neverd/emulation/DriverReportFields.def"
#undef NEVERD_DRIVER_REPORT_FIELD
} // namespace field
namespace stop {
#define NEVERD_DRIVER_STOP(Name, Spelling)                                     \
  inline constexpr char Name[] = Spelling;
#include "neverd/emulation/DriverStopReasons.def"
#undef NEVERD_DRIVER_STOP
} // namespace stop
} // namespace neverd::emulation

#endif // NEVERD_EMULATION_DRIVERREPORTFIELDS_H
