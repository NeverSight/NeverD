//===- CheckedX64Memory.cpp - Precise device and string transactions ------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "../../core/ExecutionDiagnostics.h"
#include "CheckedX64Backend.h"

#include "neverd/support/X86Addressing.h"

#include "llvm/ADT/ScopeExit.h"
#include "llvm/Support/ErrorHandling.h"
#include "llvm/Support/MathExtras.h"

#include <climits>

namespace neverd::emulation {
llvm::Expected<uint64_t>
CheckedX64Backend::operandAddress(const cs_insn &I, const cs_x86_op &O,
                                  uint64_t Offset) const {
  const auto &X = I.detail->x86;
  if (O.type != X86_OP_MEM ||
      (X.addr_size != x64::DWordBytes && X.addr_size != x64::WordBytes) ||
      (O.mem.segment != X86_REG_INVALID && O.mem.segment != X86_REG_DS &&
       O.mem.segment != X86_REG_SS && O.mem.segment != X86_REG_ES &&
       O.mem.segment != X86_REG_GS && O.mem.segment != X86_REG_FS))
    return llvm::make_error<UnsupportedExecutionError>();
  auto Base = operandRegister(O.mem.base);
  auto Index = isNoSibIndex(O.mem.index, X.addr_size)
                   ? llvm::Expected<uint64_t>(0)
                   : operandRegister(O.mem.index);
  if (!Base) {
    if (!Index)
      llvm::consumeError(Index.takeError());
    return Base.takeError();
  }
  if (!Index)
    return Index.takeError();
  uint64_t Address = *Base + *Index * O.mem.scale + O.mem.disp + Offset;
  if (O.mem.base == X86_REG_RIP || O.mem.base == X86_REG_EIP)
    Address += I.size;
  if (X.addr_size == x64::DWordBytes)
    Address = uint32_t(Address);
  if (O.mem.segment == X86_REG_GS)
    Address += CPU.GSBase;
  if (O.mem.segment == X86_REG_FS)
    Address += CPU.FSBase;
  return Address;
}
std::shared_ptr<MemoryProjection::Device>
CheckedX64Backend::deviceAt(uint64_t Address) const {
  auto I = Memory->mappings().find(Address & ~(x64::PageSize - 1));
  return I == Memory->mappings().end() ? nullptr : I->second.IO;
}
llvm::Error CheckedX64Backend::deviceResult(llvm::Error E) {
  if (E)
    DeviceFailed = true;
  return E;
}
llvm::Error CheckedX64Backend::validateDevice(const MemoryProjection::Device &D,
                                              uint64_t Address, unsigned Size,
                                              bool Write) {
  if ((Size != x64::ByteBytes && Size != x64::HalfWordBytes &&
       Size != x64::DWordBytes) ||
      Address % Size || Address < D.Address || Address - D.Address >= D.Size ||
      Size > D.Size - (Address - D.Address))
    return deviceResult(diagnostic::error(diagnostic::DeviceWidth));
  return deviceResult(deviceCallback(
      [&] { return D.Callbacks.Validate(Address - D.Address, Size, Write); }));
}
void CheckedX64Backend::setOperandRegister(unsigned R, uint64_t Value) {
  switch (R) {
#define NEVERD_X64_OPERAND_REGISTER(ID, Name, Shift, Bits)                     \
  case X86_REG_##ID: {                                                         \
    constexpr uint64_t Mask = UINT64_MAX >> (x64::WordBits - Bits);            \
    auto &Register = CPU.reg(X64Register::Name);                               \
    Register = ((Bits == x64::DWordBytes * CHAR_BIT ? 0 : Register) &          \
                ~(Mask << Shift)) |                                            \
               ((Value & Mask) << Shift);                                      \
    return;                                                                    \
  }
#include "X64OperandRegisters.def"
#undef NEVERD_X64_OPERAND_REGISTER
  default:
    llvm_unreachable(diagnostic::Register);
  }
}
llvm::Error CheckedX64Backend::deviceTransfer(const cs_insn &I,
                                              uint64_t Address, unsigned Size,
                                              unsigned Permission,
                                              uint64_t Value) {
  if (auto E = Memory->prepareWrite())
    return E;
  auto D = deviceAt(Address);
  if (auto E = validateDevice(*D, Address, Size, Permission == Write))
    return E;
  if (StopRequested || FirstFault)
    return llvm::Error::success();
  const uint64_t Mask = UINT64_MAX >> (x64::WordBits - Size * CHAR_BIT);
  if (Permission == Write) {
    if (auto E = deviceResult(deviceCallback([&] {
          return D->Callbacks.Write(Address - D->Address, Size, Value & Mask);
        })))
      return E;
  } else {
    auto Result = deviceCallback(
        [&] { return D->Callbacks.Read(Address - D->Address, Size); });
    if (!Result)
      return deviceResult(Result.takeError());
    Value = *Result & Mask;
    if (I.id == X86_INS_MOVSX && (Value & ((Mask >> 1) + 1)))
      Value |= ~Mask;
    setOperandRegister(I.detail->x86.operands[0].reg, Value);
  }
  CPU.reg(X64Register::PC) = I.address + I.size;
  return llvm::Error::success();
}

