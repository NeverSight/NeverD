//===- DriverSession.cpp - Bounded WDM driver execution -------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Bounded WDM driver execution.
///
//===----------------------------------------------------------------------===//

#include "neverd/emulation/DriverSession.h"

#include "DriverScenario.h"
#include "X64ExecutionPolicy.h"
#include "unicorn/UnicornBackend.h"
#include "windows/DriverImage.h"
#include "windows/GuardControlFlow.h"
#include "windows/KernelExportRegistry.h"
#include "windows/KernelModel.h"

#include "llvm/ADT/StringExtras.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <map>
#include <memory>

namespace neverd::emulation {
namespace {
using namespace profile;
constexpr uint64_t ReturnSentinel = ThunkBase + ThunkSize - ThunkStride;

llvm::Error failure(const std::string &Text) {
  return llvm::createStringError(llvm::inconvertibleErrorCode(), Text);
}

std::string guestCallPhase(const GuestCallToken &Token) {
  return std::string(Token.Owner == GuestCallOwner::Framework
                         ? "callback:framework"
                         : "callback:wdm") +
         std::to_string(Token.ID);
}

} // namespace

llvm::Expected<DriverResult> emulateDriver(const std::filesystem::path &Path,
                                           const DriverOptions &Options) {
  if (!Options.InstructionLimit || !Options.MemoryLimit ||
      !Options.EventLimit || !Options.TimeoutMilliseconds ||
      Options.TimeoutMilliseconds > MaxTimeoutMilliseconds ||
      Options.MemoryLimit > MaxMemoryLimit ||
      Options.EventLimit > MaxEventLimit)
    return failure("driver limits must be positive (memory <= 1 GiB, events <= "
                   "1000000, timeout <= 3600000 ms)");
  if (Options.ServiceName.empty() ||
      Options.ServiceName.size() > MaxServiceNameSize ||
      !std::all_of(Options.ServiceName.begin(), Options.ServiceName.end(),
                   [](unsigned char C) {
                     return (C >= 'a' && C <= 'z') || (C >= 'A' && C <= 'Z') ||
                            (C >= '0' && C <= '9') || C == '_' || C == '-';
                   }))
    return failure("service name must contain 1..128 ASCII letters, digits, "
                   "underscores or hyphens");
  if (auto E = validateDriverScenario(Options))
    return std::move(E);
  KernelExportRegistry Exports;
  if (auto E = Exports.initialize(Options))
    return std::move(E);
  auto Image = loadDriverImage(Path, Options.MemoryLimit, Options.LoadAddress);
  if (!Image)
    return Image.takeError();
  DriverResult Result;
  Result.Configuration = Options;
  Result.ImageBase = Image->Base;
  Result.PreferredImageBase = Image->PreferredBase;
  Result.SecurityCookieAddress = Image->SecurityCookieAddress;
  Result.Entry = Image->Entry;
  Result.PC = Image->Entry;
  if (Image->Imports.size() > (ThunkSize / ThunkStride) - 1)
    return failure("too many driver imports for the bounded x64 profile");
  auto Backend = UnicornBackend::create(Options.MemoryLimit);
  if (!Backend)
    return Backend.takeError();
  auto &CPU = **Backend;
  GuardControlFlow Guard(*Image);
  // Temporary writable image pages are private setup state. Final permissions
  // are applied before any guest instruction can run.
  for (const auto &Region : Image->Regions) {
    if (auto E = CPU.map(Region.Address, Region.Bytes.size(), Read | Write))
      return std::move(E);
    if (auto E = CPU.write(Region.Address, Region.Bytes))
      return std::move(E);
  }
  for (const auto &Import : Image->Imports) {
    auto Address = Exports.bindImport(Import);
    if (!Address)
      return Address.takeError();
    if (auto E = CPU.writeInteger(Import.Slot, *Address, PointerSize))
      return std::move(E);
  }
  if (Image->Guard.Enabled) {
    if (auto E = CPU.writeInteger(Image->Guard.CheckPointerAddress,
                                  GuardCheckThunk, PointerSize))
      return std::move(E);
    if (Image->Guard.DispatchPointerAddress)
      if (auto E = CPU.writeInteger(Image->Guard.DispatchPointerAddress,
                                    GuardDispatchThunk, PointerSize))
        return std::move(E);
    if (auto E = CPU.map(GuardThunkBase, PageSize, Read | Execute))
      return std::move(E);
  }
  for (const auto &Region : Image->Regions)
    if (auto E = CPU.protect(Region.Address, Region.Bytes.size(),
                             Region.Permissions))
      return std::move(E);
  if (auto E = CPU.map(ThunkBase, ThunkSize, Read | Write))
    return std::move(E);
  std::vector<uint8_t> TrapBytes(ThunkSize, ThunkTrapByte);
  if (auto E = CPU.write(ThunkBase, TrapBytes))
    return std::move(E);
  if (auto E = CPU.protect(ThunkBase, ThunkSize, Read | Execute))
    return std::move(E);
  if (auto E = CPU.map(StackBase, StackSize, Read | Write))
    return std::move(E);
  KernelModel Kernel(CPU, Result, &Exports);
  if (auto E = Kernel.initialize(*Image, Options))
    return std::move(E);
  uint64_t ExpectedReturnSP = 0;
  uint64_t ActiveStackBase = 0;
  uint64_t ActiveStackSize = 0;
  X64ExecutionPolicy Policy;
  if (auto E = Policy.initialize())
    return std::move(E);
  bool Stopped = false;
  std::optional<uint64_t> InvocationReturn;
  const KernelExportRegistry::Export *Pending = nullptr;
  uint64_t PendingGuard = 0;
  auto Stop = [&](DriverStopReason Reason, const std::string &Diagnostic) {
    if (Stopped)
      return;
    Result.Stop = Reason;
    Result.Diagnostic = Diagnostic;
    Stopped = true;
    CPU.stop();
  };
  auto EventAvailable = [&]() {
    if (Result.Calls.size() + Result.Writes.size() >= Options.EventLimit) {
      Stop(DriverStopReason::EventLimit, "behavior event limit reached");
      return false;
    }
    return true;
  };
  BackendHooks Hooks;
  Hooks.Instruction = [&](uint64_t Address, uint32_t Size) {
    if (Stopped)
      return;
    Result.PC = Address;
    auto StackPointer = CPU.reg(X64Register::SP);
    if (!StackPointer) {
      Stop(DriverStopReason::EngineError,
           llvm::toString(StackPointer.takeError()));
      return;
    }
    if (*StackPointer < ActiveStackBase ||
        *StackPointer >= ActiveStackBase + ActiveStackSize) {
      Stop(DriverStopReason::ModelError,
           "guest stack pointer exceeds the invocation stack");
      return;
    }
    if (Address == ReturnSentinel) {
      auto SP = CPU.reg(X64Register::SP);
      auto AX = CPU.reg(X64Register::AX);
      if (!SP || !AX) {
        std::string Error;
        if (!SP)
          Error += llvm::toString(SP.takeError());
        if (!AX)
          Error += llvm::toString(AX.takeError());
        Stop(DriverStopReason::EngineError, Error);
      } else if (*SP != ExpectedReturnSP) {
        Stop(DriverStopReason::ModelError,
             "driver callback returned with an unbalanced stack");
      } else {
        InvocationReturn = *AX;
        Stop(DriverStopReason::Returned, "");
      }
      return;
    }
    if (Image->Guard.Enabled &&
        (Address == GuardCheckThunk || Address == GuardDispatchThunk)) {
      PendingGuard = Address;
      CPU.stop();
      return;
    }
    if (const auto *Export = Exports.lookup(Address)) {
      if (!KernelModel::argumentCount(*Export)) {
        Stop(DriverStopReason::UnsupportedAPI,
             "unsupported import: " + Export->Module + "!" + Export->Name);
        return;
      }
      Pending = Export;
      CPU.stop();
      return;
    }
    if (Result.Instructions >= Options.InstructionLimit) {
      Stop(DriverStopReason::InstructionLimit,
           "guest instruction limit reached");
      return;
    }
    if (!Size || Size > MaxInstructionSize) {
      Stop(DriverStopReason::UnsupportedInstruction,
           "invalid x64 instruction extent");
      return;
    }
    if (auto E = Kernel.validateGuestAccess(Address, Size, false)) {
      Stop(DriverStopReason::ModelError, llvm::toString(std::move(E)));
      return;
    }
    std::array<uint8_t, MaxInstructionSize> Bytes{};
    if (auto E = CPU.fetch(
            Address, llvm::MutableArrayRef<uint8_t>(Bytes.data(), Size))) {
      Stop(DriverStopReason::MemoryFault, llvm::toString(std::move(E)));
      return;
    }
    if (auto E = Policy.validate(llvm::ArrayRef<uint8_t>(Bytes.data(), Size),
                                 Address)) {
      Stop(DriverStopReason::UnsupportedInstruction,
           llvm::toString(std::move(E)));
      return;
    }
    ++Result.Instructions;
  };
  Hooks.Read = [&](uint64_t Address, uint32_t Size) {
    if (Stopped)
      return;
    if (Address < ThunkBase + ThunkSize &&
        (Address >= ThunkBase || Size > ThunkBase - Address)) {
      Stop(DriverStopReason::UnsupportedAPI,
           "reading bytes of an imported symbol requires an unmodeled kernel "
           "image");
      return;
    }
    if (auto E = Kernel.validateGuestAccess(Address, Size, false))
      Stop(DriverStopReason::ModelError, llvm::toString(std::move(E)));
  };
  Hooks.Write = [&](uint64_t Address, uint32_t Size, uint64_t Value) {
    if (Stopped)
      return;
    if (auto E = Kernel.validateGuestAccess(Address, Size, true)) {
      Stop(DriverStopReason::ModelError, llvm::toString(std::move(E)));
      return;
    }
    if (Stopped || ((Address >= StackBase && Address < StackBase + StackSize) ||
                    (Address >= CallbackStackBase &&
                     Address < CallbackStackBase + MaxConcurrentCallbacks *
                                                       CallbackStackStride)))
      return;
    if (!EventAvailable())
      return;
    DriverMemoryWrite Event;
    Event.PC = Result.PC;
    Event.Phase = Result.Phase;
    Event.Address = Address;
    Event.Size = Size;
    if (Size && Size <= PointerSize)
      Event.Value = Size == PointerSize
                        ? Value
                        : Value & ((uint64_t(1) << (Size * 8)) - 1);
    Result.Writes.push_back(std::move(Event));
  };
  Hooks.Fault = [&](uint64_t Address, uint32_t Size, const char *Access) {
    Stop(DriverStopReason::MemoryFault, std::string("guest ") + Access +
                                            " fault at 0x" +
                                            llvm::utohexstr(Address) + " (" +
                                            std::to_string(Size) + " bytes)");
  };
  Hooks.Interrupt = [&](uint32_t Number) {
    Stop(DriverStopReason::UnsupportedInstruction,
         "unmodeled CPU exception/interrupt " + std::to_string(Number));
  };
  Hooks.InvalidInstruction = [&]() {
    auto PC = CPU.reg(X64Register::PC);
    if (PC)
      Result.PC = *PC;
    else
      llvm::consumeError(PC.takeError());
    Stop(DriverStopReason::UnsupportedInstruction,
         "CPU rejected an invalid or unsupported instruction");
  };
  if (auto E = CPU.installHooks(std::move(Hooks)))
    return std::move(E);

  auto Deadline = std::chrono::steady_clock::now() +
                  std::chrono::milliseconds(Options.TimeoutMilliseconds);
  auto DeadlineExceeded = [&]() {
    if (std::chrono::steady_clock::now() < Deadline)
      return false;
    Stopped = false;
    Stop(DriverStopReason::Timeout, "execution time limit reached");
    return true;
  };
  auto ModelFailure = [&](llvm::Error E) {
    Stopped = false;
    Stop(CPU.hasMemoryFault() ? DriverStopReason::MemoryFault
                              : DriverStopReason::ModelError,
         llvm::toString(std::move(E)));
  };
  struct Execution {
    uint64_t ID = 0; // Zero denotes the foreground driver invocation.
    uint64_t Base = 0;
    uint64_t Size = 0;
    uint64_t InitialSP = 0;
    uint64_t PC = 0;
    std::string Phase;
    std::unique_ptr<BackendContext> Context;
    std::optional<KernelModel::Wait> Wait;
    std::optional<uint64_t> ResumeValue;
    size_t WaitEvent = 0;
    bool PrivateStack = false;
    GuestCallToken ReturnToken;
    std::optional<KernelGuestCall> ChildCall;
    std::unique_ptr<Execution> Parent;
  };
  std::vector<std::unique_ptr<Execution>> Waiting;
  std::array<bool, MaxConcurrentCallbacks> StackMapped{}, StackInUse{};
  auto NewExecution =
      [&](uint64_t PC, llvm::ArrayRef<uint64_t> Arguments,
          const std::string &Phase, uint64_t ID,
          bool Nested = false) -> llvm::Expected<std::unique_ptr<Execution>> {
    if (!CPU.executable(PC) || (PC >= ThunkBase && PC < ThunkBase + ThunkSize))
      return failure("driver callback does not name guest executable code");
    if (Arguments.size() > MaxCallbackArguments)
      return failure("scheduled callback exceeds the argument limit");
    auto Frame = std::make_unique<Execution>();
    Frame->ID = ID;
    Frame->PC = PC;
    Frame->Phase = Phase;
    Frame->Base = StackBase;
    Frame->Size = StackSize;
    Frame->PrivateStack = ID || Nested;
    if (Frame->PrivateStack) {
      auto Slot = std::find(StackInUse.begin(), StackInUse.end(), false);
      if (Slot == StackInUse.end())
        return failure("concurrent callback stack limit exhausted");
      const size_t Index = Slot - StackInUse.begin();
      Frame->Base = CallbackStackBase + PageSize + Index * CallbackStackStride;
      Frame->Size = CallbackStackSize;
      if (!StackMapped[Index]) {
        if (auto E = CPU.map(Frame->Base, Frame->Size, Read | Write))
          return std::move(E);
        StackMapped[Index] = true;
      }
      StackInUse[Index] = true;
    }
    if (auto E = Kernel.activateStack(Frame->Base, Frame->Size))
      return std::move(E);
    // Reserve the return address, Win64 shadow space and all stack parameters.
    // Callee entry RSP is always 8 mod 16, including odd stack-argument counts.
    const uint64_t Extra =
        Arguments.size() > RegisterArgumentCount
            ? (Arguments.size() - RegisterArgumentCount) * PointerSize
            : 0;
    const uint64_t Reservation =
        EntryStackReservation +
        ((Extra + StackAlignment - 1) & ~(StackAlignment - 1));
    Frame->InitialSP = Frame->Base + Frame->Size - Reservation;
    if (auto E =
            CPU.writeInteger(Frame->InitialSP, ReturnSentinel, PointerSize))
      return std::move(E);
    if (auto E = CPU.setReg(X64Register::SP, Frame->InitialSP))
      return std::move(E);
    constexpr X64Register Registers[] = {X64Register::CX, X64Register::DX,
                                         X64Register::R8, X64Register::R9};
    for (size_t I = 0;
         I < std::max(Arguments.size(), size_t(RegisterArgumentCount)); ++I) {
      const auto Value = I < Arguments.size() ? Arguments[I] : 0;
      if (I < RegisterArgumentCount) {
        if (auto E = CPU.setReg(Registers[I], Value))
          return std::move(E);
      } else if (auto E = CPU.writeInteger(
                     Frame->InitialSP + StackArgumentOffset +
                         (I - RegisterArgumentCount) * PointerSize,
                     Value, PointerSize)) {
        return std::move(E);
      }
    }
    if (auto E = CPU.setReg(X64Register::CR8, Kernel.currentIRQL()))
      return std::move(E);
    return Frame;
  };
  auto RefreshWaiters = [&]() -> llvm::Error {
    for (auto &Frame : Waiting) {
      if (!Frame->Wait)
        continue;
      auto Status = Kernel.pollWait(*Frame->Wait);
      if (!Status)
        return Status.takeError();
      if (*Status) {
        Frame->Wait.reset();
        Frame->ResumeValue = **Status;
        Result.Calls[Frame->WaitEvent].Result = **Status;
      }
    }
    return llvm::Error::success();
  };
  auto RunExecution = [&](Execution &Frame) -> llvm::Error {
    if (Frame.Context) {
      if (auto E = CPU.restoreContext(*Frame.Context))
        return E;
      if (Frame.ResumeValue) {
        if (auto E = CPU.setReg(X64Register::AX, *Frame.ResumeValue))
          return E;
        Frame.ResumeValue.reset();
      }
    }
    Result.Phase = Frame.Phase;
    Result.PC = Frame.PC;
    ExpectedReturnSP = Frame.InitialSP + PointerSize;
    ActiveStackBase = Frame.Base;
    ActiveStackSize = Frame.Size;
    Stopped = false;
    InvocationReturn.reset();
    uint64_t NextPC = Frame.PC;
    while (!Stopped) {
      auto Remaining = std::chrono::duration_cast<std::chrono::microseconds>(
                           Deadline - std::chrono::steady_clock::now())
                           .count();
      if (Remaining <= 0) {
        Stop(DriverStopReason::Timeout, "execution time limit reached");
        break;
      }
      Pending = nullptr;
      PendingGuard = 0;
      if (auto E = CPU.run(NextPC, Remaining)) {
        std::string Message = llvm::toString(std::move(E));
        if (!Stopped)
          Stop(CPU.hasMemoryFault() ? DriverStopReason::MemoryFault
                                    : DriverStopReason::EngineError,
               Message);
      }
      if (Stopped)
        break;
      if (CPU.timedOut()) {
        Stop(DriverStopReason::Timeout, "execution time limit reached");
        break;
      }
      if (PendingGuard) {
        auto Target =
            CPU.reg(PendingGuard == GuardCheckThunk ? X64Register::CX
                                                    : X64Register::AX);
        if (!Target)
          return Target.takeError();
        if (Exports.lookup(*Target))
          if (auto E = Guard.registerExportTarget(*Target))
            return E;
        if (auto E = Guard.validateTarget(*Target))
          return E;
        if (PendingGuard == GuardCheckThunk) {
          auto SP = CPU.reg(X64Register::SP);
          if (!SP)
            return SP.takeError();
          if (auto E = Kernel.validateGuestAccess(*SP, PointerSize, false))
            return E;
          auto Return = CPU.readInteger(*SP, PointerSize);
          if (!Return)
            return Return.takeError();
          if (!CPU.executable(*Return))
            return failure("CFG check has a non-executable return address");
          if (auto E = CPU.setReg(X64Register::SP, *SP + PointerSize))
            return E;
          NextPC = *Return;
        } else {
          NextPC = *Target;
        }
        continue;
      }
      if (!Pending) {
        Stop(DriverStopReason::EngineError,
             "CPU stopped without a recognized exit");
        break;
      }
      if (!EventAvailable())
        break;
      DriverAPIEvent Event;
      Event.PC = Result.PC;
      Event.Phase = Result.Phase;
      Event.Name = Pending->Name;
      auto SP = CPU.reg(X64Register::SP);
      if (!SP)
        return SP.takeError();
      if ((*SP & (StackAlignment - 1)) != PointerSize ||
          *SP > UINT64_MAX - (StackArgumentOffset +
                              MaxVariableAPIArguments * PointerSize)) {
        Stop(DriverStopReason::ModelError,
             "kernel call violates x64 stack alignment");
        break;
      }
      constexpr X64Register ArgumentRegisters[] = {
          X64Register::CX, X64Register::DX, X64Register::R8, X64Register::R9};
      unsigned Count = *KernelModel::argumentCount(*Pending);
      if (Count > MaxAPIArguments) {
        Stop(DriverStopReason::EngineError,
             "kernel API argument contract exceeds the x64 dispatcher limit");
        break;
      }
      if (auto E = Kernel.validateGuestAccess(*SP, 8, false)) {
        Stop(DriverStopReason::ModelError, llvm::toString(std::move(E)));
        break;
      }
      auto ReadArgument = [&](unsigned I) -> llvm::Expected<uint64_t> {
        if (I >= MaxVariableAPIArguments)
          return failure("kernel call exceeds the variable argument limit");
        if (I >= RegisterArgumentCount) {
          if (auto E = Kernel.validateGuestAccess(
                  *SP + StackArgumentOffset +
                      (I - RegisterArgumentCount) * PointerSize,
                  8, false)) {
            return std::move(E);
          }
        }
        return I < RegisterArgumentCount
                   ? CPU.reg(ArgumentRegisters[I])
                   : CPU.readInteger(*SP + StackArgumentOffset +
                                         (I - RegisterArgumentCount) *
                                             PointerSize,
                                     PointerSize);
      };
      for (unsigned I = 0; I < Count; ++I) {
        auto Argument = ReadArgument(I);
        if (!Argument) {
          Stop(CPU.hasMemoryFault() ? DriverStopReason::MemoryFault
                                    : DriverStopReason::ModelError,
               llvm::toString(Argument.takeError()));
          break;
        }
        Event.Arguments.push_back(*Argument);
      }
      Result.Calls.push_back(std::move(Event));
      if (Stopped)
        break;
      auto ReturnPC = CPU.readInteger(*SP, PointerSize);
      if (!ReturnPC) {
        Stop(DriverStopReason::MemoryFault,
             llvm::toString(ReturnPC.takeError()));
        break;
      }
      if (!CPU.executable(*ReturnPC)) {
        Stop(DriverStopReason::MemoryFault,
             "kernel call has a non-executable return address");
        break;
      }
      // Keep fixed arguments independent of the growing trace: a variadic read
      // can reallocate Event.Arguments while the model still holds ArrayRef.
      const std::vector<uint64_t> FixedArguments =
          Result.Calls.back().Arguments;
      auto ReadVariableArgument = [&](unsigned I) -> llvm::Expected<uint64_t> {
        auto &Arguments = Result.Calls.back().Arguments;
        if (I < Arguments.size())
          return Arguments[I];
        if (I != Arguments.size())
          return failure("variadic kernel arguments must be read in order");
        auto Value = ReadArgument(I);
        if (!Value)
          return Value.takeError();
        Arguments.push_back(*Value);
        return *Value;
      };
      auto Value = Kernel.call(*Pending, FixedArguments, ReadVariableArgument);
      if (!Value) {
        std::string Error = llvm::toString(Value.takeError());
        Result.Calls.back().Detail = Error;
        Stop(CPU.hasMemoryFault() ? DriverStopReason::MemoryFault
                                  : DriverStopReason::ModelError,
             Error);
        break;
      }
      if (auto E = RefreshWaiters())
        return E;
      if (auto Call = Kernel.takeGuestCall()) {
        Frame.ChildCall = std::move(*Call);
        Frame.WaitEvent = Result.Calls.size() - 1;
        Frame.PC = *ReturnPC;
        if (auto E = CPU.setReg(X64Register::SP, *SP + PointerSize))
          return E;
        auto Context = CPU.saveContext();
        if (!Context)
          return Context.takeError();
        Frame.Context = std::move(*Context);
        return llvm::Error::success();
      }
      if (auto Wait = Kernel.takeWait()) {
        Frame.Wait = *Wait;
        Frame.WaitEvent = Result.Calls.size() - 1;
        Frame.PC = *ReturnPC;
        if (auto E = CPU.setReg(X64Register::SP, *SP + PointerSize))
          return E;
        auto Context = CPU.saveContext();
        if (!Context)
          return Context.takeError();
        Frame.Context = std::move(*Context);
        if (Frame.ID)
          if (auto E = Kernel.suspendScheduled(Frame.ID))
            return E;
        return llvm::Error::success();
      }
      Result.Calls.back().Result = *Value;
      if (auto E = CPU.setReg(X64Register::AX, *Value))
        return std::move(E);
      if (auto E = CPU.setReg(X64Register::SP, *SP + PointerSize))
        return std::move(E);
      NextPC = *ReturnPC;
    }
    return llvm::Error::success();
  };
  auto Pump = [&](std::unique_ptr<Execution> Current,
                  const std::string &ParentPhase) -> llvm::Error {
    const bool Foreground = bool(Current) && !Current->ID;
    while (!DeadlineExceeded()) {
      if (Current) {
        if (auto E = RunExecution(*Current)) {
          ModelFailure(std::move(E));
          return llvm::Error::success();
        }
        if (Current->ChildCall) {
          auto Call = std::move(*Current->ChildCall);
          Current->ChildCall.reset();
          auto Child =
              NewExecution(Call.PC, Call.Arguments, guestCallPhase(Call.Token),
                           Current->ID, true);
          if (!Child) {
            ModelFailure(Child.takeError());
            return llvm::Error::success();
          }
          (*Child)->ReturnToken = Call.Token;
          (*Child)->Parent = std::move(Current);
          Current = std::move(*Child);
          continue;
        }
        if (Current->Wait) {
          Waiting.push_back(std::move(Current));
        } else {
          if (Result.Stop != DriverStopReason::Returned)
            return llvm::Error::success();
          if (auto E = Kernel.retireStack(Current->Base, Current->Size)) {
            ModelFailure(std::move(E));
            return llvm::Error::success();
          }
          if (Current->PrivateStack)
            StackInUse[(Current->Base - CallbackStackBase) /
                       CallbackStackStride] = false;
          if (Current->Parent) {
            auto Completion =
                Kernel.finishGuestCall(Current->ReturnToken, *InvocationReturn);
            if (!Completion) {
              ModelFailure(Completion.takeError());
              return llvm::Error::success();
            }
            auto Parent = std::move(Current->Parent);
            if (*Completion) {
              Parent->ResumeValue = **Completion;
              Result.Calls[Parent->WaitEvent].Result = **Completion;
            } else {
              Parent->ChildCall = Kernel.takeGuestCall();
              if (!Parent->ChildCall) {
                ModelFailure(
                    failure("model continuation lost its guest callback"));
                return llvm::Error::success();
              }
              auto Call = std::move(*Parent->ChildCall);
              Parent->ChildCall.reset();
              auto Child =
                  NewExecution(Call.PC, Call.Arguments,
                               guestCallPhase(Call.Token), Parent->ID, true);
              if (!Child) {
                ModelFailure(Child.takeError());
                return llvm::Error::success();
              }
              (*Child)->ReturnToken = Call.Token;
              (*Child)->Parent = std::move(Parent);
              Current = std::move(*Child);
              continue;
            }
            Current = std::move(Parent);
            continue;
          }
          if (!Current->ID)
            return llvm::Error::success();
          auto Continuation =
              Kernel.continueScheduled(Current->ID, *InvocationReturn);
          if (!Continuation) {
            ModelFailure(Continuation.takeError());
            return llvm::Error::success();
          }
          if (*Continuation) {
            auto Frame =
                NewExecution((**Continuation).PC, (**Continuation).Arguments,
                             Current->Phase, Current->ID);
            if (!Frame) {
              ModelFailure(Frame.takeError());
              return llvm::Error::success();
            }
            Current = std::move(*Frame);
            continue;
          }
          if (auto E = Kernel.finishScheduled(Current->ID)) {
            ModelFailure(std::move(E));
            return llvm::Error::success();
          }
          Current.reset();
        }
      }
      // Waiters retain independent stacks and CPU state. Ready passive frames
      // yield to queued DPCs, cancellation and provider completion callbacks.
      if (auto E = RefreshWaiters()) {
        ModelFailure(std::move(E));
        return llvm::Error::success();
      }
      for (size_t I = 0;
           I < Waiting.size() && !Kernel.hasQueuedPriorityCallback(); ++I) {
        if (Waiting[I]->Wait)
          continue;
        Current = std::move(Waiting[I]);
        Waiting.erase(Waiting.begin() + I);
        if (Current->ID) {
          if (auto E = Kernel.resumeScheduled(Current->ID)) {
            ModelFailure(std::move(E));
            return llvm::Error::success();
          }
        } else {
          Kernel.enterForeground();
        }
        break;
      }
      if (Current)
        continue;
      auto Next = Kernel.nextScheduled(false);
      if (!Next) {
        ModelFailure(Next.takeError());
        return llvm::Error::success();
      }
      if (auto E = RefreshWaiters()) {
        ModelFailure(std::move(E));
        return llvm::Error::success();
      }
      if (!*Next) {
        if (std::any_of(Waiting.begin(), Waiting.end(),
                        [](const auto &Frame) { return !Frame->Wait; }))
          continue;
        if (!Foreground && Waiting.empty() && !Kernel.requestPending()) {
          Result.Phase = ParentPhase;
          Result.Stop = DriverStopReason::Returned;
          return llvm::Error::success();
        }
        std::optional<uint64_t> Bound = Kernel.nextEventTime();
        for (const auto &Frame : Waiting)
          if (Frame->Wait && Frame->Wait->Deadline &&
              (!Bound || *Frame->Wait->Deadline < *Bound))
            Bound = Frame->Wait->Deadline;
        if (!Bound) {
          ModelFailure(
              failure("STATUS_PENDING request or blocked wait is "
                      "stalled: no scheduled completion source remains"));
          return llvm::Error::success();
        }
        Next = Kernel.nextScheduled(true, Bound);
        if (!Next) {
          ModelFailure(Next.takeError());
          return llvm::Error::success();
        }
        // Timer expiration satisfies existing waits before its queued DPC can
        // rearm the timer and clear its signal.
        if (auto E = RefreshWaiters()) {
          ModelFailure(std::move(E));
          return llvm::Error::success();
        }
        if (!*Next)
          continue;
      }
      auto Frame =
          NewExecution((**Next).PC, (**Next).Arguments,
                       std::string(CallbackPhase) + std::to_string((**Next).ID),
                       (**Next).ID);
      if (!Frame) {
        ModelFailure(Frame.takeError());
        return llvm::Error::success();
      }
      Current = std::move(*Frame);
    }
    return llvm::Error::success();
  };
  auto Invoke = [&](const KernelModel::Invocation &Invocation,
                    const std::string &Phase) -> llvm::Error {
    Kernel.enterForeground();
    std::vector<uint64_t> Arguments{Invocation.Argument0, Invocation.Argument1,
                                    Invocation.Argument2, Invocation.Argument3};
    Arguments.insert(Arguments.end(), Invocation.StackArguments.begin(),
                     Invocation.StackArguments.end());
    auto Frame = NewExecution(Invocation.PC, Arguments, Phase, 0);
    if (!Frame) {
      ModelFailure(Frame.takeError());
      return llvm::Error::success();
    }
    return Pump(std::move(*Frame), Phase);
  };
  auto DrainCallbacks = [&]() -> llvm::Error {
    const std::string ParentPhase = Result.Phase;
    return Pump(nullptr, ParentPhase);
  };
  if (auto E =
          Invoke({Image->Entry, Kernel.driverObject(), Kernel.registryPath()},
                 EntryPhase))
    return std::move(E);
  if (Result.Stop == DriverStopReason::Returned)
    Result.NTStatus = InvocationReturn;
  if (Result.Stop == DriverStopReason::Returned) {
    if (auto E = Kernel.finishEntry())
      ModelFailure(std::move(E));
    else if (*Result.NTStatus && !(*Result.NTStatus & NTStatusFailureMask))
      ModelFailure(
          failure("DriverEntry must return STATUS_SUCCESS to initialize; "
                  "nonzero successful or pending status is unsupported"));
  }
  // Entry failure is a valid completed observation. No requests or unload are
  // delivered to a driver whose initialization did not succeed.
  if (Result.Stop == DriverStopReason::Returned && Result.NTStatus == 0) {
    if (auto E = DrainCallbacks())
      return std::move(E);
    if (Result.Stop == DriverStopReason::Returned)
      if (auto E = Kernel.preparePnpDevices())
        ModelFailure(std::move(E));
    for (const auto &Device : Options.PnpDevices) {
      if (Result.Stop != DriverStopReason::Returned)
        break;
      Result.Phase = std::string(AddDevicePhase) + Device.ID;
      if (DeadlineExceeded())
        break;
      auto Invocation = Kernel.beginAddDevice(Device.ID);
      if (!Invocation) {
        ModelFailure(Invocation.takeError());
        break;
      }
      if (auto E = Invoke(*Invocation, Result.Phase))
        return std::move(E);
      if (Result.Stop != DriverStopReason::Returned)
        break;
      if (auto E =
              Kernel.finishAddDevice(Device.ID, uint32_t(*InvocationReturn))) {
        ModelFailure(std::move(E));
        break;
      }
      if (auto E = DrainCallbacks())
        return std::move(E);
    }
    for (size_t Index = 0; Index < Options.Requests.size(); ++Index) {
      if (Result.Stop != DriverStopReason::Returned)
        break;
      Result.Phase = std::string(RequestPhase) + std::to_string(Index);
      if (DeadlineExceeded())
        break;
      auto Invocation = Kernel.beginRequest(Options.Requests[Index]);
      if (!Invocation) {
        ModelFailure(Invocation.takeError());
        break;
      }
      if (Invocation->PC) {
        if (auto E = Invoke(*Invocation, Result.Phase))
          return std::move(E);
        if (Result.Stop != DriverStopReason::Returned)
          break;
        const uint32_t DispatchStatus =
            Invocation->FrameworkDispatchStatus.value_or(
                uint32_t(*InvocationReturn));
        if (auto E =
                Kernel.recordDispatchReturn(Invocation->IRP, DispatchStatus)) {
          ModelFailure(std::move(E));
          break;
        }
      }
      const bool Deferred = Kernel.requestPending(Invocation->IRP);
      if (!Deferred) {
        if (auto E = Kernel.finalizeRequest(Invocation->IRP)) {
          ModelFailure(std::move(E));
          break;
        }
      }
      if (auto E = DrainCallbacks())
        return std::move(E);
      if (Result.Stop != DriverStopReason::Returned)
        break;
      if (Deferred) {
        if (auto E = Kernel.finalizeRequest(Invocation->IRP)) {
          ModelFailure(std::move(E));
          break;
        }
      }
    }
    if (Result.Stop == DriverStopReason::Returned && Options.Unload &&
        !DeadlineExceeded()) {
      Result.Phase = UnloadPhase;
      auto Invocation = Kernel.beginUnload();
      if (!Invocation) {
        ModelFailure(Invocation.takeError());
      } else {
        if (auto E = Invoke(*Invocation, UnloadPhase))
          return std::move(E);
        if (Result.Stop == DriverStopReason::Returned) {
          if (auto E = Kernel.finishUnload())
            ModelFailure(std::move(E));
          else
            Result.UnloadCompleted = true;
        }
      }
    }
  }
  if (auto E = Kernel.snapshot()) {
    Result.Diagnostic += (Result.Diagnostic.empty() ? "" : "; ") +
                         std::string("object snapshot failed: ") +
                         llvm::toString(std::move(E));
    if (Result.Stop == DriverStopReason::Returned) {
      Result.Stop = DriverStopReason::ModelError;
    }
  }
  if (Result.Stop == DriverStopReason::Returned)
    DeadlineExceeded();
  if (auto Fault = CPU.fault()) {
    DriverFault Observation;
    Observation.Kind = backendFaultKindName(Fault->Kind);
    Observation.PC = Fault->PC;
    Observation.Address = Fault->Address;
    Observation.Size = Fault->Size;
    Observation.Interrupt = Fault->Interrupt;
    if (Fault->Access)
      Observation.Access = backendAccessKindName(*Fault->Access);
    Result.Fault = std::move(Observation);
  }
  return Result;
}
} // namespace neverd::emulation
