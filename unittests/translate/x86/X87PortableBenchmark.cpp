//===- X87PortableBenchmark.cpp - Scoped portable x87 measurements --------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Measure only the authored portable fixture libraries retained by the frozen
/// recorder. The caller verifies those artifacts before selecting this driver.
//===----------------------------------------------------------------------===//

#include "X87TranslationTestSupport.h"

#include "neverd/translate/NativeTranslationSession.h"
#include "neverd/translate/X86/X87PortableExecution.h"

#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/DynamicLibrary.h"
#include "llvm/Support/JSON.h"
#include "llvm/Support/raw_ostream.h"

#include <charconv>
#include <chrono>
#include <cstdint>
#include <fstream>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

using namespace neverd;
using namespace neverd::translate;

namespace {

constexpr uint32_t Load80 =
    0x28db | X87ContextMemory | (0x28u << X87ContextOriginalModRMShift);
constexpr uint32_t Store80 =
    0x38db | X87ContextMemory | (0x38u << X87ContextOriginalModRMShift);
constexpr uint64_t RefusalInterval = 32;
constexpr uint64_t ReturnSentinel = 0xfedcba9876543210;

struct Loaded {
  llvm::sys::DynamicLibrary Library;
  explicit Loaded(const char *Path, std::string &Error)
      : Library(llvm::sys::DynamicLibrary::getLibrary(Path, &Error)) {}
  ~Loaded() {
    if (Library.isValid())
      llvm::sys::DynamicLibrary::closeLibrary(Library);
  }
};

uint64_t number(std::string_view Text) {
  uint64_t Result = 0;
  const auto Parsed =
      std::from_chars(Text.data(), Text.data() + Text.size(), Result);
  if (Parsed.ec != std::errc{} || Parsed.ptr != Text.data() + Text.size())
    throw std::runtime_error("invalid unsigned decimal argument");
  return Result;
}

uint64_t peakMemoryKiB() {
  std::ifstream Input("/proc/self/status");
  std::string Line;
  while (std::getline(Input, Line)) {
    if (Line.rfind("VmHWM:", 0) != 0)
      continue;
    const auto First = Line.find_first_of("0123456789");
    const auto Last = Line.find_first_not_of("0123456789", First);
    return number(std::string_view(Line).substr(First, Last - First));
  }
  throw std::runtime_error("Linux VmHWM is unavailable");
}

PortableX87StateV1 environment() {
  PortableX87StateV1 State;
  State.EnvironmentKnown = 1;
  State.Image[0] = 0x7f;
  State.Image[1] = 3;
  State.Image[3] = 5u << 3;
  for (unsigned I = 32; I != 160; ++I)
    State.Image[I] = uint8_t(I ^ 0xa5);
  return State;
}

std::unique_ptr<GuestMemoryRuntime> memory(bool NaN = false) {
  auto Logical =
      llvm::cantFail(createZeroedGuestState(GuestArchitecture::X86_64));
  Logical.Memory = {
      {1, MemoryPermission::Read, 0, {0, 0, 0, 0, 0, 0, 0, 0x80, 0xfe, 0x3f}},
      {0x1000, MemoryPermission::Read, 0,
       NaN ? std::vector<uint8_t>{0x23, 1, 0, 0, 0, 0, 0, 0xc0, 0xff, 0x7f}
           : std::vector<uint8_t>{1, 0, 0, 0, 0, 0, 0, 0x80, 0xff, 0x3f}},
      {0x3000, MemoryPermission::Write, 0, std::vector<uint8_t>(12, 0xa5)}};
  return llvm::cantFail(GuestMemoryRuntime::create(Logical));
}

std::string raw(const PortableX87StateV1 &State) {
  return llvm::toHex(
      llvm::ArrayRef(reinterpret_cast<const uint8_t *>(&State), sizeof(State)));
}

using Leaf = uint32_t (*)(uint64_t, uint64_t, uint64_t, void *, uint64_t *);
using Control = uint32_t (*)(uint64_t, uint64_t, uint64_t, uint64_t, void *,
                             uint64_t *);

uint32_t invoke(void *Symbol, unsigned Shape, PortableX87ExecutionV1 &Execution,
                uint64_t &Result) {
  return Shape == 0 ? reinterpret_cast<Leaf>(Symbol)(0x1000, 1, 0x3001,
                                                     &Execution, &Result)
                    : reinterpret_cast<Control>(Symbol)(0x1000, 1, 0x3001, 2,
                                                        &Execution, &Result);
}

} // namespace

