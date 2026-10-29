//===- SessionFunctionDiscoveryTests.cpp - Recovered function extents
//------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "SessionImpl.h"
#include "SessionTestAccess.h"
#include "gtest/gtest.h"

#include "neverd/sdk/NeverDCAPI.h"

#include "llvm/ADT/ScopeExit.h"
#include "llvm/Object/ELFTypes.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/JSON.h"

#include <algorithm>
#include <atomic>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <future>
#include <memory>

namespace {
std::string takeString(const char *Text) {
  const std::string Result = Text ? Text : "";
  neverd_free_string(Text);
  return Result;
}

std::string makeNativeELF(bool AArch64, uint64_t Base = 0x400000,
                          std::string_view TrailingCode = {},
                          std::string_view Body = {}) {
  using ELF = llvm::object::ELF64LE;
  using namespace llvm::ELF;
  std::string Code = !Body.empty() ? std::string(Body)
                     : AArch64
                         ? std::string("\xe0\x00\x80\x52\xc0\x03\x5f\xd6", 8)
                         : std::string("\xb8\x07\x00\x00\x00\xc3", 6);
  Code += TrailingCode;
  const size_t CodeOffset = sizeof(ELF::Ehdr) + sizeof(ELF::Phdr);
  std::string Bytes(CodeOffset + Code.size(), '\0');
  ELF::Ehdr Header{};
  std::memcpy(Header.e_ident, ElfMagic, sizeof(ElfMagic) - 1);
  Header.e_ident[EI_CLASS] = ELFCLASS64;
  Header.e_ident[EI_DATA] = ELFDATA2LSB;
  Header.e_ident[EI_VERSION] = EV_CURRENT;
  Header.e_type = ET_EXEC;
  Header.e_machine = AArch64 ? EM_AARCH64 : EM_X86_64;
  Header.e_version = EV_CURRENT;
  Header.e_entry = Base + CodeOffset;
  Header.e_phoff = sizeof(ELF::Ehdr);
  Header.e_ehsize = sizeof(ELF::Ehdr);
  Header.e_phentsize = sizeof(ELF::Phdr);
  Header.e_phnum = 1;
  Header.e_shentsize = sizeof(ELF::Shdr);
  ELF::Phdr Segment{};
  Segment.p_type = PT_LOAD;
  Segment.p_flags = PF_R | PF_X;
  Segment.p_vaddr = Base;
  Segment.p_paddr = Base;
  Segment.p_filesz = Bytes.size();
  Segment.p_memsz = Bytes.size();
  Segment.p_align = 0x1000;
  std::memcpy(Bytes.data(), &Header, sizeof(Header));
  std::memcpy(Bytes.data() + sizeof(Header), &Segment, sizeof(Segment));
  Bytes.replace(CodeOffset, Code.size(), Code);
  return Bytes;
}

class SessionFunctionDiscovery : public ::testing::Test {
protected:
  neverd_session_t Session = nullptr;
  std::filesystem::path Directory;
  void SetUp() override {
    llvm::SmallString<128> Path;
    ASSERT_FALSE(
        llvm::sys::fs::createUniqueDirectory("neverd-discovery", Path));
    Directory = Path.c_str();
    Session = neverd_session_create();
    ASSERT_NE(Session, nullptr);
  }
  void TearDown() override {
    neverd_session_destroy(Session);
    std::error_code Error;
    std::filesystem::remove_all(Directory, Error);
  }
  std::string write(std::string_view Name, std::string_view Bytes) {
    const auto File = Directory / Name;
    std::ofstream Stream(File, std::ios::binary);
    Stream.write(Bytes.data(), static_cast<std::streamsize>(Bytes.size()));
    return File.string();
  }
};

TEST_F(SessionFunctionDiscovery,
       NativeDiscoveryBeforeAnalysisPublishesRecoveredExtents) {
  for (bool AArch64 : {false, true}) {
    for (bool Lazy : {false, true}) {
      for (neverd_va_t Base : {0x400000ULL, 0xffff800000400000ULL}) {
        SCOPED_TRACE(AArch64);
        SCOPED_TRACE(Lazy);
        SCOPED_TRACE(Base);
        const std::string Path =
            write("discovered.elf", makeNativeELF(AArch64, Base));
        ASSERT_EQ(neverd_session_load(Session, Path.c_str()), 1);
        const auto Entry = neverd_session_entry_addr(Session);
        ASSERT_EQ(neverd_session_discover_functions(Session), 1);
        auto &State = *reinterpret_cast<neverd::sdk::Session *>(Session);
        ASSERT_EQ(State.Functions.size(), 1u);
        EXPECT_EQ(State.Functions.front().LinkageOrigin,
                  neverd::NameOrigin::Synthesized);
        EXPECT_EQ(State.Functions.front().ResolvedOrigin,
                  neverd::NameOrigin::Synthesized);
        ASSERT_EQ(neverd_func_size(Session, 0), 0);
        const auto Name = takeString(neverd_func_name(Session, 0));
        if (Lazy)
          ASSERT_FALSE(takeString(neverd_ir_low(Session, Entry)).empty());
        else
          ASSERT_EQ(neverd_session_analyze(Session), 1);
        EXPECT_EQ(neverd_func_entry(Session, 0), Entry);
        EXPECT_EQ(neverd_func_size(Session, 0), AArch64 ? 8 : 6);
        EXPECT_EQ(takeString(neverd_func_name(Session, 0)), Name);
        EXPECT_EQ(State.Functions.front().LinkageOrigin,
                  neverd::NameOrigin::Synthesized);
        EXPECT_EQ(State.Functions.front().ResolvedOrigin,
                  neverd::NameOrigin::Synthesized);
        ASSERT_FALSE(takeString(neverd_ir_med(Session, Entry)).empty());
        ASSERT_EQ(neverd_session_analyze(Session), 1);
        EXPECT_EQ(neverd_func_size(Session, 0), AArch64 ? 8 : 6);
      }
    }
  }
}

TEST_F(SessionFunctionDiscovery, CallsAtTheEntryAreSweptInStep) {
  // The image's one PT_LOAD maps its headers too, and a sweep from them
  // decodes `00 e8`, the last header byte with the opcode of the entry's
  // first call, as one instruction.  Swept from the entry, every call is
  // read: main calls a, c and d; a calls b; b calls c.
  constexpr char Code[] = "\xe8\x0b\x00\x00\x00"     // main: call a
                          "\xe8\x12\x00\x00\x00"     // call c
                          "\xe8\x13\x00\x00\x00"     // call d
                          "\xc3"                     // ret
                          "\xe8\x01\x00\x00\x00\xc3" // a: call b; ret
                          "\xe8\x01\x00\x00\x00\xc3" // b: call c; ret
                          "\xb8\x07\x00\x00\x00\xc3" // c: mov eax, 7; ret
                          "\xc3";                    // d: ret
  const std::string Path = write(
      "chain.elf", makeNativeELF(false, 0x400000, {},
                                 std::string_view(Code, sizeof(Code) - 1)));
  ASSERT_EQ(neverd_session_load(Session, Path.c_str()), 1);
  const auto Entry = neverd_session_entry_addr(Session);
  ASSERT_EQ(neverd_session_discover_functions(Session), 5);
  for (const uint64_t Offset : {0x0, 0x10, 0x16, 0x1c, 0x22})
    EXPECT_GE(neverd_func_find_by_addr(Session, Entry + Offset), 0) << Offset;
}

TEST_F(SessionFunctionDiscovery, AddressedAssemblyKeepsOneCompleteNativeBody) {
  using namespace neverd;
  constexpr char Body[] = "\xf3\x0f\x1e\xfa\x50" // endbr64; push rax
                          "\x48\xb8\xcd\xcc\xcc\xcc\xcc\xcc\xcc\xcc"
                          "\x48\xf7\xe2"     // mul rdx
                          "\x48\x83\xe0\x07" // and rax,7
                          "\x58\xc3";        // pop rax; ret
  std::string Code(512, '\xcc');
  Code.replace(0, sizeof(Body) - 1, Body, sizeof(Body) - 1);
  constexpr char Namer[] = "\x48\x8d\x05\xf9\xfe\xff\xff\xc3";
  Code.replace(0x100, sizeof(Namer) - 1, Namer, sizeof(Namer) - 1);
  auto Bytes = makeNativeELF(false, 0x400000, {}, Code);
  llvm::object::ELF64LE::Ehdr Header;
  std::memcpy(&Header, Bytes.data(), sizeof(Header));
  const uint64_t Target = Header.e_entry;
  Header.e_entry = Target + 0x100;
  std::memcpy(Bytes.data(), &Header, sizeof(Header));
  const auto Path = write("addressed-assembly.elf", Bytes);
  ASSERT_EQ(neverd_session_load(Session, Path.c_str()), 1);
  auto &State = *static_cast<sdk::Session *>(Session);
  auto Guess = Symbol::makeFunc(Target + 18);
  Guess.IsBoundaryGuess = true;
  State.Img.Symbols.push_back(Guess);
  ASSERT_EQ(neverd_session_discover_functions(Session), 2);
  const int Index = neverd_func_find_by_addr(Session, Target);
  ASSERT_GE(Index, 0);
  EXPECT_EQ(neverd_func_entry(Session, Index), Target);
  for (int I = 0; I < neverd_func_count(Session); ++I)
    EXPECT_TRUE(neverd_func_entry(Session, I) == Target ||
                neverd_func_entry(Session, I) == Target + 0x100);
  ASSERT_FALSE(takeString(neverd_ir_low(Session, Target)).empty());
  EXPECT_EQ(neverd_func_size(Session, Index), sizeof(Body) - 1);
  EXPECT_EQ(neverd_session_discover_functions(Session), 2);

  const auto Comparison = neverd_comparison_create();
  ASSERT_TRUE(Comparison);
  auto DestroyComparison =
      llvm::scope_exit([&] { neverd_comparison_destroy(Comparison); });
  EXPECT_EQ(neverd_comparison_run(Comparison, Session, Session,
                                  R"({"discover":true})", nullptr, nullptr),
            1)
      << takeString(neverd_comparison_last_error(Comparison));
  auto Result =
      llvm::json::parse(takeString(neverd_comparison_export_json(Comparison)));
  ASSERT_TRUE(static_cast<bool>(Result)) << llvm::toString(Result.takeError());
  const auto *Matches = Result->getAsObject()->getArray("matches");
  ASSERT_TRUE(Matches);
  EXPECT_EQ(Matches->size(), 2u);
  for (const auto &Match : *Matches)
    EXPECT_EQ(Match.getAsObject()->getBoolean("analysis_complete"), true);
}

TEST_F(SessionFunctionDiscovery,
       CompletedDiscoveryRefutesOnlyUnconfirmedLoaderGuesses) {
  using namespace neverd;
  for (bool Async : {false, true}) {
    SCOPED_TRACE(Async);
    // Bytes beyond the entry are invalid instruction starts. Only the
    // provisional guess must disappear; stated and user-owned entries stay
    // available for inspection even when their bodies cannot be decoded.
    const auto Path = write(
        "guesses.elf", makeNativeELF(false, 0x400000, std::string(64, '\x06')));
    ASSERT_EQ(neverd_session_load(Session, Path.c_str()), 1);
    auto &State = *static_cast<sdk::Session *>(Session);
    const va_t Entry = neverd_session_entry_addr(Session);
    const va_t Rejected = Entry + 8, Stated = Entry + 16, Renamed = Entry + 24,
               Created = Entry + 32;
    for (va_t Address : {Rejected, Stated, Renamed, Created}) {
      Symbol Symbol = Symbol::makeFunc(Address);
      Symbol.IsBoundaryGuess = Address != Stated;
      State.Img.Symbols.push_back(Symbol);
    }
    State.Renames[Renamed] = "analyst_name";
    State.FunctionEdits[Created] = true;
    sdk::SessionTestAccess::resetFunctions(State);
    ASSERT_GE(neverd_func_find_by_addr(Session, Rejected), 0);
    if (Async) {
      ASSERT_EQ(neverd_session_discover_functions_step(Session), 0);
      ASSERT_TRUE(State.FunctionDiscovery);
      State.FunctionDiscovery->Result.wait();
      ASSERT_EQ(neverd_session_discover_functions_step(Session), 1);
    } else {
      ASSERT_GE(neverd_session_discover_functions(Session), 0);
    }
    ASSERT_TRUE(State.DiscoveredFunctions);
    EXPECT_FALSE(llvm::any_of(*State.DiscoveredFunctions, [&](const auto &F) {
      return F.first == Rejected;
    }));
    for (bool Rebuild : {false, true}) {
      if (Rebuild)
        sdk::SessionTestAccess::resetFunctions(State);
      EXPECT_EQ(neverd_func_find_by_addr(Session, Rejected), -1);
      for (va_t Kept : {Entry, Stated, Renamed, Created})
        EXPECT_GE(neverd_func_find_by_addr(Session, Kept), 0) << Kept;
      EXPECT_EQ(takeString(neverd_func_name(
                    Session, neverd_func_find_by_addr(Session, Renamed))),
                "analyst_name");
    }
    // The symbol remains diagnostic evidence in the image, not a mutated
    // loader fact. Creating it explicitly restores the user-owned function.
    EXPECT_TRUE(llvm::any_of(State.Img.Symbols, [&](const Symbol &Symbol) {
      return Symbol.Addr == Rejected && Symbol.IsBoundaryGuess;
    }));
    ASSERT_EQ(neverd_func_create(Session, Rejected), 0);
    EXPECT_GE(neverd_func_find_by_addr(Session, Rejected), 0);
  }
}

TEST_F(SessionFunctionDiscovery,
       RecoveredExtentsSurviveSelectingAnotherFunction) {
  // An x86-64 entry that calls the function after it: discovery lists both
  // without extents, and each complete lift gives its function one.
  // Selecting the second keeps the first's, and the list read from the
  // image stays the one a rebuild would make.
  const std::string EntryCode("\xe8\x01\x00\x00\x00\xc3", 6);
  const std::string CalleeCode("\xb8\x07\x00\x00\x00\xc3", 6);
  const std::string Path =
      write("calls.elf", makeNativeELF(false, 0x400000, CalleeCode, EntryCode));
  ASSERT_EQ(neverd_session_load(Session, Path.c_str()), 1);
  const auto Entry = neverd_session_entry_addr(Session);
  const auto Callee = Entry + EntryCode.size();
  ASSERT_EQ(neverd_session_discover_functions(Session), 2);
  const int EntryIndex = neverd_func_find_by_addr(Session, Entry);
  const int CalleeIndex = neverd_func_find_by_addr(Session, Callee);
  ASSERT_GE(EntryIndex, 0);
  ASSERT_GE(CalleeIndex, 0);
  EXPECT_EQ(neverd_func_size(Session, EntryIndex), 0);
  EXPECT_EQ(neverd_func_size(Session, CalleeIndex), 0);

  // As warming leaves a session: its analysis reads the current signature
  // features, so each view prepared selects its function rather than
  // starting the analysis over.
  auto &State = *reinterpret_cast<neverd::sdk::Session *>(Session);
  State.PipelineFeatureGeneration = State.SigDB.featureGeneration();
  ASSERT_EQ(neverd_prepare_function_view(Session, Entry, "low"), 1);
  EXPECT_EQ(State.OnlyFunctionEntries, std::set<neverd::va_t>{Entry});
  EXPECT_EQ(neverd_func_size(Session, EntryIndex), 6);
  ASSERT_EQ(neverd_prepare_function_view(Session, Callee, "low"), 1);
  EXPECT_EQ(State.OnlyFunctionEntries, std::set<neverd::va_t>{Callee});
  EXPECT_EQ(neverd_func_find_by_addr(Session, Entry), EntryIndex);
  EXPECT_EQ(neverd_func_size(Session, CalleeIndex), 6);
  EXPECT_EQ(neverd_func_size(Session, EntryIndex), 6);
  EXPECT_TRUE(State.functionListCurrent());

  // A rebuild from the image keeps them too.
  neverd::sdk::SessionTestAccess::resetFunctions(State);
  EXPECT_EQ(neverd_func_size(Session, EntryIndex), 6);
  EXPECT_EQ(neverd_func_size(Session, CalleeIndex), 6);
}

TEST_F(SessionFunctionDiscovery,
       NativeRecoveryKeepsKnownExtentsAndDeletedEntries) {
  for (bool MissingEntry : {false, true}) {
    auto &State = *reinterpret_cast<neverd::sdk::Session *>(Session);
    State.Loaded = true;
    State.Img.Arch = neverd::Arch::X64;
    State.PipeRan = true;
    State.PipeResult.Success = true;
    State.NativeFunctionsSynchronized = false;
    State.Functions = {{0x1000, 0, "discovered"}, {0x2000, 23, "stated"}};
    State.functionListChanged();
    State.FunctionEdits[0x4000] = false;
    State.PipeResult.LowFuncs.clear();
    for (auto Entry : {0x1000, 0x2000, 0x4000}) {
      neverd::LowFunc F;
      F.Entry = Entry;
      F.OriginalSize = 8;
      State.PipeResult.LowFuncs.push_back(std::move(F));
    }
    if (MissingEntry) {
      neverd::LowFunc F;
      F.Entry = 0x3000;
      F.OriginalSize = 12;
      State.PipeResult.LowFuncs.push_back(std::move(F));
    }
    ASSERT_EQ(neverd_func_count(Session), MissingEntry ? 3 : 2);
    EXPECT_EQ(neverd_func_size(Session, 0), 8);
    EXPECT_EQ(neverd_func_size(Session, 1), 23);
    EXPECT_EQ(neverd_func_find_by_addr(Session, 0x4000), -1);
    if (MissingEntry)
      EXPECT_EQ(neverd_func_size(Session, 2), 12);
  }
}

TEST_F(SessionFunctionDiscovery, CooperativeStopKeepsInventoryAndAllowsRetry) {
  for (bool AArch64 : {false, true}) {
    SCOPED_TRACE(AArch64);
    const std::string Padding(1024 * 1024, AArch64 ? '\0' : '\x90');
    const auto Path =
        write("cancel.elf", makeNativeELF(AArch64, 0x400000, Padding));
    ASSERT_EQ(neverd_session_load(Session, Path.c_str()), 1);
    auto &State = *static_cast<neverd::sdk::Session *>(Session);
    const auto Count = neverd_func_count(Session);
    const auto Revision = State.FunctionListRevision;
    const auto Verified = State.Img.VerifiedFunctionEntries;
    std::atomic<unsigned> Probes{0};
    const auto Cancel = [](void *Context) -> int {
      return static_cast<std::atomic<unsigned> *>(Context)->fetch_add(1) >= 64;
    };
    EXPECT_EQ(
        neverd_session_discover_functions_cancellable(Session, Cancel, &Probes),
        -2);
    EXPECT_GT(Probes.load(), 64u);
    EXPECT_FALSE(State.DiscoveredFunctions);
    EXPECT_FALSE(State.FunctionDiscovery);
    EXPECT_EQ(State.FunctionListRevision, Revision);
    EXPECT_EQ(State.Img.VerifiedFunctionEntries, Verified);
    EXPECT_EQ(neverd_func_count(Session), Count);
    EXPECT_NE(takeString(neverd_last_error(Session)).find("cancelled"),
              std::string::npos);
    std::unique_ptr<void, decltype(&neverd_session_destroy)> Fresh(
        neverd_session_create(), neverd_session_destroy);
    ASSERT_EQ(neverd_session_load(Fresh.get(), Path.c_str()), 1);
    const auto Complete = neverd_session_discover_functions(Fresh.get());
    ASSERT_GE(Complete, 0);
    ASSERT_EQ(neverd_session_discover_functions(Session), Complete);
    const auto &Expected = *static_cast<neverd::sdk::Session *>(Fresh.get());
    EXPECT_EQ(State.Functions, Expected.Functions);
    EXPECT_EQ(State.Img.VerifiedFunctionEntries,
              Expected.Img.VerifiedFunctionEntries);
    EXPECT_TRUE(State.DiscoveredFunctions);
    EXPECT_TRUE(takeString(neverd_last_error(Session)).empty());
  }
}

TEST_F(SessionFunctionDiscovery,
       CooperativeStopJoinsPendingSnapshotBeforeRetry) {
  const auto Path =
      write("pending.elf",
            makeNativeELF(false, 0x400000, std::string(1024 * 1024, '\x90')));
  ASSERT_EQ(neverd_session_load(Session, Path.c_str()), 1);
  const auto Count = neverd_func_count(Session);
  auto &State = *static_cast<neverd::sdk::Session *>(Session);
  ASSERT_EQ(neverd_session_discover_functions_step(Session), 0);
  ASSERT_TRUE(State.FunctionDiscovery);
  const auto Token = State.FunctionDiscovery->Cancel;
  const auto Revision = State.FunctionListRevision;
  EXPECT_EQ(neverd_session_discover_functions_cancellable(
                Session, [](void *) { return 1; }, nullptr),
            -2);
  EXPECT_TRUE(Token->load());
  EXPECT_FALSE(State.FunctionDiscovery);
  EXPECT_FALSE(State.DiscoveredFunctions);
  EXPECT_EQ(State.FunctionListRevision, Revision);
  EXPECT_EQ(neverd_func_count(Session), Count);
  std::unique_ptr<void, decltype(&neverd_session_destroy)> Fresh(
      neverd_session_create(), neverd_session_destroy);
  ASSERT_EQ(neverd_session_load(Fresh.get(), Path.c_str()), 1);
  const auto Complete = neverd_session_discover_functions(Fresh.get());
  ASSERT_GE(Complete, 0);
  ASSERT_EQ(neverd_session_discover_functions(Session), Complete);
  const auto &Expected = *static_cast<neverd::sdk::Session *>(Fresh.get());
  EXPECT_EQ(State.Functions, Expected.Functions);
  EXPECT_EQ(State.Img.VerifiedFunctionEntries,
            Expected.Img.VerifiedFunctionEntries);
  EXPECT_TRUE(State.DiscoveredFunctions);
}

TEST_F(SessionFunctionDiscovery, ClosingSessionStopsItsOwnedDiscovery) {
  const auto Path =
      write("closing.elf",
            makeNativeELF(false, 0x400000, std::string(1024 * 1024, '\x90')));
  ASSERT_EQ(neverd_session_load(Session, Path.c_str()), 1);
  ASSERT_EQ(neverd_session_discover_functions_step(Session), 0);
  auto &State = *static_cast<neverd::sdk::Session *>(Session);
  ASSERT_TRUE(State.FunctionDiscovery);
  const auto Token = State.FunctionDiscovery->Cancel;
  neverd_session_destroy(Session);
  Session = nullptr;
  EXPECT_TRUE(Token->load());
}

TEST_F(SessionFunctionDiscovery,
       NonNativeSynchronousQueryDoesNotJoinAnEarlierJob) {
  const auto Native = write("earlier.elf", makeNativeELF(false));
  const auto Contract = write("current.evm", "6001600055");
  ASSERT_EQ(neverd_session_load(Session, Native.c_str()), 1);
  auto &State = *static_cast<neverd::sdk::Session *>(Session);
  std::promise<neverd::sdk::Session::FunctionDiscoveryResult> Pending;
  State.FunctionDiscovery.emplace(neverd::sdk::Session::FunctionDiscoveryJob{
      State.FunctionListRevision, Pending.get_future()});
  ASSERT_EQ(neverd_session_load(Session, Contract.c_str()), 1);
  ASSERT_TRUE(State.FunctionDiscovery);
  // A query for the replacement input must not wait for an unrelated job.
  // It requests retirement, then a later poll drains the completed future.
  EXPECT_TRUE(State.FunctionDiscovery->Cancel->load());
  EXPECT_GE(neverd_session_discover_functions(Session), 0);
  EXPECT_TRUE(State.FunctionDiscovery);
  Pending.set_value({});
  EXPECT_EQ(neverd_session_discover_functions_step(Session), 1);
  EXPECT_FALSE(State.FunctionDiscovery);
}

} // namespace
