//===- KernelReferenceInitializer.cpp - Original initialization replay ----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Original initialization replay.
///

#include "KernelReferenceInitializer.h"

#include "../driver/WindowsX64ExecutionPolicy.h"
#include "KernelReferenceImage.h"

#include "neverd/emulation/CPU.h"
#include "neverd/emulation/IntegerABI.h"

#include "llvm/ADT/ScopeExit.h"
#include "llvm/ADT/StringExtras.h"

#include <algorithm>
#include <chrono>
#include <exception>
#include <iterator>
#include <map>
#include <memory>
#include <set>

namespace neverd::emulation {
namespace {
constexpr uint64_t PageSize = 4096;
llvm::Error invalid(const llvm::Twine &Text) {
  return llvm::createStringError(llvm::inconvertibleErrorCode(), Text);
}
bool validRange(uint64_t Address, uint64_t Size) {
  return Size && Size <= UINT64_MAX - Address;
}
bool overlaps(uint64_t A, uint64_t N, uint64_t B, uint64_t M) {
  return A < B + M && B < A + N;
}
class Ranges {
  std::map<uint64_t, uint64_t> Ends;

public:
  void add(uint64_t Address, uint64_t Size) {
    uint64_t End = Address + Size;
    auto I = Ends.lower_bound(Address);
    if (I != Ends.begin() && std::prev(I)->second >= Address) {
      --I;
      Address = I->first;
    }
    while (I != Ends.end() && I->first <= End) {
      End = std::max(End, I->second);
      I = Ends.erase(I);
    }
    Ends.emplace(Address, End);
  }
  bool contains(uint64_t Address, uint64_t Size) const {
    if (!validRange(Address, Size))
      return false;
    auto I = Ends.upper_bound(Address);
    return I != Ends.begin() && Address + Size <= std::prev(I)->second;
  }
  bool intersects(uint64_t Address, uint64_t Size) const {
    const auto I = Ends.lower_bound(Address);
    return (I != Ends.end() && I->first < Address + Size) ||
           (I != Ends.begin() && std::prev(I)->second > Address);
  }
};
} // namespace

llvm::Expected<KernelInitializerResult>
initializeFromKernelReference(const KernelReferenceImage &Reference,
                              const KernelInitializerPlan &Plan) {
  if (Reference.pdbDigest().empty())
    return invalid("kernel initialization requires a matched PDB");
  if (Plan.Calls.empty() || Plan.Calls.size() > 16 ||
      Plan.Inputs.size() > 4096 || Plan.Writable.size() > 4096 ||
      Plan.Outputs.size() > 64 || Plan.Services.size() > 64 ||
      !Plan.InstructionLimit || Plan.InstructionLimit > 1000000 ||
      !Plan.TimeoutMicroseconds || Plan.TimeoutMicroseconds > 30000000 ||
      !Plan.MemoryLimit || Plan.MemoryLimit > 128ULL * 1024 * 1024 ||
      !validRange(Plan.StackBase, Plan.StackSize) ||
      Plan.StackBase % PageSize || Plan.StackSize % PageSize ||
      Plan.StackSize > 1024 * 1024 || Plan.ReturnPC % PageSize ||
      !validRange(Plan.ReturnPC, PageSize) ||
      overlaps(Plan.StackBase, Plan.StackSize, Plan.ReturnPC, PageSize))
    return invalid("invalid bounded kernel initialization plan");
  Ranges Known, Mutable;
  std::set<uint64_t> Pages;
  auto Include = [&](uint64_t Address, uint64_t Size) -> llvm::Error {
    if (!validRange(Address, Size) || Size > Plan.MemoryLimit ||
        overlaps(Address, Size, Plan.StackBase, Plan.StackSize) ||
        overlaps(Address, Size, Plan.ReturnPC, PageSize))
      return invalid(
          "kernel initializer range overlaps its helper or overflows");
    for (uint64_t Page = Address & ~(PageSize - 1); Page < Address + Size;
         Page += PageSize) {
      if (Page > UINT64_MAX - PageSize)
        return invalid("kernel initializer page extent overflows");
      if (Reference.contains(Page))
        return invalid("kernel initializer cannot replace original code");
      if (Reference.exportData().overlapsBytes(Address, Size))
        return invalid("kernel initializer cannot replace export metadata");
      if (Page >= Reference.base() &&
          Page - Reference.base() < Reference.size()) {
        const bool Data = std::any_of(
            Reference.opaqueData().begin(), Reference.opaqueData().end(),
            [&](const auto &R) {
              return Page >= R.Address && Page - R.Address < R.Bytes.size();
            });
        if (!Data)
          return invalid(
              "kernel initializer range is in a reference image gap");
      } else {
        Pages.insert(Page);
        if (Pages.size() > Plan.MemoryLimit / PageSize)
          return invalid("kernel initializer mapping budget exceeded");
      }
    }
    return llvm::Error::success();
  };
  for (const auto &R : Plan.Writable) {
    if (auto E = Include(R.Address, R.Size))
      return std::move(E);
    Mutable.add(R.Address, R.Size);
  }
  Ranges Inputs;
  for (const auto &R : Plan.Inputs) {
    if (auto E = Include(R.Address, R.Bytes.size()))
      return std::move(E);
    if (Inputs.intersects(R.Address, R.Bytes.size()))
      return invalid("overlapping kernel initializer inputs");
    Inputs.add(R.Address, R.Bytes.size());
  }
  uint64_t OutputBytes = 0;
  for (const auto &R : Plan.Outputs) {
    if (!validRange(R.Address, R.Size) ||
        R.Size > Plan.MemoryLimit - OutputBytes)
      return invalid("invalid kernel initializer output range");
    OutputBytes += R.Size;
  }
  // Helper storage cannot shadow any reference page, including opaque data.
  for (const auto R : {KernelInitializerRange{Plan.StackBase, Plan.StackSize},
                       {Plan.ReturnPC, PageSize}})
    if (overlaps(R.Address, R.Size, Reference.base(), Reference.size()))
      return invalid("kernel initializer helper overlaps the reference image");
  auto Backend =
      createExecutionBackend(ExecutionBackendKind::Unicorn,
                             ExecutionContract::Legacy, Plan.MemoryLimit);
  if (!Backend)
    return Backend.takeError();
  auto &CPU = *Backend->CPU;
  const auto BranchModel = CPU.x64BranchModel();
  if (!BranchModel)
    return invalid("kernel initializer CPU lacks an x64 branch model");
  WindowsX64ExecutionPolicy Policy;
  if (auto E = Policy.initialize(
          *BranchModel, false, false,
          WindowsX64ExecutionPolicy::Purpose::ReferenceInitialization, false,
          Plan.TimestampCounter.has_value()))
    return std::move(E);
  if (auto E = CPU.setReg(X64Register::CR8, 0))
    return std::move(E);
  if (auto E = Reference.map(CPU))
    return std::move(E);
  for (auto Page : Pages)
    if (auto E = CPU.map(Page, PageSize, Read | Write))
      return std::move(E);
  if (auto E = CPU.map(Plan.StackBase, Plan.StackSize, Read | Write))
    return std::move(E);
  if (auto E = CPU.map(Plan.ReturnPC, PageSize, Read | Execute))
    return std::move(E);
  Known.add(Plan.StackBase, Plan.StackSize);
  Mutable.add(Plan.StackBase, Plan.StackSize);
  for (const auto &R : Plan.Inputs) {
    if (auto E = CPU.writeBacking(R.Address, R.Bytes))
      return std::move(E);
    Known.add(R.Address, R.Bytes.size());
  }
  if (auto E = CPU.setGSBase(Plan.GSBase))
    return std::move(E);
  std::map<uint64_t, const KernelInitializerService *> Services;
  for (const auto &S : Plan.Services)
    if (!S.Prepare || S.Arity > 32 || !CPU.executable(S.Entry) ||
        !Reference.contains(S.Entry) || !Services.emplace(S.Entry, &S).second)
      return invalid("invalid or duplicate kernel initializer service");
  auto ABI = IntegerABI::get(IntegerCallingConvention::Win64);
  if (!ABI)
    return ABI.takeError();
  KernelInitializerResult Result;
  std::string Failure;
  bool Returned = false;
  uint64_t CurrentPC = 0;
  const KernelInitializerService *Pending = nullptr;
  std::optional<WindowsX64ExecutionPolicy::Action> PendingControl;
  uint64_t NextControlPC = 0;
  auto Reject = [&](const llvm::Twine &Text) {
    if (Failure.empty())
      Failure = Text.str();
    CPU.stop();
  };
  auto Defined = [&](uint64_t Address, uint64_t Size) {
    if (Known.contains(Address, Size))
      return true;
    if (!validRange(Address, Size))
      return false;
    for (uint64_t I = 0; I < Size; ++I)
      if (!Known.contains(Address + I, 1) && !Reference.contains(Address + I))
        return false;
    return true;
  };
  BackendHooks Hooks;
  Hooks.Instruction = [&](uint64_t Address, uint32_t Size) {
    CurrentPC = Address;
    if (Address == Plan.ReturnPC) {
      Returned = true;
      CPU.stop();
    } else if (const auto I = Services.find(Address); I != Services.end()) {
      Pending = I->second;
      CPU.stop();
    } else if (!Reference.contains(Address)) {
      Reject("kernel initializer executed outside original code");
    } else if (Result.Instructions == Plan.InstructionLimit) {
      Reject("kernel initializer instruction limit exceeded");
    } else {
      if (!Size || Size > profile::MaxInstructionSize) {
        Reject("kernel initializer instruction has an invalid extent");
        return;
      }
      std::array<uint8_t, profile::MaxInstructionSize> Bytes;
      auto Instruction = llvm::MutableArrayRef(Bytes).take_front(Size);
      if (auto E = CPU.fetch(Address, Instruction)) {
        Reject(llvm::toString(std::move(E)));
        return;
      }
      auto Action = Policy.inspect(Instruction, Address);
      if (!Action) {
        Reject(llvm::toString(Action.takeError()));
        return;
      }
      if (*Action) {
        using Kind = WindowsX64ExecutionPolicy::Action::Kind;
        if ((**Action).Source != Kind::ReadIRQL &&
            (**Action).Source != Kind::WriteIRQL &&
            (**Action).Source != Kind::DisableInterrupts &&
            (**Action).Source != Kind::EnableInterrupts &&
            (**Action).Source != Kind::ReadTimestamp) {
          Reject("kernel initializer instruction needs an unbound environment "
                 "action");
          return;
        }
        PendingControl = **Action;
        NextControlPC = Address + Size;
        CPU.stop();
      }
      ++Result.Instructions;
    }
  };
  Hooks.Read = [&](uint64_t Address, uint32_t Size) {
    if (!Defined(Address, Size))
      Reject("kernel initializer read undefined input at 0x" +
             llvm::utohexstr(Address) + " (PC 0x" + llvm::utohexstr(CurrentPC) +
             ")");
  };
  Hooks.Write = [&](uint64_t Address, uint32_t Size, uint64_t) {
    if (!Mutable.contains(Address, Size))
      Reject("kernel initializer wrote outside declared storage at 0x" +
             llvm::utohexstr(Address) + " (PC 0x" + llvm::utohexstr(CurrentPC) +
             ")");
    else
      Known.add(Address, Size);
  };
  if (auto E = CPU.installHooks(std::move(Hooks)))
    return std::move(E);
  const auto Deadline = std::chrono::steady_clock::now() +
                        std::chrono::microseconds(Plan.TimeoutMicroseconds);
  for (const auto &Call : Plan.Calls) {
    if (Call.Arguments.size() > 32)
      return invalid("kernel initializer call has too many arguments");
    if (!CPU.executable(Call.Entry) || !Reference.contains(Call.Entry))
      return invalid(
          "kernel initializer entry is not original executable code");
    auto Frame = ABI->prepareCall(CPU, Plan.StackBase, Plan.StackSize,
                                  Plan.ReturnPC, Call.Arguments);
    if (!Frame)
      return Frame.takeError();
    uint64_t PC = Call.Entry;
    Returned = false;
    while (!Returned) {
      Pending = nullptr;
      PendingControl.reset();
      const auto Left = std::chrono::duration_cast<std::chrono::microseconds>(
                            Deadline - std::chrono::steady_clock::now())
                            .count();
      if (Left <= 0)
        return invalid("kernel initializer deadline exceeded");
      auto Exit = CPU.runUntilExit(PC, uint64_t(Left));
      if (!Exit)
        return Exit.takeError();
      if (!Failure.empty())
        return invalid(Failure);
      if (const auto Fault = CPU.fault())
        return invalid("kernel initializer CPU fault at PC 0x" +
                       llvm::utohexstr(Fault->PC) +
                       (Fault->Address
                            ? ", address 0x" + llvm::utohexstr(*Fault->Address)
                            : std::string()));
      if (CPU.hasDeviceError())
        return invalid("kernel initializer device error");
      if (CPU.timedOut())
        return invalid("kernel initializer deadline exceeded");
      if (Returned)
        break;
      if (PendingControl) {
        using Kind = WindowsX64ExecutionPolicy::Action::Kind;
        if (PendingControl->Source == Kind::ReadTimestamp) {
          if (!Plan.TimestampCounter)
            return invalid("kernel initializer lacks a timestamp source");
          if (auto E =
                  CPU.setReg(X64Register::AX, uint32_t(*Plan.TimestampCounter)))
            return std::move(E);
          if (auto E = CPU.setReg(X64Register::DX,
                                  uint32_t(*Plan.TimestampCounter >> 32)))
            return std::move(E);
          PC = NextControlPC;
          continue;
        }
        if (PendingControl->Source == Kind::DisableInterrupts ||
            PendingControl->Source == Kind::EnableInterrupts) {
          // Ring-zero private CPU with no interrupt producers. Model IF;
          // STI's delivery delay cannot deliver an event in this profile.
          auto Flags = CPU.reg(X64Register::FLAGS);
          if (!Flags)
            return Flags.takeError();
          constexpr uint64_t IF = 1u << 9;
          const auto Value = PendingControl->Source == Kind::EnableInterrupts
                                 ? *Flags | IF
                                 : *Flags & ~IF;
          if (auto E = CPU.setReg(X64Register::FLAGS, Value))
            return std::move(E);
          PC = NextControlPC;
          continue;
        }
        const bool Write = PendingControl->Source == Kind::WriteIRQL;
        const auto Register =
            Write ? PendingControl->Input : PendingControl->Destination;
        if (!Register)
          return invalid("kernel initializer control action lacks a register");
        auto Value = CPU.reg(Write ? *Register : X64Register::CR8);
        if (!Value)
          return Value.takeError();
        if (Write && *Value > 15)
          return invalid("kernel initializer CR8 write sets reserved bits");
        if (auto E = CPU.setReg(Write ? X64Register::CR8 : *Register, *Value))
          return std::move(E);
        PC = NextControlPC;
        continue;
      }
      if (!Pending || ++Result.ServiceCalls > 64)
        return invalid(
            "kernel initializer stopped without completion or service");
      auto SP = CPU.reg(X64Register::SP);
      if (!SP)
        return SP.takeError();
      if (!Known.contains(*SP, 8))
        return invalid(
            "kernel initializer service has an undefined return slot");
      std::vector<uint64_t> Arguments;
      for (unsigned I = 0; I < Pending->Arity; ++I) {
        auto Location = ABI->argumentLocation(*SP, I);
        if (!Location)
          return Location.takeError();
        if (Location->Register == CPURegister::Invalid &&
            !Known.contains(Location->Address, 8))
          return invalid(
              "kernel initializer service has an undefined argument");
        auto A = ABI->readArgument(CPU, *SP, I);
        if (!A)
          return A.takeError();
        Arguments.push_back(*A);
      }
      auto ReadActive = std::make_shared<bool>(true);
      KernelInitializerServiceInput Input{
          Arguments,
          [&, ReadActive](uint64_t Address, uint64_t Size)
              -> llvm::Expected<std::vector<uint8_t>> {
            if (!*ReadActive)
              return invalid("kernel initializer service reader has expired");
            if (!Size || Size > Plan.MemoryLimit || !validRange(Address, Size))
              return invalid("invalid kernel initializer service read range");
            if (!Defined(Address, Size))
              return invalid("kernel initializer service read undefined input "
                             "at 0x" +
                             llvm::utohexstr(Address));
            std::vector<uint8_t> Bytes(Size);
            if (auto E = CPU.readBacking(Address, Bytes))
              return std::move(E);
            return Bytes;
          }};
      auto Prepared = [&]() -> llvm::Expected<KernelInitializerServiceResult> {
        auto Revoke = llvm::make_scope_exit([&] { *ReadActive = false; });
        try {
          return Pending->Prepare(Input);
        } catch (const std::exception &E) {
          return invalid("kernel initializer service failed: " +
                         std::string(E.what()));
        } catch (...) {
          return invalid("kernel initializer service failed");
        }
      }();
      if (!Prepared)
        return Prepared.takeError();
      if (Prepared->Handoff) {
        if (&Call != &Plan.Calls.back() || Call.RequireZeroResult ||
            Prepared->Value || !Prepared->Writes.empty())
          return invalid("kernel initializer handoff cannot prove a return "
                         "status or continue the plan or publish effects");
        Result.Handoff = KernelInitializerResult::Boundary{
            Pending->Entry, std::move(Arguments), {}};
#define NEVERD_X64_REGISTER(Name, DecoderID, BackendID)                        \
  {                                                                            \
    auto Value = CPU.reg(X64Register::Name);                                   \
    if (!Value)                                                                \
      return Value.takeError();                                                \
    Result.Handoff->Registers.emplace(X64Register::Name, *Value);              \
  }
#include "neverd/emulation/X64Registers.def"
#undef NEVERD_X64_REGISTER
        break;
      }
      for (const auto &W : Prepared->Writes)
        if (!Mutable.contains(W.Address, W.Bytes.size()))
          return invalid(
              "kernel initializer service write exceeds declared storage");
      for (const auto &W : Prepared->Writes) {
        if (auto E = CPU.writeBacking(W.Address, W.Bytes))
          return std::move(E);
        Known.add(W.Address, W.Bytes.size());
      }
      auto Return = ABI->readReturnAddress(CPU, *SP);
      auto NextSP = ABI->returnStackPointer(*SP);
      if (!Return || !NextSP)
        return llvm::joinErrors(Return.takeError(), NextSP.takeError());
      if (auto E = CPU.setReg(X64Register::AX, Prepared->Value))
        return std::move(E);
      if (auto E = CPU.setReg(X64Register::SP, *NextSP))
        return std::move(E);
      PC = *Return;
    }
    if (Result.Handoff)
      break; // The caller has not returned; preserve that distinction.
    auto SP = CPU.reg(X64Register::SP);
    auto AX = CPU.reg(X64Register::AX);
    if (!SP || !AX)
      return llvm::joinErrors(SP.takeError(), AX.takeError());
    if (*SP != Frame->ReturnStackPointer)
      return invalid(
          "kernel initializer returned with an invalid stack pointer");
    if (Call.RequireZeroResult && *AX)
      return invalid("kernel initializer returned an unsuccessful status: 0x" +
                     llvm::utohexstr(*AX));
    Result.ReturnValues.push_back(*AX);
  }
  for (const auto &R : Plan.Outputs) {
    if (!Known.contains(R.Address, R.Size))
      return invalid("kernel initializer output contains undefined bytes");
    auto &Output =
        Result.Outputs.emplace_back(R.Address, std::vector<uint8_t>(R.Size));
    if (auto E = CPU.readBacking(Output.Address, Output.Bytes))
      return std::move(E);
  }
  auto IRQL = CPU.reg(X64Register::CR8);
  if (!IRQL)
    return IRQL.takeError();
  Result.FinalIRQL = *IRQL;
  return Result;
}
} // namespace neverd::emulation
