//===- cpu-bench.cpp - Checked ARM64 CPU execution measurements ---------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "neverd/emulation/CPU.h"

#include "llvm/Support/Endian.h"
#include "llvm/Support/JSON.h"
#include "llvm/Support/raw_ostream.h"

#include <charconv>
#include <chrono>
#include <cstdlib>
#include <string_view>
#include <vector>

using namespace neverd::emulation;
namespace {
using Clock = std::chrono::steady_clock;
llvm::ExitOnError Checked("neverd-cpu-bench: ");
constexpr uint64_t Code = 0x10000, Data = 0x20000;
constexpr uint64_t PageSize = 4096;
constexpr unsigned WideLoopAdds = 4096, WideLoopIterations = 16;

void require(bool Condition, const char *Message) {
  if (!Condition) {
    llvm::errs() << "neverd-cpu-bench: " << Message << '\n';
    std::exit(EXIT_FAILURE);
  }
}

// Original fixed encodings. The final NOP is an observer stop marker, never
// executed. All branches below are PC-relative; each loop has 1000 iterations.
constexpr uint32_t IntegerLoop[] = {
    0xd2800000, 0xd2807d01, // mov x0, #0; mov x1, #1000
    0x91000400, 0xf1000421, // add x0, x0, #1; subs x1, x1, #1
    0x54ffffc1, 0xd503201f, // b.ne -8; stop
};
constexpr uint32_t Branch[] = {
    0xd2800000, 0xd2807d01, 0xd2800002, // x0=0; x1=1000; x2=0
    0x91000400, 0x36000060,             // add x0, x0, #1; tbz w0, #0, +12
    0x91000c42, 0x14000002,             // add x2, x2, #3; b +8
    0x91001442, 0xf1000421,             // add x2, x2, #5; subs x1, x1, #1
    0x54ffff41, 0xd503201f,             // b.ne -24; stop
};
constexpr uint32_t Memory[] = {
    0xd2800000, 0xd2807d01, // x0=0; x1=1000
    0xf9000040, 0xf9400043, // str x0, [x2]; ldr x3, [x2]
    0x91000460, 0xf1000421, // add x0, x3, #1; subs x1, x1, #1
    0x54ffff81, 0xd503201f, // b.ne -16; stop
};
constexpr uint32_t TLSCall[] = {
    0xd2800000, 0xd2807d01, // x0=0; x1=1000
    0x94000004, 0xf1000421, // bl +16; subs x1, x1, #1
    0x54ffffc1, 0x14000005, // b.ne -8; b +20 (stop)
    0xd53bd042, 0xf9400043, // mrs x2, tpidr_el0; ldr x3, [x2]
    0x8b030000, 0xd65f03c0, // add x0, x0, x3; ret
    0xd503201f,             // stop
};
constexpr uint32_t Switching[] = {0x91000400,
                                  0xd503201f}; // add x0, x0, #1; stop

// A repeated working set larger than the small-loop instruction cache. Its
// arithmetic oracle is independent of transport cache capacity and layout.
std::vector<uint32_t> wideLoop() {
  std::vector<uint32_t> Words(WideLoopAdds + 5, 0x91000400); // add x0, x0, #1
  Words[0] = 0xd2800000;                                     // mov x0, #0
  Words[1] = 0xd2800001 | (WideLoopIterations << 5); // mov x1, #iterations
  Words[WideLoopAdds + 2] = 0xf1000421;              // subs x1, x1, #1
  const uint32_t Displacement = uint32_t(-int32_t(WideLoopAdds + 1));
  Words[WideLoopAdds + 3] = 0x54000001 | ((Displacement & 0x7ffff) << 5);
  Words.back() = 0xd503201f; // observer stop marker
  return Words;
}

struct Program {
  const char *Name;
  llvm::ArrayRef<uint32_t> Words;
  uint64_t Expected, Instructions;
};

std::unique_ptr<ExecutionBackend> create(ExecutionBackendKind Kind,
                                         bool UserMode) {
  return std::move(Checked(createExecutionBackend(
                               Kind,
                               UserMode ? ExecutionContract::CheckedUserAArch64
                                        : ExecutionContract::CheckedAArch64,
                               16 * PageSize, GuestArchitecture::AArch64))
                       .CPU);
}

class Workload {
public:
  const Program P;
  std::unique_ptr<ExecutionBackend> CPU;
  Workload(ExecutionBackendKind Kind, bool UserMode, Program P)
      : P(P), CPU(create(Kind, UserMode)) {
    const unsigned User = UserMode ? UserAccessible : 0;
    const uint64_t CodeBytes =
        (P.Words.size() * 4 + PageSize - 1) & ~(PageSize - 1);
    require(CodeBytes <= Data - Code, "benchmark code overlaps data");
    Checked(CPU->map(Code, CodeBytes, Read | Write | Execute | User));
    Checked(CPU->map(Data, PageSize, Read | Write | User));
    std::vector<uint8_t> Bytes(P.Words.size() * 4);
    for (size_t I = 0; I < P.Words.size(); ++I)
      llvm::support::endian::write32le(Bytes.data() + I * 4, P.Words[I]);
    Checked(CPU->write(Code, Bytes));
    Checked(CPU->writeInteger(Data, 3, 8));
    Checked(CPU->setReg(AArch64Register::TPIDR_EL0, Data));
    BackendHooks Hooks;
    Hooks.Instruction = [this](uint64_t PC, uint32_t) {
      if (PC == end())
        CPU->stop();
      else
        ++Count;
    };
    Checked(CPU->installHooks(std::move(Hooks)));
  }
  void prepare() {
    Count = 0;
    Checked(CPU->setReg(AArch64Register::X2, Data));
  }
  void execute() { Exit = Checked(CPU->runUntilExit(Code, 30000000)); }
  void verify(uint64_t Expected) {
    require(Exit.Kind == ExecutionExitKind::Stopped && !CPU->fault(),
            "execution did not stop cleanly at the marker");
    require(Checked(CPU->reg(AArch64Register::X0)) == Expected,
            "wrong accumulator");
    require(Checked(CPU->reg(AArch64Register::PC)) == end(), "wrong final PC");
    require(Count == P.Instructions, "wrong instruction count");
    if (std::string_view(P.Name) == "branch")
      require(Checked(CPU->reg(AArch64Register::X2)) == 4000,
              "wrong branch accumulation");
    if (std::string_view(P.Name) == "memory")
      require(Checked(CPU->readInteger(Data, 8)) == 999, "wrong final RAM");
  }

private:
  uint64_t Count = 0;
  ExecutionExit Exit;
  uint64_t end() const { return Code + P.Words.size() * 4 - 4; }
};

void report(ExecutionBackendKind Kind, bool UserMode, const char *Name,
            unsigned Index, bool Warmup, uint64_t Instructions,
            Clock::time_point Start, Clock::time_point End) {
  llvm::outs() << llvm::json::Value(llvm::json::Object{
                      {"schema_version", 1},
                      {"architecture", "aarch64"},
                      {"contract", UserMode ? "checked-user" : "checked"},
                      {"backend", executionBackendName(Kind)},
                      {"workload", Name},
                      {"sample", Index},
                      {"warmup", Warmup},
                      {"guest_instructions", Instructions},
                      {"elapsed_ns",
                       std::chrono::duration_cast<std::chrono::nanoseconds>(
                           End - Start)
                           .count()}})
               << '\n';
  llvm::outs().flush();
}

unsigned number(std::string_view Text) {
  unsigned Value = 0;
  auto [End, E] =
      std::from_chars(Text.data(), Text.data() + Text.size(), Value);
  require(E == std::errc() && End == Text.data() + Text.size() && Value <= 1000,
          "sample/warmup count must be an integer from 0 to 1000");
  return Value;
}
} // namespace

