//===- AndroidCalendar.cpp - Deterministic Bionic calendar conversion -----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "AndroidInternal.h"

#include "llvm/ADT/STLExtras.h"
#include "llvm/Support/Endian.h"

#include <array>
#include <bit>

namespace neverd::emulation::android_model {
namespace {
// Process-owned storage, disjoint from TLS, property, JNI and thread arenas.
// This placement is model policy, not an Android library virtual address.
constexpr uint64_t CalendarTM = 0x7030000000;
constexpr uint64_t GMTName = CalendarTM + PageSize;
constexpr uint64_t LocalName = GMTName + 64;
constexpr unsigned TMSize = 56, OffsetOffset = 40, ZoneOffset = 48;
bool calendarSymbol(llvm::StringRef Name) {
  return Name == symbol::LocalTime || Name == symbol::LocalTimeR ||
         Name == symbol::GMTime || Name == symbol::GMTimeR;
}
constexpr bool leap(int64_t Year) {
  return Year % 4 == 0 && (Year % 100 != 0 || Year % 400 == 0);
}
// Days since 1970-01-01. Only positive civil years are passed here.
constexpr int64_t firstDay(int64_t Year) {
  int64_t Prior = Year - 1;
  return Prior * 365 + Prior / 4 - Prior / 100 + Prior / 400 - 719162;
}
} // namespace

llvm::Error validateCalendar(const AndroidNativeOptions &Options) {
  if (!Options.FixedTimeZone)
    return llvm::Error::success();
  const auto &Z = *Options.FixedTimeZone;
  if (Z.OffsetSeconds < -86400 || Z.OffsetSeconds > 86400 || Z.Name.empty() ||
      Z.Name.size() > 32 || !llvm::all_of(Z.Name, [](unsigned char C) {
        return C >= 33 && C <= 126;
      }))
    return failure(diagnostic::CalendarZoneInput);
  return llvm::Error::success();
}

llvm::Error initializeCalendar(AddressSpace &Space,
                               const AndroidNativeOptions &Options,
                               const LinkedImage &Linked) {
  if (!llvm::any_of(Linked.Imports,
                    [](const auto &I) { return calendarSymbol(I.second); }))
    return llvm::Error::success();
  if (auto E =
          Space.map(CalendarTM, 2 * PageSize, Read | Write | UserAccessible))
    return E;
  std::array<uint8_t, PageSize> Names{};
  Names[0] = 'G';
  Names[1] = 'M';
  Names[2] = 'T';
  if (Options.FixedTimeZone)
    llvm::copy(Options.FixedTimeZone->Name, Names.begin() + 64);
  if (auto E = Space.write(GMTName, Names))
    return E;
  return Space.protect(GMTName, PageSize, Read | UserAccessible);
}

BionicResult Bionic::calendarTime(const NativeCallEvent &Call) {
  bool Local =
      Call.Name == symbol::LocalTime || Call.Name == symbol::LocalTimeR;
  bool Reentrant =
      Call.Name == symbol::LocalTimeR || Call.Name == symbol::GMTimeR;
  if (Local && !Options.Android->FixedTimeZone) {
    Result.Stop = ProcessStopReason::UnsupportedService;
    Result.Diagnostic = diagnostic::CalendarZoneUnspecified;
    return std::optional<BionicValue>();
  }
  int64_t Offset = Local ? Options.Android->FixedTimeZone->OffsetSeconds : 0;
  llvm::StringRef Name =
      Local ? llvm::StringRef(Options.Android->FixedTimeZone->Name)
            : llvm::StringRef("GMT");
  uint64_t Zone = Local ? LocalName : GMTName;
  if (auto E = access(Call.Arguments[0], 8, Read))
    return std::move(E);
  std::array<uint8_t, 8> Input;
  if (auto E = CPU.read(Call.Arguments[0], Input))
    return std::move(E);
  const int64_t Seconds =
      std::bit_cast<int64_t>(llvm::support::endian::read64le(Input.data()));
  // Split before adding the fixed offset, so INT64_MIN/MAX never overflow.
  int64_t Days = Seconds / 86400, Remainder = Seconds % 86400 + Offset;
  while (Remainder < 0) {
    Remainder += 86400;
    --Days;
  }
  while (Remainder >= 86400) {
    Remainder -= 86400;
    ++Days;
  }
  if (Days < firstDay(1) || Days >= firstDay(10000)) {
    // Bionic has wider civil-year and overflow side effects. Keep that
    // unimplemented boundary visible instead of inventing errno or tm bytes.
    Result.Stop = ProcessStopReason::UnsupportedService;
    Result.Diagnostic = diagnostic::CalendarRange;
    return std::optional<BionicValue>();
  }
  int64_t Low = 1, High = 10000;
  while (Low + 1 < High) {
    int64_t Mid = (Low + High) / 2;
    if (firstDay(Mid) <= Days)
      Low = Mid;
    else
      High = Mid;
  }
  const int64_t Year = Low;
  const int DayOfYear = Days - firstDay(Year);
  int Day = DayOfYear, Month = 0;
  constexpr int MonthDays[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
  while (true) {
    int Length = MonthDays[Month] + (Month == 1 && leap(Year));
    if (Day < Length)
      break;
    Day -= Length;
    ++Month;
  }
  int Weekday = (Days + 4) % 7;
  if (Weekday < 0)
    Weekday += 7;
  const int32_t Fields[] = {int32_t(Remainder % 60),
                            int32_t(Remainder / 60 % 60),
                            int32_t(Remainder / 3600),
                            Day + 1,
                            Month,
                            int32_t(Year - 1900),
                            Weekday,
                            DayOfYear,
                            0};
  std::array<uint8_t, TMSize> Bytes{};
  for (unsigned I = 0; I < std::size(Fields); ++I)
    llvm::support::endian::write32le(Bytes.data() + I * 4, uint32_t(Fields[I]));
  llvm::support::endian::write64le(Bytes.data() + OffsetOffset,
                                   uint64_t(Offset));
  llvm::support::endian::write64le(Bytes.data() + ZoneOffset, Zone);
  if (auto E = access(Zone, Name.size() + 1, Read))
    return std::move(E);
  std::vector<uint8_t> Stored(Name.size() + 1);
  if (auto E = CPU.read(Zone, Stored))
    return std::move(E);
  if (Stored.back() || !std::equal(Name.begin(), Name.end(), Stored.begin()))
    return failure(diagnostic::CalendarZoneChanged);
  const uint64_t Destination = Reentrant ? Call.Arguments[1] : CalendarTM;
  if (auto E = access(Destination, TMSize, Write))
    return std::move(E);
  // Preserve ABI padding at 36..39. Read time_t before any output write,
  // including when the caller's input and output objects overlap.
  if (auto E = CPU.write(Destination, llvm::ArrayRef(Bytes).take_front(36)))
    return std::move(E);
  if (auto E = CPU.write(Destination + OffsetOffset,
                         llvm::ArrayRef(Bytes).drop_front(OffsetOffset)))
    return std::move(E);
  return value(Destination);
}
} // namespace neverd::emulation::android_model
