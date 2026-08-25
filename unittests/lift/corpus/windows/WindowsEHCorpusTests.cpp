//===- WindowsEHCorpusTests.cpp - Windows PE corpus tests ----------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "WindowsEHCorpusManifest.h"
#include "gtest/gtest.h"

#include "neverd/backend/llvm/WindowsEHMetadata.h"
#include "neverd/backend/llvm/WindowsEHMetadataEncoder.h"
#include "neverd/loader/BinaryImage.h"

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/Metadata.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Verifier.h"
#include "llvm/IRReader/IRReader.h"
#include "llvm/Support/Error.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/FileUtilities.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/Program.h"
#include "llvm/Support/SHA256.h"
#include "llvm/Support/SourceMgr.h"
#include "llvm/Support/raw_ostream.h"

#include <array>
#include <cstdint>
#include <filesystem>
#include <map>
#include <set>
#include <string>
#include <vector>

using namespace llvm;
using namespace neverd;
using namespace neverd::test;

namespace {

Expected<std::vector<WindowsEHArtifactExpectation>> loadExpectations() {
  const std::filesystem::path ManifestPath =
      std::filesystem::path(NEVERD_BINARY_CORPUS_ROOT) / "manifests" /
      "windows-eh.json";
  return loadWindowsEHCorpusManifest(ManifestPath.string(), true);
}

std::string diagnosticsFor(const ExceptionInfo &Info) {
  std::string Result;
  for (const std::string &Diagnostic : Info.Diagnostics) {
    if (!Result.empty())
      Result += "; ";
    Result += Diagnostic;
  }
  for (const ExceptionFunction &Function : Info.Functions)
    for (const std::string &Diagnostic : Function.Diagnostics) {
      if (!Result.empty())
        Result += "; ";
      Result += Diagnostic;
    }
  return Result;
}

bool containsString(ArrayRef<std::string> Values, StringRef Needle) {
  return llvm::any_of(
      Values, [&](const std::string &Value) { return Value == Needle; });
}

bool encodingMatchesArchitecture(ExceptionEncoding Encoding, Arch TheArch) {
  if (TheArch == Arch::ARM)
    return Encoding == ExceptionEncoding::ARM32Packed ||
           Encoding == ExceptionEncoding::ARM32PackedFragment ||
           Encoding == ExceptionEncoding::ARM32Unpacked;
  if (TheArch == Arch::AArch64)
    return Encoding == ExceptionEncoding::ARM64Packed ||
           Encoding == ExceptionEncoding::ARM64PackedFragment ||
           Encoding == ExceptionEncoding::ARM64Unpacked;
  return true;
}

TEST(WindowsEHCorpus, DeclaresCompleteMultiToolchainMatrix) {
  auto ExpectationsOrErr = loadExpectations();
  ASSERT_TRUE(static_cast<bool>(ExpectationsOrErr))
      << toString(ExpectationsOrErr.takeError());
  EXPECT_EQ(ExpectationsOrErr->size(), 168u);

  std::set<std::string> Toolchains;
  std::set<Arch> Architectures;
  for (const WindowsEHArtifactExpectation &Expectation : *ExpectationsOrErr) {
    Toolchains.insert(Expectation.Toolchain);
    Architectures.insert(Expectation.ExpectedArch);
  }
  EXPECT_EQ(Toolchains, (std::set<std::string>{"msvc", "clang-cl"}));
  EXPECT_EQ(Architectures,
            (std::set<Arch>{Arch::X86, Arch::X64, Arch::ARM, Arch::AArch64}));
}

TEST(WindowsEHCorpus, PreservesSharedFH3NativeFunctionGroupIdentity) {
  auto ExpectationsOrErr = loadExpectations();
  ASSERT_TRUE(static_cast<bool>(ExpectationsOrErr))
      << toString(ExpectationsOrErr.takeError());

  const WindowsEHArtifactExpectation *Probe = nullptr;
  for (const WindowsEHArtifactExpectation &Expectation : *ExpectationsOrErr) {
    if (Expectation.Name != "cxx_eh_probe" ||
        Expectation.Toolchain != "msvc" ||
        Expectation.Architecture != "x86_64" ||
        Expectation.CxxFormat != "fh3" || Expectation.SecurityCookie ||
        Expectation.Optimization != "o0")
      continue;
    ASSERT_EQ(Probe, nullptr) << "duplicate focused ABI probe";
    Probe = &Expectation;
  }
  ASSERT_NE(Probe, nullptr);

  const std::filesystem::path Input =
      std::filesystem::path(NEVERD_BINARY_CORPUS_ROOT) / Probe->Path;
  std::unique_ptr<Loader> ImageLoader = Loader::create(Input);
  ASSERT_NE(ImageLoader, nullptr);
  auto ImageOrErr = ImageLoader->load(Input);
  ASSERT_TRUE(static_cast<bool>(ImageOrErr))
      << toString(ImageOrErr.takeError());

  std::map<va_t, std::vector<const ExceptionFunction *>> Groups;
  for (const ExceptionFunction &Function :
       ImageOrErr->ExceptionMetadata.Functions) {
    if (!Function.Cxx ||
        Function.Cxx->NativeEncoding != CxxExceptionInfo::Encoding::FH3 ||
        !Function.Cxx->IsSeparated)
      continue;
    ASSERT_NE(Function.Cxx->NativeFuncInfoVA, 0u);
    Groups[Function.Cxx->NativeFuncInfoVA].push_back(&Function);
  }

  bool SawParentAndCatchGroup = false;
  llvm::LLVMContext Context;
  for (const auto &[GroupVA, Members] : Groups) {
    bool SawParent = false;
    bool SawCatch = false;
    for (const ExceptionFunction *Member : Members) {
      ASSERT_NE(Member, nullptr);
      SawCatch |= Member->Cxx->IsCatchFunclet;
      SawParent |= !Member->Cxx->IsCatchFunclet;

      llvm::MDNode *Payload = windows_eh_md::getCanonicalFunctionMetadata(
          Context, *Member, ImageOrErr->Arch, ImageOrErr->Format);
      ASSERT_NE(Payload, nullptr);
      const auto *Header = llvm::dyn_cast<llvm::MDNode>(
          Payload->getOperand(windows_eh_md::CxxHeader).get());
      ASSERT_NE(Header, nullptr);
      ASSERT_EQ(Header->getNumOperands(),
                windows_eh_md::CxxHeaderOperandCount);
      const auto *GroupMetadata = llvm::dyn_cast<llvm::ConstantAsMetadata>(
          Header->getOperand(windows_eh_md::CxxNativeFuncInfoVA).get());
      const auto *EncodedGroup =
          GroupMetadata
              ? llvm::dyn_cast<llvm::ConstantInt>(GroupMetadata->getValue())
              : nullptr;
      ASSERT_NE(EncodedGroup, nullptr);
      EXPECT_EQ(EncodedGroup->getZExtValue(), GroupVA);
    }
    SawParentAndCatchGroup |= Members.size() >= 2 && SawParent && SawCatch;
  }
  EXPECT_TRUE(SawParentAndCatchGroup)
      << "focused FH3 image exposed no complete parent/catch function group";
}

// ARM32 spells every code pointer in a language table with the Thumb
// interworking bit set.  The runtime masks it before use, so a decoder that
// keeps it yields guarded ranges that overrun their function by a byte and
// handler addresses that name no instruction — a failure that looks like
// corrupt metadata rather than a decoding bug.  Requiring every recovered
// code address to be even pins the masking directly.
TEST(WindowsEHCorpus, MasksThumbBitInARM32LanguageTables) {
  auto ExpectationsOrErr = loadExpectations();
  ASSERT_TRUE(static_cast<bool>(ExpectationsOrErr))
      << toString(ExpectationsOrErr.takeError());
  const std::filesystem::path CorpusRoot(NEVERD_BINARY_CORPUS_ROOT);

  unsigned Images = 0;
  unsigned CxxRecords = 0;
  unsigned TryBlocks = 0;
  unsigned Scopes = 0;
  for (const WindowsEHArtifactExpectation &Expectation : *ExpectationsOrErr) {
    if (Expectation.ExpectedArch != Arch::ARM)
      continue;
    const std::filesystem::path ArtifactPath = CorpusRoot / Expectation.Path;
    std::unique_ptr<Loader> ImageLoader = Loader::create(ArtifactPath);
    if (!ImageLoader)
      continue;
    auto ImageOrErr = ImageLoader->load(ArtifactPath);
    if (!ImageOrErr) {
      consumeError(ImageOrErr.takeError());
      ADD_FAILURE() << "cannot load " << Expectation.Path;
      continue;
    }
    ++Images;
    SCOPED_TRACE(Expectation.Path);
    const ExceptionInfo &Info = ImageOrErr->ExceptionMetadata;
    const std::string Diagnostics = diagnosticsFor(Info);
    for (const ExceptionFunction &Function : Info.Functions) {
      EXPECT_NE(Function.ParseStatus, ExceptionParseStatus::Malformed)
          << Diagnostics;
      if (Function.SEH)
        for (const SEHScopeRecord &Scope : Function.SEH->Scopes) {
          ++Scopes;
          EXPECT_EQ(Scope.GuardedRange.Begin & 1u, 0u);
          EXPECT_EQ(Scope.GuardedRange.End & 1u, 0u);
          EXPECT_EQ(Scope.FilterOrFinallyVA & 1u, 0u);
          EXPECT_EQ(Scope.HandlerVA & 1u, 0u);
          EXPECT_TRUE(Scope.GuardedRange.Begin == 0 ||
                      Function.CodeRange.isValid());
        }
      if (!Function.Cxx)
        continue;
      ++CxxRecords;
      TryBlocks += Function.Cxx->TryBlocks.size();
      EXPECT_TRUE(Function.Cxx->hasValidStateGraph());
      for (const CxxUnwindAction &Action : Function.Cxx->UnwindMap)
        EXPECT_EQ(Action.ActionVA & 1u, 0u);
      for (const CxxIPState &State : Function.Cxx->IPMap)
        EXPECT_EQ(State.IP & 1u, 0u);
      for (const CxxTryBlock &Try : Function.Cxx->TryBlocks)
        for (const CxxCatchHandler &Catch : Try.Handlers)
          EXPECT_EQ(Catch.HandlerVA & 1u, 0u);
    }
  }

  EXPECT_EQ(Images, 24u);
  EXPECT_GE(CxxRecords, 30u);
  EXPECT_GE(TryBlocks, 70u);
  EXPECT_GE(Scopes, 700u);
}

// `__GSHandlerData` packs its flags into the low bits of the cookie's frame
// offset, so a 32-bit target has one fewer of them than a 64-bit one and drops
// the aligned-frame pair that a 64-bit record stores behind the word.  Decoding
// the wide shape out of a narrow record misreads the offset and then reads two
// words of whatever .xdata follows, which is how the ARM32 GS images used to
// come back partial.
TEST(WindowsEHCorpus, DecodesGSHandlerDataAtTheTargetsPointerWidth) {
  auto ExpectationsOrErr = loadExpectations();
  ASSERT_TRUE(static_cast<bool>(ExpectationsOrErr))
      << toString(ExpectationsOrErr.takeError());
  const std::filesystem::path CorpusRoot(NEVERD_BINARY_CORPUS_ROOT);

  unsigned NarrowCookies = 0;
  unsigned WideCookies = 0;
  for (const WindowsEHArtifactExpectation &Expectation : *ExpectationsOrErr) {
    const std::filesystem::path ArtifactPath = CorpusRoot / Expectation.Path;
    std::unique_ptr<Loader> ImageLoader = Loader::create(ArtifactPath);
    if (!ImageLoader) {
      ADD_FAILURE() << "NeverD did not recognize the GS corpus artifact";
      continue;
    }
    auto ImageOrErr = ImageLoader->load(ArtifactPath);
    if (!ImageOrErr) {
      ADD_FAILURE() << "NeverD failed to load the GS corpus artifact: "
                    << toString(ImageOrErr.takeError());
      continue;
    }
    SCOPED_TRACE(Expectation.Path);
    const bool Wide = ImageOrErr->is64Bit();
    for (const ExceptionFunction &Function :
         ImageOrErr->ExceptionMetadata.Functions) {
      if (!Function.GSCookie)
        continue;
      const GSCookieInfo &Cookie = *Function.GSCookie;
      EXPECT_EQ(Cookie.ParseStatus, ExceptionParseStatus::Complete)
          << diagnosticsFor(ImageOrErr->ExceptionMetadata);
      // The masked-off bits are exactly the ones the offset cannot use, so a
      // correctly masked offset is aligned to the target's pointer size.
      const int32_t Alignment = Wide ? 8 : 4;
      EXPECT_EQ(Cookie.CookieOffset % Alignment, 0)
          << "cookie offset " << Cookie.CookieOffset << " keeps a flag bit";
      if (Wide) {
        ++WideCookies;
        EXPECT_EQ(Cookie.Payload.size(), Cookie.HasAlignment ? 12u : 4u);
      } else {
        ++NarrowCookies;
        // A 32-bit CRT derives the aligned-frame adjustment arithmetically, so
        // it stores nothing for it however the flag reads.
        EXPECT_EQ(Cookie.Payload.size(), 4u);
        EXPECT_EQ(Cookie.AlignmentBaseOffset, 0);
        EXPECT_EQ(Cookie.Alignment, 0u);
        EXPECT_FALSE(Cookie.HasExceptionHandler);
        EXPECT_FALSE(Cookie.HasUnwindHandler);
      }
    }
  }

  EXPECT_GT(NarrowCookies, 0u) << "no 32-bit GS record was exercised";
  EXPECT_GT(WideCookies, 0u) << "no 64-bit GS record was exercised";
}

// x86-32 carries no exception directory, so the manifest can only require the
// image to load.  What NeverD recovers from the FS:[0] registration chain is
// still a contract worth pinning: every record must name a classified handler,
// sit inside real code, and — for the two dialects that own a table — decode
// one.  Anchoring on the corpus totals catches a scan that silently stops
// finding frames, which a per-artifact "at least one" check would not.
//
// The scope-table entry array is unsized, so the decisive invariant is that no
// two functions claim overlapping table bytes.  The compiler emits these
// tables consecutively, which means a walk that stops only when an entry fails
// validation runs into the next function's table and reports its handlers as
// this function's; the entries it invents are individually well-formed, so
// nothing but disjointness catches it.
TEST(WindowsEHCorpus, RecoversX86RegistrationChains) {
  auto ExpectationsOrErr = loadExpectations();
  ASSERT_TRUE(static_cast<bool>(ExpectationsOrErr))
      << toString(ExpectationsOrErr.takeError());
  const std::filesystem::path CorpusRoot(NEVERD_BINARY_CORPUS_ROOT);

  unsigned Images = 0;
  unsigned Records = 0;
  unsigned ScopeEntries = 0;
  unsigned CxxRecords = 0;
  unsigned EH4Records = 0;
  for (const WindowsEHArtifactExpectation &Expectation : *ExpectationsOrErr) {
    if (Expectation.ExpectedArch != Arch::X86)
      continue;
    const std::filesystem::path ArtifactPath = CorpusRoot / Expectation.Path;
    std::unique_ptr<Loader> ImageLoader = Loader::create(ArtifactPath);
    if (!ImageLoader)
      continue;
    auto ImageOrErr = ImageLoader->load(ArtifactPath);
    if (!ImageOrErr) {
      consumeError(ImageOrErr.takeError());
      ADD_FAILURE() << "cannot load " << Expectation.Path;
      continue;
    }
    ++Images;
    SCOPED_TRACE(Expectation.Path);
    const ExceptionInfo &Info = ImageOrErr->ExceptionMetadata;

    // Byte span each recovered scope table occupies, in table order.
    std::vector<std::pair<va_t, va_t>> TableSpans;
    for (const ExceptionFunction &Function : Info.Functions) {
      if (Function.model() != ExceptionModel::WindowsRegistration)
        continue;
      ++Records;
      EXPECT_TRUE(Function.CodeRange.isValid());
      EXPECT_NE(Function.ParseStatus, ExceptionParseStatus::Malformed);
      ASSERT_TRUE(Function.Registration.has_value());
      const RegistrationChainInfo &Chain = *Function.Registration;
      EXPECT_TRUE(Function.CodeRange.contains(Chain.ChainInstallVA));
      EXPECT_NE(Chain.HandlerVA, 0u);
      for (const RegistrationScopeRecord &Scope : Chain.Scopes) {
        ++ScopeEntries;
        EXPECT_NE(Scope.HandlerVA, 0u);
        EXPECT_EQ(Scope.IsFinally, Scope.FilterVA == 0);
      }
      const bool IsEH4 =
          Function.Encoding == ExceptionEncoding::X86ScopeTableEH4;
      EH4Records += IsEH4;
      if (!Chain.Scopes.empty() && Chain.ScopeTableVA != 0 &&
          Function.Encoding != ExceptionEncoding::X86CxxFuncInfo) {
        const va_t ArrayVA = Chain.ScopeTableVA + (IsEH4 ? 16 : 0);
        TableSpans.emplace_back(ArrayVA, ArrayVA + Chain.Scopes.size() * 12);
      }
      if (Function.Encoding == ExceptionEncoding::X86CxxFuncInfo) {
        ++CxxRecords;
        ASSERT_TRUE(Function.Cxx.has_value());
        EXPECT_NE(Function.Cxx->NativeFuncInfoVA, 0u);
        EXPECT_EQ(Function.Cxx->NativeFuncInfoVA, Chain.ScopeTableVA);
        EXPECT_TRUE(Function.Cxx->hasValidStateGraph());
        // x86 keeps the current state in the frame, never in a table.
        EXPECT_TRUE(Function.Cxx->IPMap.empty());
      }
    }

    std::sort(TableSpans.begin(), TableSpans.end());
    for (size_t I = 1; I < TableSpans.size(); ++I)
      EXPECT_LE(TableSpans[I - 1].second, TableSpans[I].first)
          << "scope table at 0x" << llvm::utohexstr(TableSpans[I - 1].first)
          << " runs into the one at 0x" << llvm::utohexstr(TableSpans[I].first);
  }

  EXPECT_EQ(Images, 36u);
  EXPECT_GE(Records, 200u);
  EXPECT_GE(ScopeEntries, 900u);
  EXPECT_GE(CxxRecords, 12u);
  // Half the x86 matrix is built with /GS, whose frames install the local
  // `_except_handler4` wrapper rather than an importable routine.  Requiring
  // the dialect to appear keeps that identification from silently lapsing back
  // into "unknown handler, no table".
  EXPECT_GE(EH4Records, 80u);
}

// An ARM `.pdata` entry describes a frame in one of two forms, and neither is
// readable as a frame layout without being decoded: the packed form is a set
// of counts standing for a prologue that is never written down, and the
// unpacked form is a byte string of unwind codes.  Keeping the raw bytes and
// stopping there leaves every ARM frame with no recovered saves at all, which
// reads downstream as a function that saves nothing rather than as one whose
// description was not decoded.  Requiring the whole corpus to yield operations
// -- and requiring the register saves among them to name real callee-saved
// registers -- is what keeps that from silently returning.
TEST(WindowsEHCorpus, DecodesARMUnwindOperations) {
  auto ExpectationsOrErr = loadExpectations();
  ASSERT_TRUE(static_cast<bool>(ExpectationsOrErr))
      << toString(ExpectationsOrErr.takeError());
  const std::filesystem::path CorpusRoot(NEVERD_BINARY_CORPUS_ROOT);

  unsigned Images = 0;
  unsigned Frames = 0;
  unsigned FramesWithOperations = 0;
  unsigned PackedFrames = 0;
  unsigned UnpackedFrames = 0;
  unsigned RegisterSaves = 0;
  unsigned Epilogues = 0;
  for (const WindowsEHArtifactExpectation &Expectation : *ExpectationsOrErr) {
    const bool IsARM = Expectation.ExpectedArch == Arch::ARM ||
                       Expectation.ExpectedArch == Arch::AArch64;
    if (!IsARM)
      continue;
    const std::filesystem::path ArtifactPath = CorpusRoot / Expectation.Path;
    std::unique_ptr<Loader> ImageLoader = Loader::create(ArtifactPath);
    if (!ImageLoader)
      continue;
    auto ImageOrErr = ImageLoader->load(ArtifactPath);
    if (!ImageOrErr) {
      consumeError(ImageOrErr.takeError());
      ADD_FAILURE() << "cannot load " << Expectation.Path;
      continue;
    }
    ++Images;
    SCOPED_TRACE(Expectation.Path);
    const bool IsAArch64 = ImageOrErr->Arch == Arch::AArch64;

    for (const ExceptionFunction &Function :
         ImageOrErr->ExceptionMetadata.Functions) {
      if (Function.model() != ExceptionModel::WindowsTable)
        continue;
      ++Frames;
      const bool Packed =
          Function.Encoding == ExceptionEncoding::ARM32Packed ||
          Function.Encoding == ExceptionEncoding::ARM32PackedFragment ||
          Function.Encoding == ExceptionEncoding::ARM64Packed ||
          Function.Encoding == ExceptionEncoding::ARM64PackedFragment;
      if (Function.UnwindOperations.empty())
        continue;
      ++FramesWithOperations;
      if (Packed)
        ++PackedFrames;
      else
        ++UnpackedFrames;
      Epilogues += static_cast<unsigned>(Function.Epilogs.size());

      // The prologue length is the span of the instructions the operations
      // stand against, so it can never exceed the function they belong to.
      EXPECT_LE(Function.PrologueSize, Function.CodeRange.size());

      for (const UnwindOperation &Op : Function.UnwindOperations) {
        if (Op.RegisterClass == UnwindRegisterClass::None) {
          EXPECT_EQ(Op.RegisterMask, 0u);
          continue;
        }
        ++RegisterSaves;
        // Every named register must appear in the mask, and the mask must
        // start at the register the operation reports as its lowest.
        ASSERT_NE(Op.RegisterMask, 0u);
        EXPECT_EQ(Op.Register,
                  static_cast<uint16_t>(llvm::countr_zero(Op.RegisterMask)));
        // A callee-saved integer save on ARM64 can only name x19 upward, the
        // frame pointer, or the link register; on ARM32 it can only name a
        // register that exists.  A decode that drifts out of the file is how
        // a misread offset field first shows itself.
        if (Op.RegisterClass == UnwindRegisterClass::GeneralPurpose)
          EXPECT_LT(Op.Register, IsAArch64 ? 31u : 16u);
        else
          EXPECT_LT(Op.Register, 32u);
      }
    }
  }

  EXPECT_EQ(Images, 72u);
  EXPECT_GT(Frames, 0u);
  // Both forms are present across the corpus: a leaf function that only
  // allocates gets packed data, while anything with an exception handler or an
  // unusual prologue gets a full record.
  EXPECT_GT(PackedFrames, 0u);
  EXPECT_GT(UnpackedFrames, 0u);
  EXPECT_GT(Epilogues, 0u);
  EXPECT_GT(RegisterSaves, 0u);
  // A frame that yields no operations at all is one whose description was
  // rejected rather than read; a handful may legitimately be fragments that
  // declare no prologue, but the bulk must decode.
  EXPECT_GT(FramesWithOperations * 10, Frames * 9);
}

TEST(WindowsEHCorpus, ParsesDeclaredExceptionMetadata) {
  auto ExpectationsOrErr = loadExpectations();
  ASSERT_TRUE(static_cast<bool>(ExpectationsOrErr))
      << toString(ExpectationsOrErr.takeError());
  const std::filesystem::path CorpusRoot(NEVERD_BINARY_CORPUS_ROOT);

  for (const WindowsEHArtifactExpectation &Expectation : *ExpectationsOrErr) {
    SCOPED_TRACE(Expectation.Path + " [" + Expectation.Toolchain + ", " +
                 Expectation.Architecture + ", " + Expectation.CxxFormat +
                 ", " + (Expectation.SecurityCookie ? "gs" : "no-gs") + ", " +
                 Expectation.Optimization + "]");

    const std::filesystem::path ArtifactPath = CorpusRoot / Expectation.Path;
    auto BufferOrErr = MemoryBuffer::getFile(ArtifactPath.string());
    if (!BufferOrErr) {
      ADD_FAILURE() << "cannot read artifact: "
                    << BufferOrErr.getError().message();
      continue;
    }
    StringRef Bytes = (*BufferOrErr)->getBuffer();
    if (Bytes.size() != Expectation.Size) {
      ADD_FAILURE() << "artifact size does not match the manifest";
      continue;
    }
    std::array<uint8_t, 32> Digest = SHA256::hash(arrayRefFromStringRef(Bytes));
    if (toHex(ArrayRef<uint8_t>(Digest), true) != Expectation.SHA256) {
      ADD_FAILURE() << "artifact SHA-256 does not match the manifest";
      continue;
    }

    std::unique_ptr<Loader> ImageLoader = Loader::create(ArtifactPath);
    if (!ImageLoader) {
      ADD_FAILURE() << "NeverD did not recognize the PE artifact";
      continue;
    }
    auto ImageOrErr = ImageLoader->load(ArtifactPath);
    if (!ImageOrErr) {
      ADD_FAILURE() << "NeverD failed to load the PE artifact: "
                    << toString(ImageOrErr.takeError());
      continue;
    }
    const BinaryImage &Image = *ImageOrErr;
    EXPECT_EQ(Image.Format, BinaryFormat::COFF);
    EXPECT_EQ(Image.Arch, Expectation.ExpectedArch);

    const ExceptionInfo &Info = Image.ExceptionMetadata;
    const std::string Diagnostics = diagnosticsFor(Info);
    if (Expectation.ValidationLevel == CorpusValidationLevel::LoadOnly)
      continue;

    EXPECT_NE(Info.ParseStatus, ExceptionParseStatus::Malformed) << Diagnostics;
    EXPECT_TRUE(containsString(Expectation.AllowedParseStatuses,
                               getExceptionParseStatusName(Info.ParseStatus)))
        << "unexpected image exception parse status: "
        << getExceptionParseStatusName(Info.ParseStatus) << "; " << Diagnostics;
    for (const ExceptionFunction &Function : Info.Functions)
      EXPECT_NE(Function.ParseStatus, ExceptionParseStatus::Malformed)
          << Diagnostics;

    EXPECT_GE(Info.Functions.size(), Expectation.MinExceptionFunctions);
    if (Expectation.ValidationLevel == CorpusValidationLevel::UnwindOnly) {
      for (const ExceptionFunction &Function : Info.Functions) {
        EXPECT_TRUE(Function.CodeRange.isValid());
        EXPECT_TRUE(encodingMatchesArchitecture(Function.Encoding, Image.Arch));
      }
      continue;
    }

    uint64_t CxxFunctions = 0;
    uint64_t TryBlocks = 0;
    uint64_t SEHScopes = 0;
    std::set<std::string> Personalities;
    for (const ExceptionFunction &Function : Info.Functions) {
      if (Function.Personality != ExceptionPersonality::None &&
          Function.Personality != ExceptionPersonality::Unknown)
        Personalities.insert(getExceptionPersonalityName(Function.Personality));
      if (Function.SEH) {
        SEHScopes += Function.SEH->Scopes.size();
        for (const SEHScopeRecord &Scope : Function.SEH->Scopes) {
          EXPECT_TRUE(Scope.GuardedRange.isValid());
          EXPECT_NE(Scope.ParseStatus, ExceptionParseStatus::Malformed);
        }
      }
      if (Function.Cxx) {
        ++CxxFunctions;
        TryBlocks += Function.Cxx->TryBlocks.size();
        EXPECT_TRUE(Function.Cxx->hasValidStateGraph());
      }
    }

    EXPECT_GE(CxxFunctions, Expectation.MinCxxFunctions);
    EXPECT_GE(TryBlocks, Expectation.MinTryBlocks);
    EXPECT_GE(SEHScopes, Expectation.MinSEHScopes);
    const bool PersonalityMatched =
        Expectation.Personalities.empty() ||
        llvm::any_of(Expectation.Personalities, [&](const std::string &Name) {
          return Personalities.contains(Name);
        });
    EXPECT_TRUE(PersonalityMatched)
        << "parsed personalities did not include any manifest alternative";
  }
}

} // namespace
