//===- X86WatchpointObservation.h - Confined data comparisons ---*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Scope linear operand observation to one instruction or event attempt.
//===----------------------------------------------------------------------===//

#ifndef NEVERD_TRANSLATE_X86_X86WATCHPOINTOBSERVATION_H
#define NEVERD_TRANSLATE_X86_X86WATCHPOINTOBSERVATION_H

#include "neverd/ir/X86/X86DebugRules.h"
#include "neverd/translate/X86/X86PagedMemory.h"

#include <array>
#include <cstdint>

namespace neverd::translate {

template <typename Word> class X86WatchpointObservation final {
public:
  X86WatchpointObservation(
      X86PagedMemory &Memory, X86InstructionDebugState<Word> *Debug,
      X86InstructionDebugProfile Profile, uint8_t Initial = 0,
      const std::array<uint64_t, 4> *WideAddresses = nullptr)
      : Matches(Initial), Memory(Memory), Debug(Debug),
        Active(Debug && x86DebugWatchpoints(Profile)),
        WideAddresses(WideAddresses) {
    if (Active)
      Previous = Memory.exchangeAccessObserver({record, this});
  }
  X86WatchpointObservation(const X86WatchpointObservation &) = delete;
  X86WatchpointObservation &
  operator=(const X86WatchpointObservation &) = delete;
  ~X86WatchpointObservation() { stop(); }

  void stop() {
    if (Active) {
      Memory.exchangeAccessObserver(Previous);
      Active = false;
    }
  }
  void port(uint16_t Port, uint8_t Bytes) {
    if (Active)
      Matches |= x86DataBreakpoints(Debug->DR, Debug->DR7, Word(Port), Bytes,
                                    X86WatchpointAccess::Port);
  }
  uint8_t Matches;

private:
  static void record(void *Context, const X86LinearAccess &Access,
                     uint64_t Bytes) {
    auto &Self = *static_cast<X86WatchpointObservation *>(Context);
    const auto Kind = Access.Kind == MemoryAccessKind::Write
                          ? X86WatchpointAccess::Write
                          : X86WatchpointAccess::Read;
    if (Self.WideAddresses) {
      if (!Access.Wrap32) {
        Self.Matches |= x86DataBreakpoints(*Self.WideAddresses, Self.Debug->DR7,
                                           Access.Address, Bytes, Kind);
        return;
      }
      auto Enables = Self.Debug->DR7;
      for (unsigned I = 0; I != 4; ++I)
        if ((*Self.WideAddresses)[I] >> 32)
          Enables &= ~(uint64_t{3} << (I * 2));
      Self.Matches |= x86DataBreakpoints(Self.Debug->DR, Enables,
                                         Word(Access.Address), Bytes, Kind);
      return;
    }
    Self.Matches |= x86DataBreakpoints(
        Self.Debug->DR, Self.Debug->DR7, Word(Access.Address), Bytes,
        Access.Kind == MemoryAccessKind::Write ? X86WatchpointAccess::Write
                                               : X86WatchpointAccess::Read);
  }
  X86PagedMemory &Memory;
  X86InstructionDebugState<Word> *Debug;
  bool Active;
  const std::array<uint64_t, 4> *WideAddresses;
  X86PagedMemory::AccessObserver Previous;
};

} // namespace neverd::translate

#endif // NEVERD_TRANSLATE_X86_X86WATCHPOINTOBSERVATION_H
