//===- MIPSHistoricalCP0.cpp - Historical CP0 and exception entry --------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "neverd/ir/low/MIPS/MIPSHistoricalCP0.h"

#include <algorithm>

namespace neverd::mips {
namespace {
constexpr uint32_t StatusIE = 1, StatusEXL = 2, StatusERL = 4;
constexpr uint32_t StatusIM = 0xff00, StatusBEV = 1u << 22;
constexpr uint32_t StatusCU = 0xf0000000u, CauseBD = 1u << 31;
constexpr uint32_t SoftwareIP = 0x300u;
constexpr uint32_t StatusKSU = 0x18, StatusUX = 0x20, StatusSX = 0x40;

constexpr uint16_t R30xxIM = 0xff, R30xxIE = 0x100;
uint16_t r30xxInterruptFields(uint32_t Status, uint32_t Cause) {
  return ((Status & StatusIM) >> 8) | ((Status & StatusIE) << 8) |
         ((Cause & SoftwareIP) << 1);
}

bool hasRegisterHazards(const HistoricalCP0 &CPU) {
  constexpr std::array<uint8_t, 4> Clear{};
  return CPU.CoprocessorDelay != Clear || CPU.ReadDelay != Clear;
}

bool enabledInterrupt(const HistoricalCP0 &CPU) {
  return (CPU.Status & StatusIE) &&
         (CPU.usesModeStack() || !(CPU.Status & (StatusEXL | StatusERL))) &&
         (CPU.Status & CPU.Cause & StatusIM);
}

std::optional<unsigned> readHazardSlot(unsigned Register) {
  if (Register >= 12 && Register <= 14)
    return Register - 12;
  return Register == 30 ? std::optional(3u) : std::nullopt;
}
} // namespace

bool HistoricalCP0::usesModeStack() const {
  return Processor == HistoricalProcessor::R30xx ||
         Processor == HistoricalProcessor::CW4011R3000;
}

bool HistoricalCP0::supportsLevel(unsigned Level) const {
  switch (Processor) {
  case HistoricalProcessor::R30xx:
    return Level == 1 && (!R30xxRecognized.has_value() || InterruptDelay) &&
           InterruptDelay <= R30xxInterrupts.size() &&
           std::all_of(R30xxInterrupts.begin(),
                       R30xxInterrupts.begin() + InterruptDelay,
                       [](R30xxInterruptVisibility V) {
                         return !(V.Required & ~V.Possible) &&
                                !(V.Possible &
                                  ~R30xxInterruptVisibility::AllFields);
                       });
  case HistoricalProcessor::CW4011R3000:
  case HistoricalProcessor::CW4011R4000:
    // The CW4011 has a word-aligned 32-bit ErrorEPC in both modes. In
    // R4000-compatible mode its two defined base modes are kernel and user.
    return Level == 2 &&
           (!ErrorEPC || (*ErrorEPC <= UINT32_MAX && !(*ErrorEPC & 3))) &&
           (usesModeStack() || !(Status & 8));
  case HistoricalProcessor::R4000:
    return Level == 3 && (Status & StatusKSU) != StatusKSU;
  }
  return false;
}

bool HistoricalCP0::canUseCP0() const {
  const bool Kernel =
      usesModeStack() ? !(Status & 2)
                      : (Status & (StatusEXL | StatusERL)) || !(Status & 0x18);
  return Kernel || (Status & (1u << 28));
}

bool HistoricalCP0::permitsDoublewordGPRs() const {
  if (Processor != HistoricalProcessor::R4000)
    return false;
  if ((Status & (StatusEXL | StatusERL)) || !(Status & StatusKSU))
    return true;
  return Status & ((Status & StatusKSU) == 8 ? StatusSX : StatusUX);
}

std::optional<uint64_t> HistoricalCP0::read(unsigned Register,
                                            bool Doubleword) const {
  const auto Slot = readHazardSlot(Register);
  if ((Slot && ReadDelay[*Slot]) || !canUseCP0())
    return std::nullopt;
  if (Doubleword && (Processor != HistoricalProcessor::R4000 ||
                     (Register != 8 && Register != 14 && Register != 30)))
    return std::nullopt;
  switch (Register) {
  case 8:
    return BadVAddr;
  case 12:
    return Status;
  case 13:
    return Cause;
  case 14:
    return EPC;
  case 30:
    return Processor == HistoricalProcessor::R30xx ? std::nullopt : ErrorEPC;
  default:
    return std::nullopt;
  }
}

bool HistoricalCP0::canWrite(unsigned Register, uint64_t Value,
                             bool Doubleword) const {
  if (!canUseCP0() || (Doubleword && (Processor != HistoricalProcessor::R4000 ||
                                      (Register != 14 && Register != 30))))
    return false;
  if (Register == 14 || Register == 30)
    return Processor == HistoricalProcessor::R4000 ||
           (Processor != HistoricalProcessor::R30xx && !(Value & 3));
  if (Register == 13)
    return true; // Only IP[1:0] is writable.
  if (Register != 12)
    return false;
  const auto Changed = Status ^ uint32_t(Value);
  uint32_t Writable = StatusIE | StatusIM | StatusCU;
  if (Processor == HistoricalProcessor::R4000) {
    // A kernel handler can change the saved KSU/UX/SX while an exception
    // level stays set. F-2 excludes changing KSU while entering that level.
    if (!(Status & StatusKSU))
      Writable |= StatusEXL | StatusERL;
    if ((Status & Value & (StatusEXL | StatusERL)) &&
        !(Changed & (StatusEXL | StatusERL)) &&
        (Value & StatusKSU) != StatusKSU)
      Writable |= StatusKSU | StatusUX | StatusSX;
  }
  return !(Changed & ~Writable);
}

std::optional<unsigned> HistoricalCP0::coprocessorOf(uint32_t Word) {
  const unsigned Major = Word >> 26;
  if ((Major >= 16 && Major <= 19) || Major == 49 || Major == 53 ||
      Major == 57 || Major == 61)
    return Major & 3;
  return std::nullopt;
}

bool HistoricalCP0::coprocessorUsable(unsigned Number) const {
  return Number == 0 ? canUseCP0()
                     : Number < 4 && (Status & (1u << (28 + Number)));
}

bool HistoricalCP0::permitsIssue(uint32_t Word) const {
  if (InterruptDelay && Processor == HistoricalProcessor::R30xx &&
      !pendingR30xxInterrupt().has_value())
    return false;
  if (!hasRegisterHazards(*this))
    return true;
  const unsigned Major = Word >> 26, RS = Word >> 21 & 31;
  if (const auto Cop = coprocessorOf(Word); Cop && CoprocessorDelay[*Cop])
    return false;
  const unsigned Register = Word >> 11 & 31;
  if (Major == 16 && (RS == 0 || RS == 1))
    if (const auto Slot = readHazardSlot(Register); Slot && ReadDelay[*Slot])
      return false;
  return true;
}

void HistoricalCP0::retire(std::optional<unsigned> Register, uint64_t Value,
                           bool Doubleword) {
  const auto Age = [](uint8_t &Delay) {
    if (Delay)
      --Delay;
  };
  if (InterruptDelay) {
    if (Processor == HistoricalProcessor::R30xx) {
      R30xxInterrupts[0] = R30xxInterrupts[1];
      R30xxRecognized.reset();
    }
    --InterruptDelay;
  }
  Age(ExceptionReturnDelay);
  Age(ExceptionModeDelay);
  if (hasRegisterHazards(*this)) {
    for (auto &Delay : CoprocessorDelay)
      Age(Delay);
    for (auto &Delay : ReadDelay)
      Age(Delay);
  }
  if (!Register)
    return;
  const bool R30xx = Processor == HistoricalProcessor::R30xx;
  const bool R4000 = Processor == HistoricalProcessor::R4000;
  const uint16_t Previous = R30xx ? r30xxInterruptFields(Status, Cause) : 0;
  ReadDelay[*readHazardSlot(*Register)] = R4000 ? 2 : R30xx ? 3 : 1;
  if (*Register == 14 || *Register == 30) {
    ExceptionReturnDelay =
        std::max<uint8_t>(ExceptionReturnDelay, R4000 ? 2 : 1);
    if (!Doubleword) {
      const auto Word = uint32_t(Value);
      Value = R4000 ? uint64_t(int64_t(int32_t(Word))) : Word;
    }
    if (*Register == 30)
      ErrorEPC = Value;
    else
      EPC = Value;
  } else if (*Register == 12) {
    const auto Changed = Status ^ uint32_t(Value);
    Status = uint32_t(Value);
    ExceptionReturnDelay = std::max<uint8_t>(ExceptionReturnDelay, R4000   ? 3
                                                                   : R30xx ? 3
                                                                           : 1);
    if (R4000 && (Changed & (StatusEXL | StatusERL)))
      ExceptionModeDelay = 3;
    if (!R30xx &&
        (Changed & (StatusIE | StatusIM | (R4000 ? StatusEXL | StatusERL : 0))))
      InterruptDelay = R4000 ? 3 : 1;
    for (unsigned I = 0; I < 4; ++I)
      if (Changed & (1u << (28 + I)))
        CoprocessorDelay[I] = R4000 ? 4 : R30xx ? 2 : 1;
  } else if (*Register == 13) {
    if (!R30xx && ((Cause ^ uint32_t(Value)) & SoftwareIP))
      InterruptDelay = R4000 ? 3 : 1;
    Cause = (Cause & ~SoftwareIP) | (uint32_t(Value) & SoftwareIP);
  }
  if (R30xx && (*Register == 12 || *Register == 13))
    propagateR30xxInterrupts(Previous, r30xxInterruptFields(Status, Cause));
}

void HistoricalCP0::propagateR30xxInterrupts(uint16_t Before, uint16_t After) {
  const uint16_t Changed = Before ^ After;
  if (!Changed)
    return;
  // R30xx manual 13-3. An ordinary enable cannot interrupt the next two
  // instructions. A disable may still admit an old source. IEc and IM can
  // reach the controller separately, including across overlapping writes.
  for (unsigned I = 0; I < R30xxInterrupts.size(); ++I) {
    auto View = I < InterruptDelay ? R30xxInterrupts[I]
                                   : R30xxInterruptVisibility{Before, Before};
    // Opposing gate/mask changes can momentarily open a line that both
    // endpoints close. Setting both gates from zero still needs two slots.
    if ((View.Possible & R30xxIE) && !(After & R30xxIE))
      View.MixedLines |= R30xxIM & After & ~View.Required;
    if (!(View.Required & R30xxIE) && (After & R30xxIE))
      View.MixedLines |= R30xxIM & View.Possible & ~After;
    View.Required &= ~Changed;
    R30xxInterrupts[I] = View;
  }
  InterruptDelay = R30xxInterrupts.size();
}

void HistoricalCP0::restoreR30xxInterruptEnable() {
  // Exception entry and RFE set IEc as part of the mode-stack operation.
  // Keep any independently propagating IM or software-IP bits.
  if (Processor != HistoricalProcessor::R30xx || !InterruptDelay)
    return;
  R30xxRecognized.reset();
  const uint16_t IE = Status & StatusIE ? R30xxIE : 0;
  for (auto &View : R30xxInterrupts) {
    View.Required = (View.Required & ~R30xxIE) | IE;
    View.Possible = (View.Possible & ~R30xxIE) | IE;
    View.MixedLines = 0;
  }
}

void HistoricalCP0::restoreExceptionState() {
  if (usesModeStack()) {
    Status = (Status & ~0xfu) | ((Status >> 2) & 0xf);
    restoreR30xxInterruptEnable();
  } else
    Status &= ~(Status & StatusERL ? StatusERL : StatusEXL);
}

std::optional<bool> HistoricalCP0::pendingR30xxInterrupt() const {
  const auto Asserted = [&](uint16_t Fields, uint8_t MixedLines) {
    const auto Pending = ((Cause >> 8) & 0xfc) | ((Fields >> 9) & 3);
    const auto Enabled = Fields & R30xxIE ? Fields & R30xxIM : 0;
    return ((Enabled | MixedLines) & Pending) != 0;
  };
  const auto &View = R30xxInterrupts.front();
  if (Asserted(View.Required, 0))
    return true;
  if (!Asserted(View.Possible, View.MixedLines))
    return false;
  return R30xxRecognized;
}

std::optional<bool> HistoricalCP0::interruptRecognition() const {
  if (InterruptDelay && Processor == HistoricalProcessor::R30xx)
    return pendingR30xxInterrupt();
  return !InterruptDelay && enabledInterrupt(*this);
}

bool HistoricalCP0::canTakeInterrupt() const {
  if (InterruptDelay)
    return Processor == HistoricalProcessor::R30xx &&
           pendingR30xxInterrupt().value_or(false);
  return enabledInterrupt(*this);
}

bool HistoricalCP0::enterException(uint8_t Code, uint64_t PC,
                                   std::optional<uint64_t> Branch,
                                   std::optional<uint64_t> FaultAddress,
                                   bool Taken,
                                   std::optional<unsigned> Coprocessor) {
  if (Code != 0 && Code != 4 && Code != 5 && Code != 8 && Code != 9 &&
      Code != 10 && Code != 11 && Code != 12 &&
      (Code != 13 || Processor == HistoricalProcessor::R30xx))
    return false;
  if (ExceptionModeDelay || (Code == 11) != Coprocessor.has_value() ||
      (Coprocessor && *Coprocessor >= 4) ||
      (Code == 4 || Code == 5) != FaultAddress.has_value() ||
      (Code == 0 && !canTakeInterrupt()))
    return false;
  // A fault never retires the victim.
  if (usesModeStack() || !(Status & StatusEXL)) {
    EPC = Branch.value_or(PC);
  }
  const bool CW4011 = tracksBranchTaken();
  if (usesModeStack() || !(Status & StatusEXL) || CW4011)
    Cause = Branch ? Cause | CauseBD : Cause & ~CauseBD;
  if (CW4011) {
    constexpr uint32_t CauseBT = 1u << 30;
    Cause = Branch && Taken ? Cause | CauseBT : Cause & ~CauseBT;
  }
  if (Coprocessor)
    Cause = (Cause & ~(3u << 28)) | (*Coprocessor << 28);
  Cause = (Cause & ~0x7cu) | (uint32_t(Code) << 2);
  if (FaultAddress || usesModeStack())
    BadVAddr = FaultAddress;
  const bool BEV = Status & StatusBEV;
  if (usesModeStack()) {
    Status = (Status & ~0x3fu) | ((Status << 2) & 0x3fu);
    restoreR30xxInterruptEnable();
  } else
    Status |= StatusEXL;
  const auto Base = BEV ? 0xbfc00000u : 0x80000000u;
  const auto Offset = (BEV ? (usesModeStack() ? 0x100u : 0x200u) : 0u) +
                      (usesModeStack() ? 0x80u : 0x180u);
  const auto Vector = Base + Offset;
  ExceptionVector = Processor == HistoricalProcessor::R4000
                        ? uint64_t(int64_t(int32_t(Vector)))
                        : uint64_t(Vector);
  return true;
}
} // namespace neverd::mips
