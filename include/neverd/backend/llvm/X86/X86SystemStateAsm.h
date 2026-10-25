//===- X86SystemStateAsm.h - Owned system assembly contracts -*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Assembly operands shared by LLVM and both C routes. Architectural memory
/// operations stay inside the instruction; every event remains observable.
//===----------------------------------------------------------------------===//

#ifndef NEVERD_BACKEND_LLVM_X86_X86SYSTEMSTATEASM_H
#define NEVERD_BACKEND_LLVM_X86_X86SYSTEMSTATEASM_H

#include "neverd/ir/intrinsics/X86/X86SystemState.h"

#include <optional>
#include <string>
#include <vector>

namespace neverd {

struct X86SystemStateAsm {
  Intrinsic Id = Intrinsic::None;
  std::string Text;
  std::string Constraints;
  std::vector<std::string> OutputConstraints;
  std::vector<std::string> InputConstraints;
  std::vector<uint16_t> OutputBytes;
  std::vector<uint16_t> InputBytes;
  bool ChangesFlags = false;
};

inline std::optional<X86SystemStateAsm>
x86SystemStateAsm(Intrinsic Id, Arch TargetArch, uint16_t Width,
                  NdMemoryAddressSpace Space,
                  std::optional<uint16_t> Selector = std::nullopt) {
  const auto C = x86SystemStateContract(Id);
  const auto L = x86SystemStateLayout(Id, TargetArch, Width, Selector);
  if (!L || (TargetArch != Arch::X86 && TargetArch != Arch::X64) ||
      !isKnownMemoryAddressSpace(Space) ||
      ((C.MemoryArgument < 0 || C.Kind == X86SystemStateKind::MsrList) &&
       Space != NdMemoryAddressSpace::Default))
    return std::nullopt;
  X86SystemStateAsm P;
  P.Id = Id;
  P.Text = C.Mnemonic;
  P.ChangesFlags = L->HasFlag;
  if (L->ValueSize) {
    P.OutputConstraints.push_back("=&r");
    P.OutputBytes.push_back(L->ValueSize);
  }
  if (L->HasFlag) {
    P.OutputConstraints.push_back("=q");
    P.OutputBytes.push_back(1);
  }
  for (uint8_t I = 0; I < L->Arguments; ++I) {
    P.InputConstraints.push_back("r");
    P.InputBytes.push_back(isX86SystemStateAddressArgument(Id, I)
                               ? (TargetArch == Arch::X64 ? 8 : 4)
                               : L->InputSizes[I]);
  }
  const std::string Segment = Space == NdMemoryAddressSpace::X86FS   ? "%fs:"
                              : Space == NdMemoryAddressSpace::X86GS ? "%gs:"
                                                                     : "";
  auto Input = [&](unsigned I) {
    return "$" + std::to_string(P.OutputBytes.size() + I);
  };
  auto Memory = [&](unsigned I) { return Segment + "(" + Input(I) + ")"; };
  using K = X86SystemStateKind;
  switch (C.Kind) {
  case K::SegmentedBlock: {
    const char *Mnemonics[] = {"movdir64b", "enqcmd", "enqcmds"};
    const char *Segments[] = {"%es:", "%cs:", "%ss:", "%ds:", "%fs:", "%gs:"};
    const auto Value = *Selector;
    const auto Modifier = Value & X86SegmentedBlockAddress16 ? ":w}" : ":k}";
    auto Offset = [&](unsigned I) {
      return "${" + std::to_string(P.OutputBytes.size() + I) + Modifier;
    };
    P.Text += "\n\t" + std::string(Mnemonics[Value & 3]) + " " +
              Segments[(Value >> 2) & 7] + "(" + Offset(0) + "), " + Offset(1);
    if (L->HasFlag)
      P.Text += "\n\tsetz $0";
    P.InputBytes = {4, 4};
    // SI is a legal base for both addr16 and addr32. Explicitly name the
    // source segment, including default SS, independently of allocation.
    P.InputConstraints = {"{si}", "{di}"};
    break;
  }
  case K::ReadPid:
  case K::ReadBase:
    P.Text += " $0";
    break;
  case K::WriteBase:
    P.Text += " " + Input(0);
    break;
  case K::Void:
  case K::Void64:
    break;
  case K::TransactionAbort:
    P.Text += " " + Input(0);
    P.InputConstraints = {"i"};
    break;
  case K::TestUserInterrupt:
    P.Text += "\n\tsetc $0";
    break;
  case K::SendUserInterrupt:
    P.Text += " " + Input(0);
    break;
  case K::LoadKernelGs:
    P.Text += " " + (C.MemoryArgument == 0 ? Memory(0) : Input(0));
    break;
  case K::HistoryReset:
    // The encoded imm8 is ignored, including nonzero values. Keep EAX live.
    P.Text += " $$0";
    P.InputConstraints = {"{ax}"};
    break;
  case K::InvalidateBroadcast:
    P.InputConstraints = {"{ax}", "{dx}", "{cx}"};
    // LLVM's mnemonic chooses the default address size. Preserve addr32
    // explicitly even if the value could fit in a 64-bit operand.
    if (TargetArch == Arch::X64 && Width == 4)
      P.Text = "addr32 " + P.Text;
    break;
  case K::Monitor:
    if (!Segment.empty())
      P.Text = (Space == NdMemoryAddressSpace::X86FS ? "fs " : "gs ") + P.Text;
    P.Text += " " + Input(0);
    break;
  case K::CacheZero:
    // rAX is implicit, including with an FS/GS override. The lifter wraps the
    // effective offset before extending it to the canonical address carrier.
    P.InputConstraints = {"{ax}"};
    if (!Segment.empty())
      P.Text = (Space == NdMemoryAddressSpace::X86FS ? "fs " : "gs ") + P.Text;
    break;
  case K::Wait:
    P.InputConstraints = {"r", "{ax}", "{dx}"};
    P.Text += " " + Input(0) + "\n\tsetc $0";
    break;
  case K::AccessRights:
    P.Text += " " + (C.MemoryArgument == 0 ? Memory(0) : Input(0)) +
              ", $0\n\tsetz $1";
    break;
  case K::AdjustRpl:
    if (C.MemoryArgument == 0)
      P.Text += " " + Input(1) + ", " + Memory(0) + "\n\tsetz $0";
    else {
      P.InputConstraints[0] = "0";
      P.Text += " " + Input(1) + ", $0\n\tsetz $1";
    }
    break;
  case K::ShadowWrite:
    P.Text += Width == 8 ? "q " : "d ";
    P.Text += Input(1) + ", " + Memory(0);
    break;
  case K::Enqueue:
    P.Text += " " + Memory(0) + ", " + Input(1) + "\n\tsetz $0";
    break;
  case K::DirectStore:
    P.Text += " " + Input(1) + ", " + Memory(0);
    break;
  case K::DirectStore64B:
    P.Text += " " + Memory(0) + ", " + Input(1);
    break;
  case K::WriteMsr:
    P.InputConstraints = {"{cx}", "{ax}", "{dx}"};
    break;
  case K::MsrList:
    P.OutputConstraints = {"={cx}"};
    P.InputConstraints = {"{si}", "{di}", "0"};
    break;
  }
  auto Constraint = [&](const std::string &Text) {
    if (!P.Constraints.empty())
      P.Constraints += ',';
    P.Constraints += Text;
  };
  for (const auto &O : P.OutputConstraints)
    Constraint(O);
  for (const auto &I : P.InputConstraints)
    Constraint(I);
  if (P.ChangesFlags)
    Constraint("~{flags}");
  Constraint("~{memory}");
  return P;
}

/// Explicit descriptor offsets carry their own address width and segment.
/// Shared by native-call C projection and the assembly fallback.
inline std::optional<X86SystemStateAsm>
x86SystemStateAddressAsm(Intrinsic Id, Arch TargetArch, uint16_t Width,
                         unsigned AddressBits, unsigned SegmentPrefix) {
  auto Plan =
      x86SystemStateAsm(Id, TargetArch, Width, NdMemoryAddressSpace::Default);
  const auto C = x86SystemStateContract(Id);
  const bool Query = C.Kind == X86SystemStateKind::AccessRights;
  const unsigned WordBits = TargetArch == Arch::X64 ? 64 : 32;
  if (!Plan || C.MemoryArgument != 0 ||
      (!Query && Id != Intrinsic::X86LoadKernelGsMemory) ||
      (AddressBits != WordBits && AddressBits != WordBits / 2))
    return std::nullopt;
  const char *Segment = nullptr;
  switch (SegmentPrefix) {
  case 0:
    Segment = "";
    break;
  case 0x26:
    Segment = "%es:";
    break;
  case 0x2e:
    Segment = "%cs:";
    break;
  case 0x36:
    Segment = "%ss:";
    break;
  case 0x3e:
    Segment = "%ds:";
    break;
  case 0x64:
    Segment = "%fs:";
    break;
  case 0x65:
    Segment = "%gs:";
    break;
  default:
    return std::nullopt;
  }
  // A fixed BX base is legal in both i386 address sizes and retains DS
  // when no override is present. Narrow modifiers retain the actual address
  // prefix in the fallback instead of relying on a numeric pointer cast.
  auto &A = *Plan;
  const auto Offset = std::to_string(A.OutputBytes.size());
  const auto Base = AddressBits == WordBits ? "$" + Offset
                    : AddressBits == 16     ? "${" + Offset + ":w}"
                                            : "${" + Offset + ":k}";
  A.Text = std::string(C.Mnemonic) + " " + Segment + "(" + Base + ")";
  if (Query)
    A.Text += ", $0\n\tsetz $1";
  A.InputConstraints = {"{bx}"};
  A.Constraints.clear();
  for (const auto &Constraint : A.OutputConstraints)
    A.Constraints += Constraint + ",";
  A.Constraints += "{bx},";
  if (A.ChangesFlags)
    A.Constraints += "~{flags},";
  A.Constraints += "~{memory}";
  return Plan;
}

/// Fixed RAX data and RCX selector operands keep the documented ModRM roles.
/// LLVM 23's URDMSR register assembler reverses those roles. Exact bytes also
/// retain APX feature checks when an EVEX operand is allocated a low register.
inline std::optional<X86SystemStateAsm> x86MsrAccessAsm(X86MsrAccessKind Kind,
                                                        Arch TargetArch) {
  if (TargetArch != Arch::X64 || Kind >= X86MsrAccessKind::Count)
    return std::nullopt;
  using K = X86MsrAccessKind;
  const bool Write = x86MsrAccessIsWrite(Kind);
  const bool Immediate = x86MsrAccessHasImmediateSelector(Kind);
  const bool Privileged =
      Kind == K::RdmsrImmediate || Kind == K::WrmsrnsImmediate ||
      Kind == K::RdmsrVexImmediate || Kind == K::WrmsrnsVexImmediate;
  const bool Evex =
      Kind == K::RdmsrImmediate || Kind == K::WrmsrnsImmediate ||
      Kind == K::UrdmsrEvexRegister || Kind == K::UwrmsrEvexRegister ||
      Kind == K::UrdmsrEvexImmediate || Kind == K::UwrmsrEvexImmediate;
  X86SystemStateAsm P;
  P.Id = Intrinsic::X86MsrAccess;
  P.Text = "# x86 MSR: ";
  P.Text += Privileged ? (Write ? "wrmsrns" : "rdmsr")
                       : (Write ? "uwrmsr" : "urdmsr");
  P.Text += Evex ? " (EVEX)" : Immediate ? " (VEX)" : " (legacy)";
  P.Text += "\n\t.byte ";
  if (Evex) {
    P.Text += Immediate ? "0x62,0xf7," : "0x62,0xf4,";
    P.Text += Write ? "0x7e,0x08," : "0x7f,0x08,";
  } else if (Immediate) {
    P.Text += Write ? "0xc4,0xe7,0x7a," : "0xc4,0xe7,0x7b,";
  } else {
    P.Text += Write ? "0xf3,0x0f,0x38," : "0xf2,0x0f,0x38,";
  }
  P.Text += Privileged ? "0xf6," : "0xf8,";
  P.Text += Immediate ? "0xc0" : "0xc8";
  if (Immediate)
    P.Text += Write ? "\n\t.long ${0:c}" : "\n\t.long ${1:c}";
  P.InputConstraints = {Immediate ? "i" : "{cx}"};
  P.InputBytes = {static_cast<uint16_t>(Immediate ? 4 : 8)};
  if (Write) {
    P.InputConstraints.push_back("{ax}");
    P.InputBytes.push_back(8);
    P.Constraints = Immediate ? "i,{ax},~{memory}" : "{cx},{ax},~{memory}";
  } else {
    P.OutputConstraints = {"={ax}"};
    P.OutputBytes = {8};
    P.Constraints = Immediate ? "={ax},i,~{memory}" : "={ax},{cx},~{memory}";
  }
  return P;
}

} // namespace neverd

#endif // NEVERD_BACKEND_LLVM_X86_X86SYSTEMSTATEASM_H