int main(int Argc, char **Argv) {
  auto Kind = ExecutionBackendKind::HVF;
  bool UserMode = false;
  bool Wide = false;
  unsigned Samples = 7, Warmup = 1;
  for (int I = 1; I < Argc; ++I) {
    const std::string_view Arg = Argv[I];
    if (Arg == "--help") {
      llvm::outs() << "neverd-cpu-bench [--backend hvf|unicorn] [--samples N] "
                      "[--warmup N] [--contract checked|checked-user]\n"
                      "[--wide-loop]\nChecked ARM64 workloads; native HVF "
                      "requires an ARM64 macOS host. JSON lines include "
                      "warmups. No native-entry instrumentation.\n";
      return 0;
    }
    if (Arg == "--wide-loop") {
      Wide = true;
      continue;
    }
    require(I + 1 < Argc, "missing option value");
    const std::string_view Value = Argv[++I];
    if (Arg == "--backend") {
      require(Value == "hvf" || Value == "unicorn", "unknown backend");
      Kind = Value == "hvf" ? ExecutionBackendKind::HVF
                            : ExecutionBackendKind::Unicorn;
    } else if (Arg == "--samples") {
      Samples = number(Value);
    } else if (Arg == "--warmup") {
      Warmup = number(Value);
    } else if (Arg == "--contract") {
      require(Value == "checked" || Value == "checked-user",
              "unknown contract");
      UserMode = Value == "checked-user";
    } else {
      require(false, "unknown option");
    }
  }
  require(Samples != 0, "at least one measured sample is required");
  for (unsigned I = 0; I < Samples + Warmup; ++I) {
    const auto Start = Clock::now();
    auto CPU = create(Kind, UserMode);
    const auto End = Clock::now();
    report(Kind, UserMode, "initialization", I, I < Warmup, 0, Start, End);
  }
  const auto WideLoop = Wide ? wideLoop() : std::vector<uint32_t>{};
  for (auto P :
       {Program{"integer", IntegerLoop, 1000, 3002},
        Program{"branch", Branch, 1000, 5503},
        Program{"memory", Memory, 1000, 5002},
        Program{"tls_call", TLSCall, 3000, 7003},
        Program{"wide_loop", WideLoop, WideLoopAdds * WideLoopIterations,
                2 + WideLoopIterations * (WideLoopAdds + 2)}}) {
    if (P.Words.empty())
      continue; // The optional wide loop is absent from the default inventory.
    Workload W(Kind, UserMode, P);
    for (unsigned I = 0; I < Samples + Warmup; ++I) {
      W.prepare();
      const auto Start = Clock::now();
      W.execute();
      const auto End = Clock::now();
      W.verify(P.Expected);
      report(Kind, UserMode, P.Name, I, I < Warmup, P.Instructions, Start, End);
    }
  }
  Workload A(Kind, UserMode, {"switch", Switching, 0, 1});
  Workload B(Kind, UserMode, {"switch", Switching, 0, 1});
  for (unsigned I = 0; I < Samples + Warmup; ++I) {
    Checked(A.CPU->setReg(AArch64Register::X0, 7));
    Checked(B.CPU->setReg(AArch64Register::X0, 101));
    // Includes the public API/context-switch checks between the 512 steps.
    const auto Start = Clock::now();
    for (unsigned N = 1; N <= 256; ++N) {
      A.prepare();
      A.execute();
      A.verify(7 + N);
      B.prepare();
      B.execute();
      B.verify(101 + N);
    }
    report(Kind, UserMode, "two_cpu_switch", I, I < Warmup, 512, Start,
           Clock::now());
  }
}