llvm::Error CheckedX64Backend::executeString(const cs_insn &I, unsigned Size,
                                             StringOperation Operation) {
  const auto &X = I.detail->x86;
  const bool Compare = Operation == StringOperation::Compare ||
                       Operation == StringOperation::Scan;
  const bool RepeatEqual = X.prefix[0] == X86_PREFIX_REP;
  const bool Repeat =
      RepeatEqual || (Compare && X.prefix[0] == X86_PREFIX_REPNE);
  if ((X.prefix[0] && !Repeat) ||
      (X.addr_size != x64::DWordBytes && X.addr_size != x64::WordBytes))
    return llvm::make_error<UnsupportedExecutionError>();
  const bool Reads = Operation == StringOperation::Move ||
                     Operation == StringOperation::Load ||
                     Operation == StringOperation::Compare;
  const bool Writes =
      Operation == StringOperation::Move || Operation == StringOperation::Store;
  const bool UsesDestination = Writes || Compare;
  const bool Narrow = X.addr_size == x64::DWordBytes;
  auto Address = [&](uint64_t V) { return Narrow ? uint64_t(uint32_t(V)) : V; };
  uint64_t Count = Address(CPU.reg(X64Register::CX));
  if (Repeat && !Count) {
    // With address-size 32, zero-count REP high halves vary across x64
    // implementations, even between cores of one hybrid CPU. Until the
    // contract exposes a CPU model, admit only unambiguous inactive state.
    if (Narrow &&
        (CPU.reg(X64Register::CX) != Count ||
         (UsesDestination &&
          CPU.reg(X64Register::DI) != Address(CPU.reg(X64Register::DI))) ||
         (UsesDestination && Reads &&
          CPU.reg(X64Register::SI) != Address(CPU.reg(X64Register::SI)))))
      return llvm::make_error<UnsupportedExecutionError>();
    CPU.reg(X64Register::PC) = I.address + I.size;
    StringRestart.reset();
    return llvm::Error::success();
  }
  if (Writes)
    if (auto E = Memory->prepareWrite())
      return E;
  if (Compare && Repeat && !StringRestart)
    StringRestart = StringRestartState{I.address, CPU.reg(X64Register::FLAGS)};
  auto CheckAccess = [&](uint64_t A, unsigned Permission) {
    // Permission validation has no callbacks unless it raises a guest fault.
    // Both terminal and recoverable fault observers must see the flags from
    // before this REP, while retaining every completed pointer/count update.
    const uint64_t Flags = CPU.reg(X64Register::FLAGS);
    if (Compare && Repeat)
      CPU.reg(X64Register::FLAGS) = StringRestart->Flags;
    auto Restore = llvm::scope_exit([&] {
      if (!FirstFault && !RecoverableFault)
        CPU.reg(X64Register::FLAGS) = Flags;
    });
    return access(A, Size, Permission, true, true);
  };
  const uint64_t SourceOffset = Address(CPU.reg(X64Register::SI));
  const uint64_t Source =
      SourceOffset + (X.prefix[1] == X86_PREFIX_FS   ? CPU.FSBase
                      : X.prefix[1] == X86_PREFIX_GS ? CPU.GSBase
                                                     : 0);
  const uint64_t Destination = Address(CPU.reg(X64Register::DI));
  struct StringAccess {
    uint64_t Address;
    unsigned Permission;
    bool Used;
  };
  const StringAccess Accesses[] = {
      {Source, Read, Reads},
      {Destination, Writes ? Write : Read, UsesDestination}};
  for (const auto &[A, Permission, Used] : Accesses) {
    if (!Used)
      continue;
    if (Size - 1 > UINT64_MAX - A)
      return CheckAccess(A, Permission);
    const uint64_t Last = A + Size - 1;
    if (A / x64::PageSize != Last / x64::PageSize &&
        (deviceAt(A) || deviceAt(Last)))
      return llvm::make_error<UnsupportedExecutionError>();
  }
  auto Input = Reads ? deviceAt(Source) : nullptr;
  auto Output = UsesDestination ? deviceAt(Destination) : nullptr;
  if (Input || Output)
    if (auto E = Memory->prepareWrite())
      return E;
  if (Operation != StringOperation::Move && (Input || Output))
    return llvm::make_error<UnsupportedExecutionError>();
  if (Input && !Input->Callbacks.PrepareRead)
    return llvm::make_error<UnsupportedExecutionError>();
  // All permission and pure device validation precedes preparation, observer
  // callbacks and device effects. Device mappings are supervisor-only.
  for (const auto &[A, Permission, Used] : Accesses) {
    if (!Used)
      continue;
    if (auto E = CheckAccess(A, Permission))
      return E;
    if (StopRequested)
      return llvm::Error::success();
  }
  if (Input)
    if (auto E = validateDevice(*Input, Source, Size, false))
      return E;
  if (StopRequested)
    return llvm::Error::success();
  if (Output)
    if (auto E = validateDevice(*Output, Destination, Size, true))
      return E;
  if (StopRequested)
    return llvm::Error::success();
  if (Reads && Hooks.Read)
    Hooks.Read(Source, Size);
  if (StopRequested || FirstFault)
    return llvm::Error::success();
  std::optional<GuestMMIOPreparedRead> Prepared;
  uint64_t Value = CPU.reg(X64Register::AX);
  if (Input) {
    auto P = deviceCallback([&] {
      return Input->Callbacks.PrepareRead(Source - Input->Address, Size);
    });
    if (!P)
      return deviceResult(P.takeError());
    if (!P->Commit)
      return deviceResult(diagnostic::error(diagnostic::DevicePreparedRead));
    Prepared = std::move(*P);
    Value = Prepared->Value;
  } else if (Reads) {
    auto R = readInteger(Source, Size);
    if (!R)
      return R.takeError();
    Value = *R;
  }
  const uint64_t Mask = UINT64_MAX >> (x64::WordBits - Size * CHAR_BIT);
  Value &= Mask;
  uint64_t NextFlags = CPU.reg(X64Register::FLAGS);
  if (StopRequested || FirstFault)
    return llvm::Error::success();
  if (Compare) {
    if (Hooks.Read)
      Hooks.Read(Destination, Size);
    if (StopRequested || FirstFault)
      return llvm::Error::success();
    auto Right = readInteger(Destination, Size);
    if (!Right)
      return Right.takeError();
    const uint64_t Result = (Value - *Right) & Mask;
    const uint64_t SignBit = (Mask >> 1) + 1;
    NextFlags =
        (NextFlags & ~x64::ArithmeticFlags) |
        (Value < *Right ? x64::CarryFlag : 0) |
        (!(llvm::popcount(uint8_t(Result)) & 1) ? x64::ParityFlag : 0) |
        ((Value ^ *Right ^ Result) & x64::AuxiliaryFlag) |
        (!Result ? x64::ZeroFlag : 0) | (Result & SignBit ? x64::SignFlag : 0) |
        ((Value ^ *Right) & (Value ^ Result) & SignBit ? x64::OverflowFlag : 0);
  }
  if (StopRequested || FirstFault)
    return llvm::Error::success();
  if (Writes && Hooks.Write)
    Hooks.Write(Destination, Size, Value);
  if (StopRequested || FirstFault)
    return llvm::Error::success();
  if (Prepared)
    if (auto E =
            deviceResult(deviceCallback([&] { return Prepared->Commit(); })))
      return E;
  if (Output) {
    if (auto E = deviceResult(deviceCallback([&] {
          return Output->Callbacks.Write(Destination - Output->Address, Size,
                                         Value);
        })))
      return E;
  } else if (Writes) {
    // Admission and the physical execution lease retain this complete RAM
    // slice. Public host writes remain forbidden while a CPU is running.
    for (unsigned N = 0; N < Size; ++N) {
      const uint64_t Address = Destination + N;
      const auto &P = Memory->mappings().at(Address & ~(x64::PageSize - 1));
      Memory->recordRAMWrite(P.Physical + Address % x64::PageSize,
                             sizeof(uint8_t));
      *Memory->physicalPointer(P.Physical + Address % x64::PageSize) =
          uint8_t(Value >> (N * CHAR_BIT));
    }
  } else if (Operation == StringOperation::Load) {
    // AL/AX preserve the untouched accumulator bits. EAX zero-extends.
    CPU.reg(X64Register::AX) =
        Value |
        (Size == x64::DWordBytes ? 0 : CPU.reg(X64Register::AX) & ~Mask);
  }
  const uint64_t Delta = CPU.reg(X64Register::FLAGS) & x64::DirectionFlag
                             ? uint64_t(0) - Size
                             : Size;
  if (Reads)
    CPU.reg(X64Register::SI) = Address(SourceOffset + Delta);
  if (UsesDestination)
    CPU.reg(X64Register::DI) = Address(Destination + Delta);
  if (Repeat)
    CPU.reg(X64Register::CX) = --Count;
  CPU.reg(X64Register::FLAGS) = NextFlags;
  // A REP iteration is an architectural restart boundary. The shared runner
  // rechecks stops, deadlines and budgets before observing the next element.
  const bool Continue =
      Repeat && Count &&
      (!Compare || bool(NextFlags & x64::ZeroFlag) == RepeatEqual);
  CPU.reg(X64Register::PC) = Continue ? I.address : I.address + I.size;
  if (!Continue)
    StringRestart.reset();
  return llvm::Error::success();
}
} // namespace neverd::emulation
