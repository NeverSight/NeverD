//===- X64RtmCapture.cpp - Captured XBEGIN fault evidence ---------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Validate a stopped x64 vCPU and its same-ring, no-IST #UD frame before
/// binding one original XBEGIN. The live V6 event owner builds the replay.
//===----------------------------------------------------------------------===//

#include "X64RtmCapture.h"

#include "neverd/decode/X86/X86TransactionEncoding.h"

#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/Endian.h"
#include "llvm/Support/Errc.h"
#include "llvm/Support/FormatVariadic.h"
#include "llvm/Support/MathExtras.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <string>
#include <utility>

namespace neverd {

namespace {
llvm::Error invalid(const llvm::Twine &Message) {
  return llvm::createStringError(llvm::errc::invalid_argument, Message);
}

struct Stop {
  uint32_t Cpu;
  uint64_t Rip, Rsp, Rax, Flags, Efer, Cr4;
  uint16_t CS, SS;
};

std::optional<Stop> readStop(const llvm::json::Object *O,
                             unsigned AddressBits) {
  if (!O || O->size() != 10)
    return std::nullopt;
  const auto Cpu = rtmCaptureWord<uint32_t>(*O, "cpu");
  const auto Rip = rtmCaptureWord<uint64_t>(*O, "rip");
  const auto Rsp = rtmCaptureWord<uint64_t>(*O, "rsp");
  const auto Rax = rtmCaptureWord<uint64_t>(*O, "rax");
  const auto Flags = rtmCaptureWord<uint32_t>(*O, "eflags");
  const auto Efer = rtmCaptureWord<uint64_t>(*O, "efer");
  const auto Cr4 = rtmCaptureWord<uint64_t>(*O, "cr4");
  const auto CS = rtmCaptureWord<uint16_t>(*O, "cs");
  const auto SS = rtmCaptureWord<uint16_t>(*O, "ss");
  const auto Access = rtmCaptureWord<uint16_t>(*O, "cs_ar");
  // The stopped ring-zero fixture admits arithmetic flags, DF, AC and ID.
  // IF, TF and RF are clear. Reserved RFLAGS bits cannot certify a stop.
  constexpr uint32_t StoppedFlags = 0x240cd7;
  // LMA, present ring-zero long code, fixed RFLAGS bit 1.
  if (!Cpu || !Rip || !Rsp || !Rax || !Flags || !Efer || !Cr4 || !CS || !SS ||
      !Access || (*Efer & 0x400) == 0 || (*Access & 0x60f8) != 0x2098 ||
      !(*CS & 0xfffc) || ((*CS | *SS) & 3) ||
      ((*Flags & ~StoppedFlags) != 0 || (*Flags & 2) == 0) ||
      (((*Cr4 >> 12) & 1) != unsigned(AddressBits == 57)) ||
      !llvm::isIntN(AddressBits, static_cast<int64_t>(*Rip)) ||
      !llvm::isIntN(AddressBits, static_cast<int64_t>(*Rsp)))
    return std::nullopt;
  return Stop{*Cpu, *Rip, *Rsp, *Rax, *Flags, *Efer, *Cr4, *CS, *SS};
}

bool readBytes(const llvm::json::Object &O, llvm::StringRef Key, size_t Size,
               std::string &Out) {
  const auto Text = O.getString(Key);
  return Text && Text->size() == Size * 2 && llvm::tryGetFromHex(*Text, Out);
}

// IA-32e interrupt/trap gates split their offset over three fields.
uint64_t gateTarget(const char *Gate) {
  using namespace llvm::support::endian;
  return read16le(Gate) | (uint64_t(read16le(Gate + 6)) << 16) |
         (uint64_t(read32le(Gate + 8)) << 32);
}
} // namespace

llvm::Expected<va_t> rtmCaptureImagePC(va_t Rip, std::optional<va_t> LoadBase,
                                       va_t ImageBase) {
  if (!LoadBase)
    return Rip;
  if (Rip < *LoadBase || Rip - *LoadBase > InvalidVA - ImageBase)
    return invalid("captured RIP is outside the image mapping");
  return ImageBase + (Rip - *LoadBase);
}

llvm::Expected<X64RtmFaultCapture>
X64RtmFaultCapture::read(const llvm::json::Value &Value, va_t ImageBase,
                         unsigned AddressBits) {
  const auto *O = Value.getAsObject();
  if (!O)
    return invalid("expected a QEMU XBEGIN fault observation");
  const auto LoadBase = rtmCaptureWord<va_t>(*O, "load_base");
  const auto Before = readStop(O->getObject("before"), AddressBits);
  const auto After = readStop(O->getObject("after"), AddressBits);
  constexpr unsigned GateBytes = 16, ExceptionVectors = 32, UD = 6;
  constexpr unsigned FrameWords = 5, FrameBytes = FrameWords * sizeof(uint64_t);
  std::string Bytes, IDT, Frame;
  X64RtmFaultCapture Capture;
  if (!Before || !After || O->size() != 5 + unsigned(LoadBase.has_value()) ||
      !readBytes(*O, "bytes", Capture.Site.Bytes.size(), Bytes) ||
      !readBytes(*O, "exception_idt", ExceptionVectors * GateBytes, IDT) ||
      !readBytes(*O, "frame", FrameBytes, Frame))
    return invalid("invalid XBEGIN fault capture fields or x64 stop state");
  std::copy(Bytes.begin(), Bytes.end(), Capture.Site.Bytes.begin());
  const auto Insn =
      x86TransactionBeginEncoding(Capture.Site.Bytes, Before->Rip, true);
  if (!Insn || Insn->OpcodeOffset != 0 ||
      Before->Rip > InvalidVA - Bytes.size() ||
      !llvm::isIntN(AddressBits,
                    static_cast<int64_t>(Before->Rip + Bytes.size() - 1)) ||
      (After->Rip >= Before->Rip && After->Rip <= Before->Rip + Bytes.size()) ||
      After->Rip == Insn->Target)
    return invalid(
        "expected an isolated XBEGIN rel32 fault before either successor");
  const auto *Gate = IDT.data() + UD * GateBytes;
  using namespace llvm::support::endian;
  if (Gate[4] != 0 || uint8_t(Gate[5]) != 0x8e || read32le(Gate + 12) != 0 ||
      read16le(Gate + 2) != After->CS || gateTarget(Gate) != After->Rip)
    return invalid(
        "stopped RIP and CS must select the vector 6 interrupt gate");
  for (unsigned V = 0; V != ExceptionVectors; ++V) {
    const auto *Other = IDT.data() + V * GateBytes;
    if (V != UD && (Other[5] & 0x80) && gateTarget(Other) == After->Rip)
      return invalid(
          "vector 6 shares its landing address with another exception");
  }
  const uint64_t Top = llvm::alignDown(Before->Rsp, 16);
  if (Before->Cpu != After->Cpu || Before->Rax != After->Rax ||
      Before->Efer != After->Efer || Before->Cr4 != After->Cr4 ||
      Before->Flags != After->Flags || Before->SS != After->SS ||
      Top < FrameBytes || Top - FrameBytes != After->Rsp)
    return invalid(
        "inconsistent XBEGIN vCPU, register state or frame location");
  const std::array<uint64_t, FrameWords> Expected{Before->Rip, Before->CS,
                                                  Before->Flags | 0x10000,
                                                  Before->Rsp, Before->SS};
  for (unsigned I = 0; I != FrameWords; ++I)
    if (read64le(Frame.data() + I * sizeof(uint64_t)) != Expected[I])
      return invalid("XBEGIN #UD frame must save the original RIP, CS, RFLAGS "
                     "with RF, RSP and SS");
  auto PC = rtmCaptureImagePC(Before->Rip, LoadBase, ImageBase);
  if (!PC)
    return PC.takeError();
  if (*PC > InvalidVA - Bytes.size() ||
      !llvm::isIntN(AddressBits, static_cast<int64_t>(*PC)) ||
      !llvm::isIntN(AddressBits, static_cast<int64_t>(*PC + Bytes.size() - 1)))
    return invalid("captured XBEGIN is outside canonical image addresses");
  Capture.Site.Address = *PC;
  Capture.Receipt = llvm::json::Object{
      {"cpu", Before->Cpu},
      {"rip", "0x" + llvm::utohexstr(Before->Rip)},
      {"image_pc", "0x" + llvm::utohexstr(*PC)},
      {"bytes", llvm::toHex(Capture.Site.Bytes)},
      {"vector", UD},
      {"observed_handler", "0x" + llvm::utohexstr(After->Rip)},
      {"domain",
       "same observed vCPU RTM configuration, 64-bit ring zero, "
       "outside RTM/HLE, successful instruction fetch and V6 event RAM"}};
  if (LoadBase)
    Capture.Receipt["load_base"] = "0x" + llvm::utohexstr(*LoadBase);
  return Capture;
}

llvm::Expected<X64RtmFaultCapture>
X64RtmFaultCapture::bind(llvm::json::Object &Request,
                         llvm::StringRef ObservationJSON, va_t ImageBase,
                         unsigned AddressBits) {
  auto Saved = llvm::json::parse(ObservationJSON);
  if (!Saved)
    return Saved.takeError();
  auto *Envelope = Saved->getAsObject();
  auto *Fault = Envelope ? Envelope->get(Key) : nullptr;
  if (!Fault)
    return invalid("expected qemu_xbegin_fault_v1 in the observation file");
  auto Capture = read(*Fault, ImageBase, AddressBits);
  if (!Capture)
    return Capture.takeError();
  if (const auto *Existing = Request.get(Key)) {
    auto Inline = read(*Existing, ImageBase, AddressBits);
    if (!Inline)
      return Inline.takeError();
    // Both records are valid. Compare exact integers, including high unsigned
    // registers that LLVM's JSON value equality can round to the same double.
    if (llvm::formatv("{0}", *Existing).str() !=
        llvm::formatv("{0}", *Fault).str())
      return invalid("request already contains a different XBEGIN observation");
  } else {
    Request[Key] = std::move(*Fault);
  }
  return Capture;
}

} // namespace neverd
