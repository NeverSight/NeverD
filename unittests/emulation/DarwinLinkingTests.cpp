//===- DarwinLinkingTests.cpp - Independent execution fixup fixtures -----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file Exact binding, rebase, malformed metadata and preparation bounds.
#include "DarwinTestImage.h"
#include "gtest/gtest.h"
#include "os/darwin/kernel/DarwinEntropy.h"
#include "os/darwin/kernel/DarwinFiles.h"
#include "os/darwin/kernel/DarwinMemory.h"
#include "os/darwin/process/DarwinCallbacks.h"
#include "os/darwin/process/DarwinLibSystem.h"
#include "os/darwin/process/DarwinProcess.h"

#include "neverd/loader/MachO/MachOProgramLinking.h"

#include <algorithm>
#include <array>
#include <set>

namespace neverd::emulation {
namespace {
using namespace llvm::MachO;
struct LinkingImage : darwin_test::Image {
  size_t Data, Symbols, Dynamic, Dependency, Fixups;
  static constexpr uint64_t Base = 0x100000000ULL;
  static constexpr size_t Bind = 33024, Rebase = 33088, Chain = 33152;
  explicit LinkingImage(bool Chained, bool X64 = false)
      : darwin_test::Image(X64) {
    u32(Thread, LC_MAIN);
    u32(Thread + 4, 24);
    u64(Thread + 8, 4096);
    u64(Thread + 16, 0);
    CommandsEnd = Thread + 24;
    u32(20, CommandsEnd - 32);
    Bytes.resize(49152, 0);
    u32(24, MH_DYLDLINK | MH_TWOLEVEL | MH_NOUNDEFS | MH_PIE);
    Data = command(LC_SEGMENT_64, 72);
    std::copy_n("__DATA", 6, Bytes.data() + Data + 8);
    u64(Data + 24, Base + 16384);
    u64(Data + 32, 16384);
    u64(Data + 40, 16384);
    u64(Data + 48, 16384);
    u32(Data + 56, 3);
    u32(Data + 60, 3);
    Dependency = command(LC_LOAD_DYLIB, 56);
    u32(Dependency + 8, 24);
    const char Library[] = "/usr/lib/libSystem.B.dylib";
    std::copy(std::begin(Library), std::end(Library),
              Bytes.begin() + Dependency + 24);
    Symbols = command(LC_SYMTAB, 24);
    u32(Symbols + 8, 32768);
    u32(Symbols + 12, 1);
    u32(Symbols + 16, 32784);
    u32(Symbols + 20, 8);
    u32(32768, 1);
    Bytes[32772] = 1; // External undefined symbol.
    Bytes[32775] = 1; // Ordinary dylib ordinal one.
    std::copy_n("\0_write\0", 8, Bytes.data() + 32784);
    Dynamic = command(LC_DYSYMTAB, 80);
    u32(Dynamic + 28, 1);
    if (!Chained) {
      Fixups = command(LC_DYLD_INFO_ONLY, 48);
      const uint8_t Binding[] = {0x11, 0x40, '_',  'w',  'r', 'i',  't',
                                 'e',  0,    0x51, 0x72, 0,   0x90, 0};
      std::copy(std::begin(Binding), std::end(Binding), Bytes.begin() + Bind);
      const uint8_t Rebasing[] = {0x11, 0x22, 8, 0x51, 0};
      std::copy(std::begin(Rebasing), std::end(Rebasing),
                Bytes.begin() + Rebase);
      u32(Fixups + 8, Rebase);
      u32(Fixups + 12, sizeof(Rebasing));
      u32(Fixups + 16, Bind);
      u32(Fixups + 20, sizeof(Binding));
      u64(16392, Base + 4096);
    } else {
      Fixups = command(LC_DYLD_CHAINED_FIXUPS, 16);
      u32(Fixups + 8, Chain);
      u32(Fixups + 12, 88);
      u32(Chain + 4, 32);
      u32(Chain + 8, 72);
      u32(Chain + 12, 76);
      u32(Chain + 16, 1);
      u32(Chain + 20, 1);
      u32(Chain + 32, 3);
      u32(Chain + 44, 16);
      u32(Chain + 48, 24);
      Bytes[Chain + 53] = 0x40; // 16384-byte chain page.
      Bytes[Chain + 54] = 2;    // Generic64 pointer format.
      u64(Chain + 56, 16384);
      Bytes[Chain + 68] = 1; // One page, start zero.
      u32(Chain + 72, 1);    // Import zero, provider one, name offset zero.
      std::copy_n("_write\0", 7, Bytes.data() + Chain + 76);
      u64(16384, (1ULL << 63) | (2ULL << 51));
      u64(16392, Base + 4096);
    }
  }
};
void appendULEB(std::vector<uint8_t> &Bytes, uint64_t Value) {
  do {
    uint8_t Byte = Value & 127;
    Value >>= 7;
    Bytes.push_back(Byte | (Value ? 128 : 0));
  } while (Value);
}
std::vector<uint8_t> lazyProgram(uint64_t Offset, llvm::StringRef Name,
                                 unsigned Fields = 7) {
  std::vector<uint8_t> Bytes;
  if (Fields & 1) {
    Bytes.push_back(0x72);
    appendULEB(Bytes, Offset);
  }
  if (Fields & 2)
    Bytes.push_back(0x11);
  if (Fields & 4) {
    Bytes.push_back(0x40);
    Bytes.insert(Bytes.end(), Name.begin(), Name.end());
    Bytes.push_back(0);
  }
  Bytes.push_back(0x90);
  Bytes.push_back(0);
  return Bytes;
}
struct LazyImage : LinkingImage {
  size_t Lazy, Rebases, Ordinary;
  explicit LazyImage(llvm::ArrayRef<llvm::StringRef> Calls = {"_write",
                                                              "_getpid"},
                     bool Binder = true, bool X64 = false)
      : LinkingImage(false, X64) {
    const size_t DataSize = ((Calls.size() + 1) * 8 + 16383) & ~size_t(16383);
    const size_t End = 16384 + DataSize;
    Rebases = End + 256;
    Ordinary = End + 288;
    Lazy = End + 512;
    Bytes.resize(std::max<size_t>(49152, Lazy + Calls.size() * 40 + 64), 0);
    u64(Data + 32, DataSize);
    u64(Data + 48, DataSize);
    std::set<std::string> Names;
    for (auto Name : Calls)
      Names.insert(Name.str());
    if (Binder)
      Names.insert("dyld_stub_binder");
    u32(Symbols + 8, End);
    u32(Symbols + 12, Names.size());
    u32(Symbols + 16, End + 128);
    u32(Dynamic + 28, Names.size());
    size_t Index = 0, StringOffset = 1;
    Bytes[End + 128] = 0;
    for (const auto &Name : Names) {
      const auto At = End + Index++ * 16;
      std::fill_n(Bytes.begin() + At, 16, 0);
      u32(At, StringOffset);
      Bytes[At + 4] = uint8_t(N_EXT) | uint8_t(N_UNDF);
      Bytes[At + 6] = Name == "dyld_stub_binder" ? 0 : 1;
      Bytes[At + 7] = 1;
      std::copy(Name.begin(), Name.end(),
                Bytes.begin() + End + 128 + StringOffset);
      StringOffset += Name.size();
      Bytes[End + 128 + StringOffset++] = 0;
    }
    u32(Symbols + 20, StringOffset);
    std::vector<uint8_t> RebaseBytes{0x11, 0x22, 0, 0x60};
    appendULEB(RebaseBytes, Calls.size());
    RebaseBytes.push_back(0);
    std::copy(RebaseBytes.begin(), RebaseBytes.end(), Bytes.begin() + Rebases);
    u32(Fixups + 8, Rebases);
    u32(Fixups + 12, RebaseBytes.size());
    std::vector<uint8_t> Programs;
    for (size_t I = 0; I < Calls.size(); ++I) {
      u64(16384 + I * 8, Base + 4096);
      auto Program = lazyProgram(I * 8, Calls[I]);
      Programs.insert(Programs.end(), Program.begin(), Program.end());
    }
    setLazy(Programs);
    u32(Fixups + 16, Ordinary);
    u32(Fixups + 20, 0);
    if (Binder) {
      auto Binding = lazyProgram(Calls.size() * 8, "dyld_stub_binder");
      Binding.insert(Binding.begin(), 0x51);
      std::copy(Binding.begin(), Binding.end(), Bytes.begin() + Ordinary);
      u32(Fixups + 20, Binding.size());
    }
  }
  void setLazy(llvm::ArrayRef<uint8_t> Programs) {
    if (Lazy + Programs.size() > Bytes.size())
      Bytes.resize(Lazy + Programs.size());
    std::copy(Programs.begin(), Programs.end(), Bytes.begin() + Lazy);
    u32(Fixups + 32, Lazy);
    u32(Fixups + 36, Programs.size());
  }
};
llvm::Expected<MachOProgramLinking> linking(const LinkingImage &I) {
  return readMachOProgramLinking(I.Bytes, 1 << 20,
                                 [] { return llvm::Error::success(); });
}
void refused(const LinkingImage &I, llvm::StringRef Reason) {
  SCOPED_TRACE(Reason.str());
  auto R = linking(I);
  ASSERT_FALSE(bool(R));
  const auto Message = llvm::toString(R.takeError());
  EXPECT_NE(Message.find(Reason.str()), std::string::npos) << Message;
}
TEST(DarwinLinking, IndependentLazyProgramsNormalizeOnlyTheirExactRebaseSlots) {
  for (bool X64 : {false, true}) {
    LazyImage I({"_write", "_getpid"}, true, X64);
    auto Original = I.Bytes;
    auto R = linking(I);
    ASSERT_TRUE(bool(R)) << llvm::toString(R.takeError());
    ASSERT_EQ(R->Fixups.size(), 3u);
    for (unsigned N = 0; N != 2; ++N) {
      const auto &F = R->Fixups[N];
      EXPECT_EQ(F.Address, I.Base + 16384 + N * 8);
      EXPECT_TRUE(F.Bind);
      EXPECT_EQ(F.Origin, MachOProgramFixupOrigin::ClassicLazyBind);
      EXPECT_EQ(F.Symbol, N ? "_getpid" : "_write");
      EXPECT_EQ(F.LibraryOrdinal, 1);
      EXPECT_EQ(F.Addend, 0);
    }
    EXPECT_EQ(R->Fixups[2].Symbol, "dyld_stub_binder");
    EXPECT_EQ(R->Fixups[2].Origin, MachOProgramFixupOrigin::ClassicBind);
    EXPECT_EQ(I.Bytes, Original);
    auto Programs = lazyProgram(0, "_write");
    auto Second = lazyProgram(8, "_getpid");
    Programs.insert(Programs.end(), Second.begin(), Second.end());
    Programs.insert(Programs.end(), 8, 0);
    I.setLazy(Programs);
    auto Padded = linking(I);
    ASSERT_TRUE(bool(Padded)) << llvm::toString(Padded.takeError());
    EXPECT_EQ(Padded->Fixups.size(), 3u);
  }
}
TEST(DarwinLinking, LazyProgramsCannotInheritFieldsOrChangeStateAfterBinding) {
  for (unsigned Missing : {1u, 2u, 4u}) {
    LazyImage I;
    auto First = lazyProgram(0, "_write");
    auto Second = lazyProgram(8, "_getpid", 7 ^ Missing);
    First.insert(First.end(), Second.begin(), Second.end());
    I.setLazy(First);
    refused(I, "requires its own destination, ordinal and symbol");
  }
  for (uint8_t After : {0x11, 0x40, 0x51, 0x60, 0x72, 0x90}) {
    LazyImage I({"_write"});
    auto Program = lazyProgram(0, "_write");
    Program.insert(Program.end() - 1, After);
    I.setLazy(Program);
    refused(I, "immediately by DONE");
  }
  LazyImage MissingDone({"_write"});
  auto Program = lazyProgram(0, "_write");
  Program.pop_back();
  MissingDone.setLazy(Program);
  refused(MissingDone, "unterminated lazy");
  LazyImage EmptyProgram;
  auto First = lazyProgram(0, "_write");
  First.push_back(0);
  auto Second = lazyProgram(8, "_getpid");
  First.insert(First.end(), Second.begin(), Second.end());
  EmptyProgram.setLazy(First);
  refused(EmptyProgram, "incomplete lazy");
}
TEST(DarwinLinking, LazyProgramsRefuseMalformedAndUnsupportedFields) {
  const std::vector<std::pair<std::vector<uint8_t>, const char *>> Prefixes = {
      {{0x52}, "pointer type"},
      {{0x60, 1}, "nonzero lazy"},
      {{0x60, 0x7f}, "nonzero lazy"},
      {{0x30}, "unsupported lazy"},
      {{0x41, '_', 'w', 'r', 'i', 't', 'e', 0}, "symbol flags"},
      {{0xd0}, "unsupported lazy"},
      {{0x80, 0}, "unsupported lazy"},
      {{0x21, 1}, "ordinal opcode"}};
  for (const auto &[Prefix, Reason] : Prefixes) {
    LazyImage I({"_write"});
    auto Program = Prefix;
    auto Valid = lazyProgram(0, "_write");
    Program.insert(Program.end(), Valid.begin(), Valid.end());
    I.setLazy(Program);
    refused(I, Reason);
  }
  for (uint8_t Op : {0x20, 0x60, 0x72}) {
    LazyImage I({"_write"});
    I.setLazy({Op, 0x80});
    refused(I, Op == 0x60 ? "SLEB128" : "ULEB128");
    std::vector<uint8_t> Overflow{Op};
    Overflow.insert(Overflow.end(), 12, 0xff);
    Overflow.push_back(0);
    I.setLazy(Overflow);
    refused(I, Op == 0x60 ? "SLEB128" : "ULEB128");
  }
  LazyImage Ordinal({"_write"});
  auto Program = lazyProgram(0, "_write");
  Program[2] = 0x10;
  Ordinal.setLazy(Program);
  refused(Ordinal, "ordinary dependency ordinal");
  LazyImage TruncatedName({"_write"});
  TruncatedName.setLazy({0x40, '_', 'w'});
  refused(TruncatedName, "unterminated symbol");
  LazyImage ExplicitDefaults({"_write"});
  Program = {0x51, 0x60, 0};
  auto Rest = lazyProgram(0, "_write");
  Program.insert(Program.end(), Rest.begin(), Rest.end());
  ExplicitDefaults.setLazy(Program);
  auto R = linking(ExplicitDefaults);
  ASSERT_TRUE(bool(R)) << llvm::toString(R.takeError());
}
TEST(DarwinLinking, LazyNormalizationCannotHideAnyOtherDestinationCollision) {
  LazyImage NonExecutable;
  NonExecutable.u64(16384, NonExecutable.Base + 16384);
  refused(NonExecutable, "local executable bytes");
  LazyImage DuplicateRebase;
  const uint8_t RebaseBytes[] = {0x11, 0x22, 0, 0x51, 0x22, 0, 0x51, 0};
  std::copy(std::begin(RebaseBytes), std::end(RebaseBytes),
            DuplicateRebase.Bytes.begin() + DuplicateRebase.Rebases);
  DuplicateRebase.u32(DuplicateRebase.Fixups + 12, sizeof(RebaseBytes));
  refused(DuplicateRebase, "duplicate");
  LazyImage DuplicateLazy({"_write"});
  auto Program = lazyProgram(0, "_write");
  auto Copy = Program;
  Program.insert(Program.end(), Copy.begin(), Copy.end());
  DuplicateLazy.setLazy(Program);
  refused(DuplicateLazy, "duplicate");
  LazyImage OrdinaryCollision({"_write"});
  OrdinaryCollision.u32(OrdinaryCollision.Fixups + 12, 0);
  Program = lazyProgram(0, "dyld_stub_binder");
  Program.insert(Program.begin(), 0x51);
  std::copy(Program.begin(), Program.end(),
            OrdinaryCollision.Bytes.begin() + OrdinaryCollision.Ordinary);
  OrdinaryCollision.u32(OrdinaryCollision.Fixups + 20, Program.size());
  refused(OrdinaryCollision, "duplicate");
  LazyImage Unaligned({"_write"});
  Unaligned.setLazy(lazyProgram(4, "_write"));
  refused(Unaligned, "aligned");
}
TEST(DarwinLinking, LazyRawRecordLimitsSurviveNormalization) {
  std::vector<llvm::StringRef> Names(32768, "_write");
  LazyImage Exact(Names, false);
  auto R = readMachOProgramLinking(Exact.Bytes, 64 << 20,
                                   [] { return llvm::Error::success(); });
  ASSERT_TRUE(bool(R)) << llvm::toString(R.takeError());
  EXPECT_EQ(R->Fixups.size(), 32768u); // 65,536 raw rebase/bind records.
  LazyImage OneMore(Names, true);      // Adds one ordinary binder record.
  auto TooMany = readMachOProgramLinking(OneMore.Bytes, 64 << 20,
                                         [] { return llvm::Error::success(); });
  ASSERT_FALSE(bool(TooMany));
  EXPECT_NE(llvm::toString(TooMany.takeError()).find("too many fixup records"),
            std::string::npos);
}
TEST(DarwinLinking, LazyMetadataAndDeadlinesCannotPublishPartialNormalization) {
  LazyImage I;
  const auto Original = I.Bytes;
  unsigned Checks = 0;
  auto Full = readMachOProgramLinking(I.Bytes, 1 << 20, [&] {
    ++Checks;
    return llvm::Error::success();
  });
  ASSERT_TRUE(bool(Full)) << llvm::toString(Full.takeError());
  auto Exact = readMachOProgramLinking(I.Bytes, Full->MetadataBytes,
                                       [] { return llvm::Error::success(); });
  ASSERT_TRUE(bool(Exact)) << llvm::toString(Exact.takeError());
  auto Under = readMachOProgramLinking(I.Bytes, Full->MetadataBytes - 1,
                                       [] { return llvm::Error::success(); });
  ASSERT_FALSE(bool(Under));
  EXPECT_NE(llvm::toString(Under.takeError()).find("byte limit"),
            std::string::npos);
  for (unsigned Stop : {1u, Checks / 2, Checks}) {
    unsigned Seen = 0;
    auto Expired =
        readMachOProgramLinking(I.Bytes, 1 << 20, [&]() -> llvm::Error {
          if (++Seen == Stop)
            return llvm::make_error<darwin_model::PreparationTimeout>(
                GuestArchitecture::AArch64, I.Base + 4096);
          return llvm::Error::success();
        });
    ASSERT_FALSE(bool(Expired));
    auto E = Expired.takeError();
    EXPECT_TRUE(E.isA<darwin_model::PreparationTimeout>());
    llvm::consumeError(std::move(E));
  }
  EXPECT_EQ(I.Bytes, Original);
}
TEST(DarwinLinking,
     BinderAdmissionRequiresStrongOrdinaryBindingAndActualLazyCalls) {
  auto Load = [](const LinkingImage &I) {
    darwin_test::TemporaryImage File(I);
    return darwin_model::loadImage(File.path(), darwin_model::macOSProfile(),
                                   {});
  };
  auto Reject = [&](const LinkingImage &I, llvm::StringRef Reason) {
    auto R = Load(I);
    ASSERT_FALSE(bool(R));
    EXPECT_NE(llvm::toString(R.takeError()).find(Reason.str()),
              std::string::npos);
  };
  LazyImage BinderOnly({}, true);
  BinderOnly.setLazy({0, 0});
  Reject(BinderOnly, "libSystem import: dyld_stub_binder");
  LazyImage LazyBinder({"dyld_stub_binder"}, false);
  Reject(LazyBinder, "libSystem import: dyld_stub_binder");
  LinkingImage ChainedBinder(true);
  constexpr char Binder[] = "dyld_stub_binder";
  std::copy(std::begin(Binder), std::end(Binder),
            ChainedBinder.Bytes.begin() + 32785);
  ChainedBinder.u32(ChainedBinder.Symbols + 20, sizeof(Binder) + 1);
  std::copy(std::begin(Binder), std::end(Binder),
            ChainedBinder.Bytes.begin() + ChainedBinder.Chain + 76);
  ChainedBinder.u32(ChainedBinder.Fixups + 12, 76 + sizeof(Binder));
  Reject(ChainedBinder, "libSystem import: dyld_stub_binder");
  LazyImage Unknown({"_malloc"});
  Reject(Unknown, "libSystem import: _malloc");
  LazyImage Provider;
  Provider.Bytes[Provider.Dependency + 24] = 'x';
  Reject(Provider, "provider");
  LazyImage Weak;
  const auto Symbols =
      llvm::support::endian::read32le(Weak.Bytes.data() + Weak.Symbols + 8);
  Weak.Bytes[Symbols + 2 * 16 + 6] |= N_WEAK_REF;
  Weak.Bytes[Weak.Ordinary + 4] = 0x41;
  Reject(Weak, "libSystem import: dyld_stub_binder");
  LazyImage Addend;
  auto Ordinary = lazyProgram(16, "dyld_stub_binder");
  Ordinary.insert(Ordinary.begin(), 0x51);
  Ordinary.insert(Ordinary.end() - 2, {0x60, 1});
  std::copy(Ordinary.begin(), Ordinary.end(),
            Addend.Bytes.begin() + Addend.Ordinary);
  Addend.u32(Addend.Fixups + 20, Ordinary.size());
  Reject(Addend, "libSystem import: dyld_stub_binder");
  LazyImage Good;
  auto R = Load(Good);
  ASSERT_TRUE(bool(R)) << llvm::toString(R.takeError());
  EXPECT_EQ(R->Fixups.size(), 3u);
  EXPECT_EQ(R->Imports.size(), 3u);
  EXPECT_EQ(R->Fixups[0].Value,
            darwin_model::value::LibSystemGate +
                unsigned(darwin_model::LibSystemGateKind::Write) *
                    darwin_model::value::LibSystemGateStride);
  EXPECT_EQ(R->Imports.at(R->Fixups[2].Value),
            darwin_model::LibSystemGateKind::StubBinder);
}
TEST(DarwinLinking,
     CallbackStoragePreservesPreviouslyAdmittedBaseAndTLSBudgets) {
  using darwin_model::DarwinCallbacks;
  // The previous 64-bit owner had 104 bytes before its one vector. Anchor
  // that admitted footprint, including debug STL vector bookkeeping, rather
  // than deriving the expected old budget from the new owner's sizeof.
  static_assert(sizeof(void *) == 8);
  const uint64_t Vector = sizeof(std::vector<std::array<uint64_t, 2>>);
  const uint64_t Base = 104 + Vector;
  EXPECT_EQ(DarwinCallbacks::metadataBytes(false), Base);
  EXPECT_EQ(DarwinCallbacks::metadataBytes(true), Base + 1024 * 16);
  EXPECT_EQ(DarwinCallbacks::metadataBytes(false, true),
            Base + 2 * Vector + 1024 * 24);
  EXPECT_EQ(DarwinCallbacks::metadataBytes(true, true),
            Base + 2 * Vector + 1024 * (16 + 24));
}
TEST(DarwinLinking,
     RegistrationOnlyImagesReserveOriginalTargetsAndExactBudgets) {
  const std::vector<std::vector<llvm::StringRef>> Calls = {
      {"_write"},        {"_exit"},
      {"__tlv_atexit"},  {"_atexit"},
      {"___cxa_atexit"}, {"__tlv_atexit", "_atexit", "___cxa_atexit"}};
  for (bool X64 : {false, true}) {
    for (const auto &Names : Calls) {
      SCOPED_TRACE(Names.front().str());
      LazyImage I(Names, true, X64);
      darwin_test::TemporaryImage File(I);
      ProcessOptions Options;
      Options.StackSize = 16384;
      auto R = darwin_model::loadImage(File.path(),
                                       darwin_model::macOSProfile(), Options);
      ASSERT_TRUE(bool(R)) << llvm::toString(R.takeError());
      const bool TLS =
          std::find(Names.begin(), Names.end(), "__tlv_atexit") != Names.end();
      const bool Global =
          std::find(Names.begin(), Names.end(), "_atexit") != Names.end() ||
          std::find(Names.begin(), Names.end(), "___cxa_atexit") != Names.end();
      EXPECT_TRUE(R->Initializers.empty());
      EXPECT_EQ(R->HasTLSCallbacks, TLS);
      EXPECT_EQ(R->HasGlobalCallbacks, Global);
      EXPECT_EQ(!R->ExecutableSpans.empty(), TLS || Global);
      Options.MemoryLimit = R->MetadataBytes + R->Plan.MappedBytes +
                            Options.StackSize + 3 * R->Memory.PageSize;
      auto Exact = darwin_model::loadImage(
          File.path(), darwin_model::macOSProfile(), Options);
      ASSERT_TRUE(bool(Exact)) << llvm::toString(Exact.takeError());
      --Options.MemoryLimit;
      auto Under = darwin_model::loadImage(
          File.path(), darwin_model::macOSProfile(), Options);
      ASSERT_FALSE(bool(Under));
      llvm::consumeError(Under.takeError());
    }
  }
}
TEST(DarwinLinking,
     GlobalAdmissionDoesNotInventGuardOrSelectiveFinalizationCalls) {
  for (const char *Name :
       {"___cxa_guard_acquire", "___cxa_guard_release", "___cxa_guard_abort",
        "___cxa_finalize", "___cxa_finalize_ranges", "_dlopen", "_dlclose"}) {
    LazyImage I({Name});
    darwin_test::TemporaryImage File(I);
    auto R =
        darwin_model::loadImage(File.path(), darwin_model::macOSProfile(), {});
    ASSERT_FALSE(bool(R)) << Name;
    EXPECT_NE(llvm::toString(R.takeError())
                  .find(std::string("libSystem import: ") + Name),
              std::string::npos);
  }
}
TEST(DarwinLinking, BinderRefusalPrecedesCFrameDecodingAndPreservesErrno) {
  using namespace darwin_model;
  for (bool X64 : {false, true}) {
    LazyImage I({"_write", "_getpid"}, true, X64);
    darwin_test::TemporaryImage File(I);
    auto Image = loadImage(File.path(), macOSProfile(), {});
    ASSERT_TRUE(bool(Image)) << llvm::toString(Image.takeError());
    auto Physical = PhysicalMemory::create(1 << 20);
    ASSERT_TRUE(bool(Physical)) << llvm::toString(Physical.takeError());
    auto Space = AddressSpace::create(*Physical, 1 << 20);
    ASSERT_TRUE(bool(Space)) << llvm::toString(Space.takeError());
    llvm::cantFail(prepareLibSystem(**Space, *Image));
    const auto Errno = value::LibSystemGate + Image->Memory.PageSize;
    llvm::cantFail((*Space)->writeInteger(Errno, 91, 4));
    auto Backend =
        createExecutionBackend(ExecutionBackendKind::Unicorn,
                               X64 ? ExecutionContract::CheckedUserX64
                                   : ExecutionContract::CheckedUserAArch64,
                               *Space, Image->Architecture);
    ASSERT_TRUE(bool(Backend)) << llvm::toString(Backend.takeError());
    auto &CPU = *Backend->CPU;
    const auto SP = X64 ? CPURegister::X64SP : CPURegister::AArch64SP;
    llvm::cantFail(CPU.writeRegister(SP, {UINT64_MAX, 0}));
    ProcessOptions Options;
    DarwinMemory Memory(**Space, Image->Memory, Options);
    DarwinFiles Files(**Space, Options.DarwinFiles);
    DarwinEntropy Entropy(Options.DarwinSystem);
    ProcessResult Result{ProcessProfile::MacOSMachO64, Image->Architecture,
                         ExecutionBackendKind::Unicorn, "binder refusal"};
    ServiceRequest Request{
        X64 ? ServiceRequestKind::X64Syscall : ServiceRequestKind::AArch64SVC,
        Image->Fixups[2].Value, 0, uint16_t(value::LibSystemSVC)};
    auto R = invokeLibSystem(CPU, Memory, Files, Entropy, *Image,
                             LibSystemGateKind::StubBinder, Request, Options,
                             Result);
    ASSERT_FALSE(bool(R));
    EXPECT_EQ(llvm::toString(R.takeError()), diagnostic::LibSystemResolver);
    EXPECT_TRUE(Result.NativeCalls.empty());
    EXPECT_TRUE(Result.Services.empty());
    EXPECT_EQ(llvm::cantFail((*Space)->readInteger(Errno, 4)), 91u);
    EXPECT_EQ(llvm::cantFail(CPU.readRegister(SP))[0], UINT64_MAX);
  }
}
TEST(DarwinLinking, ExecutingTheAdmittedBinderGateFailsWithoutACallRecord) {
  for (bool X64 : {false, true}) {
    LazyImage I({"_write", "_getpid"}, true, X64);
    if (X64) {
      // xor %esp,%esp; jmp *binder_slot(%rip). There is no C return frame.
      const uint8_t Code[] = {0x31, 0xe4, 0xff, 0x25};
      std::copy(std::begin(Code), std::end(Code), I.Bytes.begin() + 4096);
      I.u32(4100, 16384 + 16 - 4104);
    } else {
      // mov x0,#0; mov sp,x0; adrp x16,data; ldr x16,[x16,#16]; br x16.
      I.u32(4096, 0xd2800000);
      I.u32(4100, 0x9100001f);
      I.u32(4104, 0xf0000010);
      I.u32(4108, 0xf9400a10);
      I.u32(4112, 0xd61f0200);
    }
    darwin_test::TemporaryImage File(I);
    ProcessOptions Options;
    Options.Backend = ExecutionBackendKind::Unicorn;
    auto R = darwin_model::runProcess(File.path(), darwin_model::macOSProfile(),
                                      Options);
    ASSERT_TRUE(bool(R)) << llvm::toString(R.takeError());
    EXPECT_EQ(R->Stop, ProcessStopReason::RuntimeFailure);
    EXPECT_EQ(R->Diagnostic, darwin_model::diagnostic::LibSystemResolver);
    EXPECT_EQ(R->Events, 1u);
    EXPECT_TRUE(R->NativeCalls.empty());
    EXPECT_TRUE(R->Services.empty());
    EXPECT_TRUE(R->StandardOutput.empty());
    EXPECT_FALSE(R->ExitStatus);
  }
}
TEST(DarwinLinking, ClassicAndChainedRetainExactOriginalFixupFacts) {
  for (bool Chain : {false, true})
    for (bool X64 : {false, true}) {
      LinkingImage I(Chain, X64);
      const auto Original = I.Bytes;
      auto R = linking(I);
      ASSERT_TRUE(bool(R)) << llvm::toString(R.takeError());
      ASSERT_EQ(R->Dependencies.size(), 1u);
      EXPECT_EQ(R->Dependencies[0].Command, uint32_t(LC_LOAD_DYLIB));
      EXPECT_EQ(R->Dependencies[0].Name, "/usr/lib/libSystem.B.dylib");
      ASSERT_EQ(R->Fixups.size(), 2u);
      const auto &B = R->Fixups[Chain ? 0 : 1];
      const auto &L = R->Fixups[Chain ? 1 : 0];
      EXPECT_TRUE(B.Bind);
      EXPECT_EQ(B.Address, I.Base + 16384);
      EXPECT_EQ(B.Symbol, "_write");
      EXPECT_EQ(B.LibraryOrdinal, 1);
      EXPECT_FALSE(B.WeakImport);
      EXPECT_EQ(B.Addend, 0);
      EXPECT_FALSE(L.Bind);
      EXPECT_EQ(L.Address, I.Base + 16392);
      EXPECT_EQ(L.Target, I.Base + 4096);
      EXPECT_GT(R->MetadataBytes, 0u);
      EXPECT_EQ(I.Bytes, Original);
    }
}
TEST(DarwinLinking, RejectsIncompleteMalformedAndAmbiguousClassicBindings) {
  LinkingImage Missing(false);
  Missing.u32(Missing.Fixups + 20, 13);
  refused(Missing, "unterminated");
  LinkingImage LEB(false);
  LEB.Bytes[LEB.Bind + 11] = 0x80;
  LEB.u32(LEB.Fixups + 20, 12);
  refused(LEB, "ULEB128");
  LinkingImage Opcode(false);
  Opcode.Bytes[Opcode.Bind + 12] = 0xd0;
  refused(Opcode, "opcode");
  LinkingImage Duplicate(false);
  Duplicate.Bytes[Duplicate.Bind + 13] = 0x72;
  Duplicate.Bytes[Duplicate.Bind + 14] = 0;
  Duplicate.Bytes[Duplicate.Bind + 15] = 0x90;
  Duplicate.u32(Duplicate.Fixups + 20, 17);
  refused(Duplicate, "duplicate");
  LinkingImage Ordinal(false);
  Ordinal.Bytes[Ordinal.Bind] = 0x12;
  refused(Ordinal, "ordinal");
  LinkingImage BindType(false);
  BindType.Bytes[BindType.Bind + 9] =
      0x11; // No SET_TYPE in an ordinary stream.
  refused(BindType, "pointer type");
  LinkingImage RebaseType(false);
  RebaseType.Bytes[RebaseType.Rebase] = 0x40; // ADD_ADDR_IMM_SCALED zero.
  refused(RebaseType, "pointer type");
  LinkingImage Weak(false);
  Weak.u32(Weak.Fixups + 24, Weak.Bind + 32);
  Weak.u32(Weak.Fixups + 28, 1);
  refused(Weak, "weak binding");
  LinkingImage Unknown(false);
  Unknown.Bytes[Unknown.Bind + 2] = 'x';
  refused(Unknown, "undefined symbol");
}
TEST(DarwinLinking, FixupStorageAndTargetsRequireExactMappedAuthority) {
  for (bool Chain : {false, true}) {
    LinkingImage WritableText(Chain);
    WritableText.u32(WritableText.Data + 56, 7);
    refused(WritableText, "non-executable");
    LinkingImage Unmapped(Chain);
    Unmapped.u64(16392, 0x4000);
    refused(Unmapped, "local mapped");
    LinkingImage Truncated(Chain);
    Truncated.u64(Truncated.Data + 48, 4);
    refused(Truncated, "file storage");
    LinkingImage Relocations(Chain);
    Relocations.u32(Relocations.Dynamic + 64, 33400);
    Relocations.u32(Relocations.Dynamic + 68, 1);
    refused(Relocations, "relocations");
    LinkingImage Hidden(Chain);
    Hidden.u32(Hidden.Dynamic + 28, 0);
    refused(Hidden, "declared range");
  }
}
TEST(DarwinLinking, RejectsUnsupportedAndMalformedChainedRecords) {
  LinkingImage Ordinal(true);
  Ordinal.u64(16384, (1ULL << 63) | 1);
  refused(Ordinal, "pointer import");
  LinkingImage Authenticated(true);
  Authenticated.Bytes[Authenticated.Chain + 54] = 1;
  refused(Authenticated, "chained segment");
  LinkingImage Overlap(true);
  Overlap.u64(16384, (1ULL << 63) | (1ULL << 51));
  refused(Overlap, "aligned");
  LinkingImage Page(true);
  Page.u64(16384, (1ULL << 63) | (0xfffULL << 51));
  refused(Page, "page");
  LinkingImage Reserved(true);
  Reserved.u64(16384, (1ULL << 63) | (1ULL << 32));
  refused(Reserved, "pointer import");
  LinkingImage Both(true);
  Both.command(LC_DYLD_INFO_ONLY, 48);
  refused(Both, "mixed");
}
TEST(DarwinLinking, OffsetRebasesAndUnsignedPointerAddendsKeepTheirFullBits) {
  LinkingImage I(true);
  I.Bytes[I.Chain + 54] = 6;
  I.u64(16392, 4096);
  I.u64(16384, (1ULL << 63) | (2ULL << 51) | (0xffULL << 24));
  auto R = linking(I);
  ASSERT_TRUE(bool(R)) << llvm::toString(R.takeError());
  EXPECT_EQ(R->Fixups[0].Addend, 255);
  EXPECT_EQ(R->Fixups[1].Target, I.Base + 4096);
}
TEST(DarwinLinking, ChainedOrdinaryOrdinalsKeepBitsBelowTheSpecialRange) {
  for (unsigned Format : {1u, 2u, 3u}) {
    LinkingImage I(true);
    // Extend only this metadata fixture's command area. The execution reader
    // need not execute its entry bytes to identify a real provider ordinal128.
    unsigned Commands = llvm::support::endian::read32le(I.Bytes.data() + 16);
    for (unsigned N = 1; N < 128; ++N) {
      const auto At = I.CommandsEnd;
      I.CommandsEnd += 32;
      I.u32(At, LC_LOAD_DYLIB);
      I.u32(At + 4, 32);
      I.u32(At + 8, 24);
      std::copy_n("/x\0", 3, I.Bytes.data() + At + 24);
      ++Commands;
    }
    I.u32(16, Commands);
    I.u32(20, I.CommandsEnd - 32);
    I.Bytes[32775] = 128;
    I.u32(I.Chain + 20, Format);
    const unsigned Names = Format == 1 ? 76 : Format == 2 ? 80 : 88;
    I.u32(I.Chain + 12, Names);
    I.u32(I.Fixups + 12, Names + 8);
    I.u64(I.Chain + 72, 128);
    if (Format == 3)
      I.u64(I.Chain + 80, 0);
    std::copy_n("_write\0", 7, I.Bytes.data() + I.Chain + Names);
    auto R = linking(I);
    ASSERT_TRUE(bool(R)) << llvm::toString(R.takeError());
    EXPECT_EQ(R->Dependencies.size(), 128u);
    EXPECT_EQ(R->Fixups[0].LibraryOrdinal, 128);
    I.u32(I.Chain + 72, Format == 3 ? 0xffff : 0xff);
    refused(I, "special chained");
  }
  for (uint8_t Ordinal : {0u, 0xfeu, 0xffu}) {
    LinkingImage I(true);
    I.Bytes[32775] = Ordinal;
    refused(I, "ordinary library ordinal");
  }
}
TEST(DarwinLinking,
     PreparationDeadlineAndMetadataLimitsCannotPublishPartialFacts) {
  LinkingImage I(true);
  auto Full = linking(I);
  ASSERT_TRUE(bool(Full)) << llvm::toString(Full.takeError());
  auto Exact = readMachOProgramLinking(I.Bytes, Full->MetadataBytes,
                                       [] { return llvm::Error::success(); });
  ASSERT_TRUE(bool(Exact)) << llvm::toString(Exact.takeError());
  auto Under = readMachOProgramLinking(I.Bytes, Full->MetadataBytes - 1,
                                       [] { return llvm::Error::success(); });
  ASSERT_FALSE(bool(Under));
  EXPECT_NE(llvm::toString(Under.takeError()).find("byte limit"),
            std::string::npos);
  auto Small = readMachOProgramLinking(I.Bytes, 32,
                                       [] { return llvm::Error::success(); });
  ASSERT_FALSE(bool(Small));
  EXPECT_NE(llvm::toString(Small.takeError()).find("byte limit"),
            std::string::npos);
  unsigned Checks = 0;
  auto Expired =
      readMachOProgramLinking(I.Bytes, 1 << 20, [&]() -> llvm::Error {
        if (++Checks == 10)
          return llvm::make_error<darwin_model::PreparationTimeout>(
              GuestArchitecture::AArch64, LinkingImage::Base + 4096);
        return llvm::Error::success();
      });
  ASSERT_FALSE(bool(Expired));
  auto E = Expired.takeError();
  EXPECT_TRUE(E.isA<darwin_model::PreparationTimeout>());
  EXPECT_NE(llvm::toString(std::move(E)).find("deadline"), std::string::npos);
}
TEST(DarwinLinking,
     ProcessAdmissionRequiresTheProviderNamespaceAndExactSymbols) {
  const auto Load = [](const LinkingImage &I) {
    darwin_test::TemporaryImage File(I);
    return darwin_model::loadImage(File.path(), darwin_model::macOSProfile(),
                                   {});
  };
  for (bool Chain : {false, true}) {
    LinkingImage Good(Chain);
    Good.u32(Good.Data + 68, SG_READ_ONLY);
    auto R = Load(Good);
    ASSERT_TRUE(bool(R)) << llvm::toString(R.takeError());
    ASSERT_EQ(R->Imports.size(), 1u);
    EXPECT_EQ(R->Memory.Reserved.size(), 2u);
    EXPECT_GT(R->MetadataBytes, 0u);
    EXPECT_EQ(R->Plan.Regions[1].Permissions & Write, 0u);
    EXPECT_EQ(R->Memory.Maximum.back().Protection, 3u);
    const auto Reject = [&](const LinkingImage &I, llvm::StringRef Reason) {
      SCOPED_TRACE(Reason.str());
      auto Bad = Load(I);
      ASSERT_FALSE(bool(Bad));
      const auto Message = llvm::toString(Bad.takeError());
      EXPECT_NE(Message.find(Reason.str()), std::string::npos) << Message;
    };
    LinkingImage Flat(Chain);
    Flat.u32(24, MH_DYLDLINK | MH_PIE);
    Reject(Flat, "two-level");
    LinkingImage Provider(Chain);
    Provider.Bytes[Provider.Dependency + 24] = 'x';
    Reject(Provider, "provider");
    LinkingImage Weak(Chain);
    Weak.u32(Weak.Dependency, LC_LOAD_WEAK_DYLIB);
    Reject(Weak, "provider");
    LinkingImage Version(Chain);
    Version.u32(Version.Dependency + 20, 0x20000);
    Reject(Version, "provider");
    LinkingImage Exit(Chain);
    std::copy_n("_fork\0", 6, Exit.Bytes.data() + 32785);
    std::copy_n("_fork\0", 6,
                Exit.Bytes.data() + (Chain ? Exit.Chain + 76 : Exit.Bind + 2));
    if (!Chain) {
      std::move(Exit.Bytes.begin() + Exit.Bind + 9,
                Exit.Bytes.begin() + Exit.Bind + 14,
                Exit.Bytes.begin() + Exit.Bind + 8);
      Exit.Bytes[Exit.Bind + 13] = 0;
    }
    Reject(Exit, "libSystem import");
    auto Budget = ExecutionBudget::create(
        {100, 100, 1}, ExecutionBudget::Clock::now() - std::chrono::seconds(1));
    ASSERT_TRUE(bool(Budget));
    darwin_test::TemporaryImage File(Good);
    auto Expired = darwin_model::loadImage(
        File.path(), darwin_model::macOSProfile(), {}, Budget->get());
    ASSERT_FALSE(bool(Expired));
    auto E = Expired.takeError();
    EXPECT_TRUE(E.isA<darwin_model::PreparationTimeout>());
    EXPECT_NE(llvm::toString(std::move(E)).find("deadline"), std::string::npos);

    ProcessOptions Options;
    Options.Backend = ExecutionBackendKind::Unicorn;
    Options.Limits.TimeoutMicroseconds = 1;
    auto Timeout = darwin_model::runProcess(
        File.path(), darwin_model::macOSProfile(), Options);
    ASSERT_TRUE(bool(Timeout)) << llvm::toString(Timeout.takeError());
    EXPECT_EQ(Timeout->Stop, ProcessStopReason::Timeout);
    EXPECT_EQ(Timeout->SelectedBackend, ExecutionBackendKind::Unicorn);
    EXPECT_FALSE(Timeout->BackendSelectionReason.empty());
    EXPECT_EQ(Timeout->Entry, LinkingImage::Base + 4096);
    EXPECT_EQ(Timeout->PC, Timeout->Entry);
    EXPECT_EQ(Timeout->Instructions, 0u);
    EXPECT_EQ(Timeout->Events, 0u);
    EXPECT_FALSE(Timeout->ExitStatus);
    EXPECT_FALSE(Timeout->ReturnValue);
    EXPECT_FALSE(Timeout->LastCPUExit);
    EXPECT_TRUE(Timeout->Services.empty());
    EXPECT_TRUE(Timeout->NativeCalls.empty());

    // An expired deadline cannot turn malformed admission or a genuinely
    // unavailable transport into a successfully resolved Timeout result.
    auto WrongProfile = darwin_model::runProcess(
        File.path(), darwin_model::iOSProfile(false), Options);
    ASSERT_FALSE(bool(WrongProfile));
    EXPECT_NE(llvm::toString(WrongProfile.takeError()).find("platform"),
              std::string::npos);
#if defined(__APPLE__) || defined(_WIN32)
    Options.Backend = ExecutionBackendKind::KVM;
#else
    Options.Backend = ExecutionBackendKind::HVF;
#endif
    auto Unavailable = darwin_model::runProcess(
        File.path(), darwin_model::macOSProfile(), Options);
    ASSERT_FALSE(bool(Unavailable));
    auto BackendError = Unavailable.takeError();
    EXPECT_TRUE(BackendError.isA<BackendUnavailableError>());
    llvm::consumeError(std::move(BackendError));
  }
}
TEST(DarwinLinking, ExportTriesRetainTerminalPrefixesAndMappedAddresses) {
  LinkingImage I(false);
  const uint8_t Trie[] = {0, 1,   '_', 'x', 0, 6, 3,    0,    0x80, 0x20,
                          1, 'y', 0,   14,  3, 0, 0x81, 0x20, 0};
  std::copy(std::begin(Trie), std::end(Trie), I.Bytes.begin() + 33500);
  I.u32(I.Fixups + 40, 33500);
  I.u32(I.Fixups + 44, sizeof(Trie));
  auto R = linking(I);
  ASSERT_TRUE(bool(R)) << llvm::toString(R.takeError());
  ASSERT_EQ(R->Exports.size(), 2u);
  EXPECT_EQ(R->Exports[0].Name, "_x");
  EXPECT_EQ(R->Exports[1].Name, "_xy");
  EXPECT_EQ(R->Exports[0].Address, I.Base + 4096);
  EXPECT_EQ(R->Exports[1].Address, I.Base + 4097);
  for (const auto &Bad : std::vector<std::vector<uint8_t>>{
           {0, 1, 'x', 0, 0},
           {5, 0, 0},
           {0, 1, 'x', 0, 5, 4, 8, 0x80, 0x20, 0, 0},
           {0, 1, 'x', 0, 5, 4, 0, 0x80, 0x80, 0x02, 0},
           {0, 2, 'x', 0, 8, 'x', 0, 8, 3, 0, 0x80, 0x20, 0}}) {
    auto Copy = I;
    std::fill_n(Copy.Bytes.begin() + 33500, sizeof(Trie), 0);
    std::copy(Bad.begin(), Bad.end(), Copy.Bytes.begin() + 33500);
    Copy.u32(Copy.Fixups + 44, Bad.size());
    auto Rejected = linking(Copy);
    EXPECT_FALSE(bool(Rejected));
    llvm::consumeError(Rejected.takeError());
  }
}
TEST(DarwinLinking, ThreadLocalExportMustNameAnAdmittedDescriptor) {
  LinkingImage I(false);
  const uint8_t Trie[] = {0, 1, '_', 'x', 0, 6, 3, 1, 0x80, 0x20, 0};
  std::copy(std::begin(Trie), std::end(Trie), I.Bytes.begin() + 33500);
  I.u32(I.Fixups + 40, 33500);
  I.u32(I.Fixups + 44, sizeof(Trie));
  auto Facts = linking(I);
  ASSERT_TRUE(bool(Facts)) << llvm::toString(Facts.takeError());
  darwin_test::TemporaryImage File(I);
  auto R =
      darwin_model::loadImage(File.path(), darwin_model::macOSProfile(), {});
  ASSERT_FALSE(bool(R));
  EXPECT_NE(llvm::toString(R.takeError()).find("not a descriptor"),
            std::string::npos);
}
} // namespace
} // namespace neverd::emulation
