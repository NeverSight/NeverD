//===- X64RtmAbortRegions.cpp - Bound immediate RTM aborts ---------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Keep the complete native transaction in one compiler operation and transport
/// its actual abort status without guessing an outcome or rolling back C.
//===----------------------------------------------------------------------===//

#include "X64RtmAbortRegions.h"

#include "X64RtmRegions.h"

#include "neverd/analysis/arch/x86_64/X64ContinuationSource.h"
#include "neverd/decode/X86/X86TransactionEncoding.h"
#include "neverd/loader/BinaryImage.h"

#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/Errc.h"
#include "llvm/Support/raw_ostream.h"

#include <cstdint>
#include <limits>
#include <vector>

namespace neverd {

namespace {
// Leaf 7:0 uses bit 11 in both EBX (RTM) and EDX (RTM_ALWAYS_ABORT).
// Either admits this immediate-abort pair. Only stronger evidence fixes EAX.
constexpr unsigned kRtmBit = 11;
constexpr uint32_t kRtm = 1U << kRtmBit;
constexpr uint32_t kHle = 1U << 4;
constexpr uint32_t kRtmForceAbortMsr = 1U << 13;
constexpr uint32_t kHardwiredAbort = kRtm | kRtmForceAbortMsr;
constexpr uint32_t kArchCapabilitiesMsr = 1U << 29;
constexpr uint64_t kTsxCtrlMsr = 1ULL << 7;
constexpr uint64_t kRtmDisable = 1;
constexpr uint64_t kTsxCpuidClear = 2;
constexpr uint64_t kForceAbort = 1;
constexpr uint64_t kSdvEnableRtm = 4;

template <typename T>
std::optional<T> cpuWord(const llvm::json::Object &Record,
                         llvm::StringRef Key) {
  const auto *Value = Record.get(Key);
  if (!Value)
    return std::nullopt;
  if (const auto Number = Value->getAsUINT64();
      Number && *Number <= std::numeric_limits<T>::max())
    return static_cast<T>(*Number);
  const auto Text = Value->getAsString();
  T Number;
  if (Text && !Text->empty() && !Text->starts_with('-') &&
      !Text->getAsInteger(0, Number))
    return Number;
  return std::nullopt;
}

llvm::StringRef forcedAbortEvidence(const X64RtmAbortSelection &CPU) {
  if (CPU.TsxCtrl.value_or(0) & kRtmDisable)
    return "ia32_tsx_ctrl.rtm_disable";
  if (CPU.TsxForceAbort.value_or(0) & kForceAbort)
    return "ia32_tsx_force_abort.rtm_force_abort";
  // With both EDX bits set IA32_TSX_FORCE_ABORT[0] is fixed at one.
  if ((CPU.Leaf7EDX.value_or(0) & kHardwiredAbort) == kHardwiredAbort)
    return "rtm_always_abort+rtm_force_abort";
  return {};
}
} // namespace

llvm::Expected<X64RtmAbortSelection>
X64RtmAbortSelection::read(const llvm::json::Value &V) {
  X64RtmAbortSelection Result;
  if (const auto Flag = V.getAsBoolean()) {
    Result.Kind = *Flag ? Supported : Off;
    return Result;
  }
  if (V.getAsString() == "auto") {
    Result.Kind = Probe;
    return Result;
  }
  const auto *Record = V.getAsObject();
  if (!Record)
    return llvm::createStringError(llvm::errc::invalid_argument,
                                   "native_rtm_abort_steps requires a boolean, "
                                   "auto or target CPU record");
  Result.MaxBasic = cpuWord<uint32_t>(*Record, "max_basic_leaf");
  Result.Leaf7EBX = cpuWord<uint32_t>(*Record, "leaf7_ebx");
  Result.Leaf7EDX = cpuWord<uint32_t>(*Record, "leaf7_edx");
  Result.ArchCapabilities =
      cpuWord<uint64_t>(*Record, "ia32_arch_capabilities");
  Result.TsxCtrl = cpuWord<uint64_t>(*Record, "ia32_tsx_ctrl");
  Result.TsxForceAbort = cpuWord<uint64_t>(*Record, "ia32_tsx_force_abort");
  const unsigned Fields =
      Result.MaxBasic.has_value() + Result.Leaf7EBX.has_value() +
      Result.Leaf7EDX.has_value() + Result.ArchCapabilities.has_value() +
      Result.TsxCtrl.has_value() + Result.TsxForceAbort.has_value();
  if (Record->size() != Fields)
    return llvm::createStringError(llvm::errc::invalid_argument,
                                   "unknown or invalid RTM CPU record field");
  if (!Result.MaxBasic ||
      (*Result.MaxBasic >= 7) != Result.Leaf7EBX.has_value() ||
      (Result.Leaf7EDX && !Result.Leaf7EBX))
    return llvm::createStringError(llvm::errc::invalid_argument,
                                   "RTM CPUID requires max_basic_leaf and "
                                   "leaf7_ebx exactly when leaf 7 exists");
  const uint32_t EBX = Result.Leaf7EBX.value_or(0);
  const uint32_t EDX = Result.Leaf7EDX.value_or(0);
  const uint64_t Ctrl = Result.TsxCtrl.value_or(0);
  const uint64_t Force = Result.TsxForceAbort.value_or(0);
  if ((Result.ArchCapabilities && !(EDX & kArchCapabilitiesMsr)) ||
      (Result.TsxCtrl &&
       !(Result.ArchCapabilities.value_or(0) & kTsxCtrlMsr)) ||
      (Result.TsxForceAbort && !(EDX & kRtmForceAbortMsr)))
    return llvm::createStringError(llvm::errc::invalid_argument,
                                   "RTM CPU record uses an unenumerated MSR");
  if (Ctrl & ~(kRtmDisable | kTsxCpuidClear))
    return llvm::createStringError(llvm::errc::invalid_argument,
                                   "reserved IA32_TSX_CTRL bits");
  if (Result.TsxForceAbort) {
    // EDX[11] fixes FORCE_ABORT at one. SDV_ENABLE_RTM clears both.
    // CPUID_CLEAR is implemented only in either of those two modes.
    const bool Always = EDX & kRtm;
    const bool Sdv = Force & kSdvEnableRtm;
    if ((Force & ~(kForceAbort | kTsxCpuidClear | kSdvEnableRtm)) ||
        (Always && !(Force & kForceAbort)) ||
        (Sdv && (Always || (Force & kForceAbort))) ||
        ((Force & kTsxCpuidClear) && !Always && !Sdv))
      return llvm::createStringError(llvm::errc::invalid_argument,
                                     "inconsistent IA32_TSX_FORCE_ABORT");
  }
  if (((Ctrl | Force) & kTsxCpuidClear) && (EBX & (kRtm | kHle)))
    return llvm::createStringError(
        llvm::errc::invalid_argument,
        "TSX_CPUID_CLEAR contradicts RTM/HLE enumeration");
  if (!forcedAbortEvidence(Result).empty())
    Result.Kind = ForcedAbort;
  else
    Result.Kind = ((EBX | EDX) & kRtm) ? Supported : Unadvertised;
  return Result;
}

llvm::json::Object
X64RtmAbortSelection::report(llvm::json::Array Regions) const {
  llvm::json::Object R{
      {"regions", std::move(Regions)},
      {"domain", "original execution CPU, outside RTM/HLE, successful "
                 "instruction fetch and no debug trap"},
      {"budget",
       "one translated step per attempted pair; zero on capability refusal"},
      {"selection", Kind == Probe                       ? "executing-cpu-cpuid"
                    : ArchCapabilities || TsxForceAbort ? "captured-cpu-state"
                    : MaxBasic                          ? "captured-cpuid"
                                                        : "declared-rtm"},
      {"can_decline", canDecline()}};
  if (Kind == ForcedAbort) {
    R["forced_abort_status"] = 0;
    R["forced_abort_evidence"] = forcedAbortEvidence(*this);
  } else {
    R["native_intrinsic"] = "llvm.x86.rtm.abort.pair";
  }
  if (MaxBasic)
    R["max_basic_leaf"] = *MaxBasic;
  if (Leaf7EBX)
    R["leaf7_ebx"] = *Leaf7EBX;
  if (Leaf7EDX)
    R["leaf7_edx"] = *Leaf7EDX;
  if (ArchCapabilities)
    R["ia32_arch_capabilities"] = "0x" + llvm::utohexstr(*ArchCapabilities);
  if (TsxCtrl)
    R["ia32_tsx_ctrl"] = "0x" + llvm::utohexstr(*TsxCtrl);
  if (TsxForceAbort)
    R["ia32_tsx_force_abort"] = "0x" + llvm::utohexstr(*TsxForceAbort);
  return R;
}

std::optional<X64RtmAbortRegion> readX64RtmAbortRegion(const BinaryImage &Image,
                                                       va_t Address,
                                                       unsigned AddressBits) {
  if (Image.Arch != Arch::X64 || (AddressBits != 48 && AddressBits != 57))
    return std::nullopt;
  const auto Read =
      [&](va_t PC,
          X86TransactionKind Kind) -> std::optional<llvm::ArrayRef<uint8_t>> {
    for (unsigned Size = 3; Size <= x86::kMaxInstructionLen; ++Size) {
      if (PC > std::numeric_limits<va_t>::max() - (Size - 1))
        return std::nullopt;
      const auto *Bytes = Image.readVA(PC, Size);
      if (!Bytes || !Image.isCodeAddress(PC) ||
          !Image.isCodeAddress(PC + Size - 1))
        return std::nullopt;
      const auto E = x86TransactionEncoding({Bytes, Size}, PC, true);
      if (E)
        return E->Kind == Kind ? std::optional(llvm::ArrayRef(Bytes, Size))
                               : std::nullopt;
    }
    return std::nullopt;
  };
  const auto Begin = Read(Address, X86TransactionKind::Begin);
  if (!Begin || Address > std::numeric_limits<va_t>::max() - Begin->size())
    return std::nullopt;
  const auto Fallback = x64RtmFallback(*Begin, Address, AddressBits);
  const auto Abort = Read(Address + Begin->size(), X86TransactionKind::Abort);
  if (!Fallback || !Abort)
    return std::nullopt;
  X64RtmAbortRegion Region{Address, *Fallback, {}};
  Region.Bytes.append(Begin->begin(), Begin->end());
  Region.Bytes.append(Abort->begin(), Abort->end());
  return Region;
}

namespace {
void emitCanRunPair(llvm::raw_ostream &OS) {
  OS << "static __inline__ __attribute__((__always_inline__)) int "
        "__neverd_x64_can_run_abort_pair(void) {\n"
        "        uint32_t b,d;\n"
        "#if __has_builtin(__builtin_ia32_cpuidex)\n"
        "        int info[4];\n"
        "        __builtin_ia32_cpuidex(info,0,0);\n"
        "        if ((uint32_t)info[0] < 7) return 0;\n"
        "        __builtin_ia32_cpuidex(info,7,0);\n"
        "        b=(uint32_t)info[1]; d=(uint32_t)info[3];\n"
        "#else\n"
        "        uint32_t a,c;\n"
        "        __asm__ volatile(\"cpuid\" : "
        "\"=a\"(a),\"=b\"(b),\"=c\"(c),\"=d\"(d)\n"
        "                         : \"0\"(0),\"2\"(0) : \"cc\");\n"
        "        if (a < 7) return 0;\n"
        "        __asm__ volatile(\"cpuid\" : "
        "\"=a\"(a),\"=b\"(b),\"=c\"(c),\"=d\"(d)\n"
        "                         : \"0\"(7),\"2\"(0) : \"cc\");\n"
        "#endif\n"
        "        return ((b | d) >> "
     << kRtmBit << ") & 1U;\n}\n";
}

void emitAbortResult(llvm::raw_ostream &OS, const X64RtmAbortRegion &Region,
                     llvm::StringRef Status) {
  OS << "        state->GPR[0]=" << Status << ";\n        state->PC=0x"
     << llvm::utohexstr(Region.Fallback) << "ULL;\n";
}

/// Both owners issue the entire transaction in one compiler operation. EAX
/// is the actual result, including an abort before XABORT is reached.
void emitAbortPair(llvm::raw_ostream &OS, const X64RtmAbortRegion &Region) {
  const auto Code = unsigned(Region.Bytes.back());
  OS << "        uint32_t status;\n"
        "#if __has_builtin(__builtin_ia32_rtm_abort_pair)\n"
        "        status=__builtin_ia32_rtm_abort_pair("
     << Code
     << ");\n#else\n"
        "        __asm__ volatile(\"xbegin 1f; xabort $"
     << Code << "; 1:\" : \"=a\"(status) : : \"memory\",\"cc\");\n#endif\n";
  emitAbortResult(OS, Region, "status");
}

llvm::json::Object receipt(const X64RtmAbortRegion &Region) {
  return llvm::json::Object{
      {"pc", "0x" + llvm::utohexstr(Region.Address)},
      {"bytes", llvm::toHex(Region.Bytes)},
      {"fallback", "0x" + llvm::utohexstr(Region.Fallback)},
      {"abort_code", unsigned(Region.Bytes.back())}};
}
} // namespace

X64RtmAbortDispatch::X64RtmAbortDispatch(
    X64RtmAbortSelection::Mode Mode, llvm::ArrayRef<X64RtmAbortStep> Steps) {
  if (Mode == X64RtmAbortSelection::Probe)
    for (const auto &Step : Steps)
      Sites.insert(Step.Region.Address);
}

void X64RtmAbortDispatch::declare(
    llvm::raw_ostream &OS,
    llvm::ArrayRef<analysis::X64ContinuationInstructionSource> Units) const {
  if (!Sites.empty() && llvm::any_of(Units, [&](const auto &Unit) {
        return Sites.contains(Unit.Address);
      }))
    OS << "    unsigned rtm_pair_admitted = 0;\n";
}

void X64RtmAbortDispatch::emit(llvm::raw_ostream &OS,
                               llvm::StringRef Name) const {
  OS << "status = " << Name
     << "_checked((uint8_t *)state, &rtm_pair_admitted);";
}

llvm::json::Array emitX64RtmAbortSteps(llvm::raw_ostream &OS,
                                       llvm::ArrayRef<X64RtmAbortStep> Steps,
                                       X64RtmAbortSelection::Mode Mode) {
  using Exit = analysis::X64ContinuationExitV1;
  llvm::json::Array Receipts;
  if (Mode == X64RtmAbortSelection::Probe)
    emitCanRunPair(OS);
  for (const auto &[Name, Region] : Steps) {
    Receipts.push_back(receipt(Region));
    if (Mode == X64RtmAbortSelection::Unadvertised)
      continue;
    const bool Probe = Mode == X64RtmAbortSelection::Probe;
    if (Probe)
      OS << "static uint64_t " << Name
         << "_checked(uint8_t *raw, unsigned *admitted)";
    else
      OS << "uint64_t " << Name << "(uint8_t *raw)";
    OS << " {\n        uint64_t check=" << Name
       << "_guard(raw);\n"
          "        if (check!="
       << uint64_t(Exit::NativeInstruction)
       << ") return check;\n"
          "        neverd_x64_state_v6 *state=(neverd_x64_state_v6 *)(void "
          "*)raw;\n";
    if (Probe)
      OS << "        if (!*admitted) {\n"
            "            if (!__neverd_x64_can_run_abort_pair()) return "
            "check;\n"
            "            *admitted = 1;\n        }\n";
    if (Mode == X64RtmAbortSelection::ForcedAbort)
      emitAbortResult(OS, Region, "0");
    else
      emitAbortPair(OS, Region);
    OS << "        return " << uint64_t(Exit::Jump) << ";\n}\n";
    if (Probe)
      OS << "uint64_t " << Name << "(uint8_t *raw) {\n"
         << "    unsigned admitted = 0;\n    return " << Name
         << "_checked(raw, &admitted);\n}\n";
  }
  return Receipts;
}

llvm::json::Array emitX64RtmAbortEnvironment(
    llvm::raw_ostream &OS, const BinaryImage &Image,
    llvm::ArrayRef<analysis::X64ContinuationInstructionSource> Units,
    unsigned AddressBits) {
  std::vector<X64RtmAbortRegion> Regions;
  for (const auto &Unit : Units)
    if (Unit.RequiresNativeExecution &&
        x86TransactionBeginEncoding(Unit.Bytes, Unit.Address, true))
      if (auto Region = readX64RtmAbortRegion(Image, Unit.Address, AddressBits))
        Regions.push_back(*Region);
  llvm::json::Array Receipts;
  if (!Regions.empty())
    emitCanRunPair(OS);
  OS << R"C(
/* Explicit immediate-abort RTM environment, for use with neverd_x64_run_v6.
 * Invoke outside RTM/HLE in the declared nonfaulting, no-debug-trap domain.
 * The complete pair runs in one native operation; no transaction spans a
 * C return or a translated step. Actual EAX status is zero-extended into RAX.
 * One environment budget unit covers the pair, including an early abort.
 * CPUID RTM or RTM_ALWAYS_ABORT admits the pair. Otherwise the provider declines. */
int neverd_x64_rtm_abort_environment_v6(neverd_x64_state_v6 *state,
                                       uint64_t boundary, void *context) {
    (void)context;
    if (!state || boundary != 4) return 0;
    switch (state->PC) {
)C";
  for (const auto &Region : Regions) {
    const auto PC = "0x" + llvm::utohexstr(Region.Address);
    OS << "    case " << PC
       << "ULL: {\n"
          "        if (!__neverd_x64_can_run_abort_pair()) return 0;\n";
    emitAbortPair(OS, Region);
    OS << "        return 1;\n    }\n";
    Receipts.push_back(receipt(Region));
  }
  OS << "    default: return 0;\n    }\n}\n";
  return Receipts;
}

} // namespace neverd
