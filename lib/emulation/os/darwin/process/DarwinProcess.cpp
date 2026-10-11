//===- DarwinProcess.cpp - Bounded macOS/iOS process continuations --------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "DarwinProcess.h"

#include "../../../core/ExecutionDeadline.h"
#include "../../../runtime/RuntimeValues.h"
#include "../kernel/DarwinEntropy.h"
#include "../kernel/DarwinFiles.h"
#include "../kernel/DarwinMemory.h"
#include "../kernel/DarwinSystem.h"

#include "neverd/emulation/ExecutionSession.h"

#include "llvm/Support/Endian.h"

namespace neverd::emulation::darwin_model {
using namespace value;
llvm::Expected<ProcessResult> runProcess(const std::filesystem::path &Path,
                                         ProfileSpec Profile,
                                         const ProcessOptions &Options) {
  if (Options.Android || !Options.Limits.Instructions ||
      !Options.Limits.Events || !Options.Limits.TimeoutMicroseconds ||
      !Options.MemoryLimit || !Options.StackSize || !Options.OutputLimit ||
      !Options.InstructionQuantum || Options.StackSize > MaxStackSize ||
      Options.StackSize >= Options.MemoryLimit ||
      Options.OutputLimit > Options.MemoryLimit ||
      Options.Arguments.size() > Options.StackSize / 8 ||
      Options.Environment.size() > Options.StackSize / 8)
    return failure(diagnostic::ProcessLimits);
  auto Deadline = makeExecutionDeadline(Options.Limits.TimeoutMicroseconds);
  if (!Deadline)
    return Deadline.takeError();
  auto Image = loadImage(Path, Profile, Options);
  if (!Image)
    return Image.takeError();
  auto Physical = PhysicalMemory::create(Options.MemoryLimit);
  if (!Physical)
    return Physical.takeError();
  auto Space = AddressSpace::create(*Physical, Options.MemoryLimit);
  if (!Space)
    return Space.takeError();
  // An initial segment must not retain a multi-page physical owner after a
  // partial munmap. Image, stack and anonymous mappings share this invariant.
  auto MapPages = [&](uint64_t Address, uint64_t Size,
                      unsigned Permissions) -> llvm::Error {
    for (uint64_t Offset = 0; Offset < Size; Offset += Image->Memory.PageSize)
      if (auto E = (*Space)->map(Address + Offset, Image->Memory.PageSize,
                                 Permissions))
        return E;
    return llvm::Error::success();
  };
  for (const auto &Region : Image->Plan.Regions) {
    if (auto E = MapPages(Region.Address, Region.Bytes.size(), Read | Write))
      return std::move(E);
    if (auto E = (*Space)->write(Region.Address, Region.Bytes))
      return std::move(E);
    if (auto E = (*Space)->protect(Region.Address, Region.Bytes.size(),
                                   Region.Permissions))
      return std::move(E);
  }
  auto Entry =
      (*Space)->canAccess(Image->Plan.Entry, 1, Execute | UserAccessible);
  if (!Entry)
    return Entry.takeError();
  if (!*Entry)
    return failure(diagnostic::ExecutableEntry);
  const auto StackBase = StackTop - Options.StackSize;
  if (auto E =
          MapPages(StackBase, Options.StackSize, Read | Write | UserAccessible))
    return std::move(E);
  auto Stack = prepareStack(**Space, Options, Path.filename().string());
  if (!Stack)
    return Stack.takeError();
  if (auto E = (*Space)->map(ReturnGate, Image->Memory.PageSize, Read | Write))
    return std::move(E);
  const bool X64 = Image->Architecture == GuestArchitecture::X64;
  const uint8_t TrapX64[] = {0x0f, 0x05};
  // svc #0x80; the gate's private address authenticates the main return.
  const uint8_t TrapARM64[] = {0x01, 0x10, 0x00, 0xd4};
  if (auto E =
          (*Space)->write(ReturnGate, X64 ? llvm::ArrayRef<uint8_t>(TrapX64)
                                          : llvm::ArrayRef<uint8_t>(TrapARM64)))
    return std::move(E);
  if (auto E = (*Space)->protect(ReturnGate, Image->Memory.PageSize,
                                 Read | Execute | UserAccessible))
    return std::move(E);
  auto Backend =
      createExecutionBackend(Options.Backend,
                             X64 ? ExecutionContract::CheckedUserX64
                                 : ExecutionContract::CheckedUserAArch64,
                             *Space, Image->Architecture);
  if (!Backend)
    return Backend.takeError();
  const auto PCRegister = X64 ? CPURegister::X64PC : CPURegister::AArch64PC;
  if (Image->MainEntry) {
    auto ABI = IntegerABI::get(X64 ? IntegerCallingConvention::SysVAMD64
                                   : IntegerCallingConvention::AAPCS64);
    if (!ABI)
      return ABI.takeError();
    const uint64_t Arguments[] = {Stack->Argc, Stack->Argv, Stack->Envp,
                                  Stack->Apple};
    auto Frame = ABI->prepareCall(*Backend->CPU, StackBase,
                                  Stack->SP - StackBase, ReturnGate, Arguments);
    if (!Frame)
      return Frame.takeError();
  } else if (auto E = Backend->CPU->writeRegister(X64 ? CPURegister::X64SP
                                                      : CPURegister::AArch64SP,
                                                  {Stack->SP, 0}))
    return std::move(E);
  if (auto E = Backend->CPU->writeRegister(PCRegister, {Image->Plan.Entry, 0}))
    return std::move(E);
  auto Budget = ExecutionBudget::create(Options.Limits);
  if (!Budget)
    return Budget.takeError();
  std::shared_ptr<ExecutionBudget> Resources(std::move(*Budget));
  auto Session = ExecutionSession::create(std::move(Backend->CPU), Resources);
  if (!Session)
    return Session.takeError();
  auto &CPU = (*Session)->cpu();
  DarwinMemory Memory(**Space, Image->Memory, Options);
  DarwinFiles Files(CPU, Options.DarwinFiles, Options.OutputLimit,
                    credentialID(ServiceKind::GetEUID, Options.DarwinSystem),
                    Options.DarwinSystem ? Options.DarwinSystem->Credentials
                                         : std::nullopt);
  DarwinEntropy Entropy(Options.DarwinSystem);
  ProcessResult Result{Profile.Profile, Image->Architecture, Backend->Kind,
                       Backend->Reason};
  Result.Entry = Result.PC = Image->Plan.Entry;
  auto RuntimeFailure = [&](llvm::Error E) {
    Result.Stop = ProcessStopReason::RuntimeFailure;
    Result.Diagnostic = llvm::toString(std::move(E));
  };
  while (true) {
    auto Exit = (*Session)->run(Result.PC, Options.InstructionQuantum);
    if (!Exit) {
      RuntimeFailure(Exit.takeError());
      break;
    }
    Result.LastCPUExit = std::move(Exit->CPU);
    auto PC = CPU.readRegister(PCRegister);
    if (!PC) {
      RuntimeFailure(PC.takeError());
      break;
    }
    Result.PC = (*PC)[0];
    if (Exit->Kind == SessionExitKind::Quantum)
      continue;
    if (Exit->Kind == SessionExitKind::InstructionLimit) {
      Result.Stop = ProcessStopReason::InstructionLimit;
      Result.Diagnostic = runtime::InstructionLimit;
      break;
    }
    if (Exit->Kind == SessionExitKind::Timeout) {
      Result.Stop = ProcessStopReason::Timeout;
      Result.Diagnostic = runtime::Timeout;
      break;
    }
    if (!Result.LastCPUExit ||
        Result.LastCPUExit->Kind != ExecutionExitKind::ServiceRequest) {
      Result.Stop = ProcessStopReason::CPUFailure;
      Result.Diagnostic =
          Result.LastCPUExit && !Result.LastCPUExit->Diagnostic.empty()
              ? Result.LastCPUExit->Diagnostic
              : diagnostic::MissingContinuation;
      break;
    }
    if (!Resources->consumeEvents()) {
      Result.Stop = ProcessStopReason::EventLimit;
      Result.Diagnostic = runtime::EventLimit;
      break;
    }
    auto Request = (*Session)->takeServiceRequest();
    if (!Request) {
      RuntimeFailure(Request.takeError());
      break;
    }
    if (Request->Kind != (X64 ? ServiceRequestKind::X64Syscall
                              : ServiceRequestKind::AArch64SVC) ||
        (!X64 && Request->Immediate != SVCImmediate)) {
      Result.Stop = ProcessStopReason::UnsupportedService;
      Result.Diagnostic = diagnostic::ServiceTrap;
      break;
    }
    if (Image->MainEntry && Request->PC == ReturnGate) {
      auto Value =
          CPU.readRegister(X64 ? CPURegister::X64AX : CPURegister::AArch64X0);
      if (!Value) {
        RuntimeFailure(Value.takeError());
        break;
      }
      Result.Stop = ProcessStopReason::Exited;
      Result.ExitStatus = (*Value)[0] & 0xff;
      Result.ReturnValue = (*Value)[0];
      break;
    }
    auto Event = readService(CPU, *Request);
    if (!Event) {
      RuntimeFailure(Event.takeError());
      break;
    }
    Result.Services.push_back(*Event);
    auto Returned =
        handleService(CPU, Memory, Files, Entropy, *Event, Options, Result);
    if (!Returned) {
      RuntimeFailure(Returned.takeError());
      break;
    }
    if (!*Returned)
      break;
    Result.Services.back().Result = (**Returned).Value;
    if ((**Returned).Convention == ServiceConvention::BSD)
      Result.Services.back().Error = (**Returned).Error;
    if (auto E = returnService(CPU, *Request, **Returned)) {
      RuntimeFailure(std::move(E));
      break;
    }
    Result.PC = Request->NextPC;
  }
  Result.Instructions = Resources->instructions();
  Result.Events = Resources->events();
  return Result;
}
} // namespace neverd::emulation::darwin_model