int main(int Argc, char **Argv) {
  try {
    if (Argc != 6)
      throw std::runtime_error("expected MODE ITERATIONS SHAPE MIXED LIBRARY");
    const std::string Mode = Argv[1];
    const uint64_t Iterations = number(Argv[2]);
    const uint64_t Shape = number(Argv[3]);
    const uint64_t Mixed = number(Argv[4]);
    if (!Iterations || Shape > 2 || Mixed > 1 ||
        (Mode != "generated" && Mode != "transport" && Mode != "snapshot" &&
         Mode != "session") ||
        (Mode != "generated" &&
         (Shape || Mixed || std::string_view(Argv[5]) != "-")))
      throw std::runtime_error("invalid measurement mode or fixture selection");
    if (Mode == "session" && !test::nativeLinuxAArch64())
      throw std::runtime_error("session timing requires native Linux AArch64");
    std::unique_ptr<Loaded> Library;
    void *Symbol = nullptr;
    if (Mode == "generated") {
      std::string Error;
      Library = std::make_unique<Loaded>(Argv[5], Error);
      if (!Library->Library.isValid())
        throw std::runtime_error(Error);
      Symbol =
          Library->Library.getAddressOfSymbol("nvd_portable_x87_0x1000_v1");
      if (!Symbol)
        throw std::runtime_error("authored fixture symbol is absent");
    }
    const auto Initial = environment();
    auto State = Initial;
    auto RejectedState = Initial;
    auto Memory = memory();
    auto RejectedMemory = memory(true);
    PortableX87ExecutionV1 Execution(State, *Memory);
    PortableX87ExecutionV1 RejectedExecution(RejectedState, *RejectedMemory);
    auto Logical = test::fixture();
    llvm::cantFail(setPortableX87GuestStateV1(Logical, test::X87ID, Initial));
    uint64_t Completed = 0, Refused = 0, Return = ReturnSentinel;
    uint64_t GeneratedBytes = 0;
    std::string LogicalWire;
    const auto Run = [&](uint64_t Index, bool Count) {
      if (Mode == "generated") {
        const bool Refusal =
            Mixed && (Index % RefusalInterval == RefusalInterval - 1);
        if (Refusal)
          RejectedState = Initial;
        Return = ReturnSentinel;
        const uint32_t Status =
            invoke(Symbol, static_cast<unsigned>(Shape),
                   Refusal ? RejectedExecution : Execution, Return);
        if (Status != (Refusal ? static_cast<uint32_t>(
                                     PortableX87StepStatusV1::UnsupportedValue)
                               : 0) ||
            Return != (Refusal ? ReturnSentinel : (5u << 11) + (Shape == 1)))
          throw std::runtime_error(
              "generated fixture changed its exact result or refusal");
        if (Count)
          Refusal ? ++Refused : ++Completed;
      } else if (Mode == "transport") {
        if (nvd_x87_v1_step(&Execution, Load80, 0x1000, 0x1000) ||
            nvd_x87_v1_step(&Execution, Store80, 0x1002, 0x3001))
          throw std::runtime_error(
              "raw transport refused its admitted operands");
        if (Count)
          ++Completed;
      } else if (Mode == "snapshot") {
        auto Runtime =
            llvm::cantFail(createRuntimeGuestStateX86_64X87V1(Logical));
        Runtime.Integer.GPR[0] = Index;
        llvm::cantFail(applyRuntimeGuestStateX86_64X87V1(Runtime, Logical));
        if (Count)
          ++Completed;
      } else {
        auto Session = llvm::cantFail(NativeTranslationSessionV1::create(
            test::options(true), test::fixture()));
        auto Result = Session->run();
        if (!Result)
          throw std::runtime_error(llvm::toString(Result.takeError()));
        if (Result->Exit.Reason != TranslationStopReason::Returned ||
            Result->GuestInstructions != 7 || Result->BlocksTranslated != 6)
          throw std::runtime_error(
              "native session did not complete the authored program");
        if (Count) {
          ++Completed;
          GeneratedBytes += Result->GeneratedCodeBytes;
        }
        Logical = Session->state();
      }
    };
    // One untimed invocation for each applicable path; mixed refusals also
    // warm their independent context. The timed loop checks every status.
    Run(0, false);
    if (Mixed)
      Run(RefusalInterval - 1, false);
    const auto Start = std::chrono::steady_clock::now();
    for (uint64_t I = 0; I != Iterations; ++I)
      Run(I, true);
    const auto Nanoseconds =
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now() - Start)
            .count();
    auto Output = Memory->snapshotMemoryRegions()[2].Bytes;
    auto ObservedState = State;
    if (Mode == "generated" || Mode == "transport") {
      const uint64_t Expected =
          Mode == "transport" ? 0x8000000000000001 : 0xc000000000000001;
      if (Output.front() != 0xa5 || Output.back() != 0xa5 ||
          llvm::support::endian::read64le(Output.data() + 1) != Expected ||
          llvm::support::endian::read16le(Output.data() + 9) != 0x3fff ||
          State.Image[4] != 0 ||
          llvm::support::endian::read16le(State.Image.data() + 2) != (5u << 11))
        throw std::runtime_error(
            "measured memory or x87 state disagrees with the fixture");
    } else {
      const auto Runtime =
          llvm::cantFail(createRuntimeGuestStateX86_64X87V1(Logical));
      if (Mode == "snapshot" && (Runtime.Integer.GPR[0] != Iterations - 1 ||
                                 raw(Runtime.X87) != raw(Initial)))
        throw std::runtime_error("snapshot transport changed preserved state");
      ObservedState = Runtime.X87;
      Output = Logical.Memory[2].Bytes;
      if (Mode == "session")
        Return = Runtime.Integer.GPR[0];
      LogicalWire = llvm::toHex(llvm::cantFail(serializeGuestState(Logical)));
    }
    llvm::outs()
        << llvm::json::Value(llvm::json::Object{
               {"protocol", 1},
               {"mode", Mode},
               {"shape", Shape},
               {"mixed", bool(Mixed)},
               {"iterations", Iterations},
               {"completed", Completed},
               {"refused", Refused},
               {"fallback_available", false},
               {"fallback_calls", 0},
               {"elapsed_ns", Nanoseconds},
               {"peak_rss_kib", peakMemoryKiB()},
               {"generated_object_bytes", GeneratedBytes},
               {"state", raw(ObservedState)},
               {"memory", llvm::toHex(Output)},
               {"return", Mode == "generated" || Mode == "session"
                              ? llvm::json::Value(llvm::utohexstr(Return))
                              : llvm::json::Value(nullptr)},
               {"refusal_state", Mixed ? raw(RejectedState) : ""},
               {"refusal_memory",
                Mixed ? llvm::toHex(
                            RejectedMemory->snapshotMemoryRegions()[2].Bytes)
                      : ""},
               {"logical_wire", LogicalWire}})
        << '\n';
    return 0;
  } catch (const std::exception &Error) {
    llvm::errs() << Error.what() << '\n';
    return 1;
  }
}
