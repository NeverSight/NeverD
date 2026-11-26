//===- MachOAuthenticatedNativeTests.cpp - Apple dyld oracle -------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Install into a real linked arm64e image and inspect with Apple's dyld_info.
/// Separately qualify native calls and unwinding after a compiler-original
/// execution control succeeds; offline checks never imply runtime support.
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "neverd/backend/codegen/CodeGen.h"
#include "neverd/backend/codegen/MachO/MachOAuthenticatedFixups.h"
#include "neverd/backend/codegen/MachO/MachOExceptionPatch.h"
#include "neverd/backend/codegen/MachO/MachOLayoutGrowth.h"
#include "neverd/backend/codegen/MachO/MachOPatch.h"
#include "neverd/backend/codegen/MachO/MachOSignaturePolicy.h"
#include "neverd/backend/codegen/MachO/MachOSigner.h"
#include "neverd/backend/codegen/PatchObject.h"
#include "neverd/backend/codegen/SourcePatchDependencies.h"
#include "neverd/ir/intrinsics/AArch64/AArch64PAuth.h"
#include "neverd/loader/DWARF/AArch64ReturnAddressState.h"
#include "neverd/loader/DirectBranch.h"
#include "neverd/loader/LanguageRuntime.h"
#include "neverd/loader/Loader.h"
#include "neverd/loader/MachO/CompactUnwind.h"
#include "neverd/object/MachOLayout.h"
#include "neverd/object/SectionNames.h"
#include "neverd/support/AArch64Literal.h"
#include "neverd/support/BinaryEncoding.h"

#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/ScopeExit.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/AsmParser/Parser.h"
#include "llvm/DebugInfo/DWARF/DWARFContext.h"
#include "llvm/DebugInfo/DWARF/DWARFDebugFrame.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/IRReader/IRReader.h"
#include "llvm/Object/MachO.h"
#include "llvm/Object/ObjectFile.h"
#include "llvm/Support/Errc.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/MathExtras.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/Program.h"
#include "llvm/Support/SourceMgr.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/TargetParser/Triple.h"
#include "llvm/Transforms/Utils/Cloning.h"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <optional>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

namespace {
using namespace neverd;
#define MACHO_AUTH_NATIVE_TEXT(Name, Value) constexpr char Name[] = Value;
#define MACHO_AUTH_NATIVE_VALUE(Type, Name, Value) constexpr Type Name = Value;
#define MACHO_AUTH_NATIVE_ARGS(Name, ...)                                      \
  constexpr llvm::StringLiteral Name[] = {__VA_ARGS__};
#include "MachOAuthenticatedNativeFixtures.def"

TEST(MachOArm64eRuntimeGlobals, EmitsSignedRegistrationInDyldPriorityOrder) {
  llvm::LLVMContext Context;
  llvm::SMDiagnostic Diagnostic;
  auto M = llvm::parseAssemblyString(kDarwinDestructorIR, Diagnostic, Context);
  ASSERT_NE(M, nullptr) << Diagnostic.getMessage().str();
  auto Compiled = Codegen().compile(*M, Arch::AArch64, BinaryFormat::MachO,
                                    M->getTargetTriple().str());
  ASSERT_TRUE(Compiled.Success);
  std::map<uint64_t, std::string> ByPriority, ByOffset;
  const auto *Ctors = M->getNamedGlobal(kIRConstructorList);
  ASSERT_NE(Ctors, nullptr);
  for (const auto &Entry : Ctors->getInitializer()->operands()) {
    const auto *Row = llvm::cast<llvm::ConstantStruct>(Entry);
    const auto *F =
        llvm::cast<llvm::Function>(Row->getOperand(kIRConstructorFunction));
    const auto Priority =
        llvm::cast<llvm::ConstantInt>(Row->getOperand(kIRConstructorPriority))
            ->getZExtValue();
    ByPriority.emplace(Priority,
                       (llvm::Twine(kMachOSymbolPrefix) + F->getName()).str());
  }
  const auto *Register = M->getFunction(kDarwinRegisterName);
  ASSERT_NE(Register, nullptr);
  unsigned Registrations = 0;
  for (const auto *User : Register->users()) {
    const auto *Call = llvm::dyn_cast<llvm::CallBase>(User);
    ASSERT_NE(Call, nullptr);
    const auto *Recipe =
        llvm::dyn_cast<llvm::ConstantPtrAuth>(Call->getArgOperand(0));
    ASSERT_NE(Recipe, nullptr);
    EXPECT_EQ(Recipe->getKey()->getZExtValue(), kAArch64PAuthKeyIA);
    EXPECT_TRUE(Recipe->getDiscriminator()->isZero());
    EXPECT_FALSE(Recipe->hasAddressDiscriminator());
    EXPECT_TRUE(llvm::isa<llvm::Function>(Recipe->getPointer()));
    ++Registrations;
  }
  EXPECT_EQ(Registrations, kRegisteredDestructors);
  const llvm::StringRef Bytes(
      reinterpret_cast<const char *>(Compiled.ObjectData.data()),
      Compiled.ObjectData.size());
  auto Object = llvm::object::ObjectFile::createObjectFile(
      llvm::MemoryBufferRef(Bytes, kOriginalFile));
  ASSERT_TRUE(bool(Object)) << llvm::toString(Object.takeError());
  const auto &MachO = llvm::cast<llvm::object::MachOObjectFile>(**Object);
  for (const auto &Section : MachO.sections()) {
    auto Flags = MachO.getSection64(Section.getRawDataRefImpl()).flags;
    if ((Flags & llvm::MachO::SECTION_TYPE) !=
        llvm::MachO::S_MOD_INIT_FUNC_POINTERS)
      continue;
    for (const auto &Reloc : Section.relocations()) {
      auto Symbol = Reloc.getSymbol();
      ASSERT_NE(Symbol, MachO.symbol_end());
      auto Name = Symbol->getName();
      ASSERT_TRUE(bool(Name)) << llvm::toString(Name.takeError());
      ByOffset.emplace(Reloc.getOffset(), Name->str());
    }
  }
  ASSERT_EQ(ByPriority.size(), kRegisteredInitializers);
  ASSERT_EQ(ByOffset.size(), kRegisteredInitializers);
  EXPECT_TRUE(std::equal(
      ByPriority.begin(), ByPriority.end(), ByOffset.begin(),
      [](const auto &P, const auto &S) { return P.second == S.second; }));
}

TEST(MachOArm64eRuntimeGlobals, RejectsAnIncompatibleDestructorPrototype) {
  llvm::LLVMContext Context;
  llvm::SMDiagnostic Diagnostic;
  auto M = llvm::parseAssemblyString(kInvalidDestructorIR, Diagnostic, Context);
  ASSERT_NE(M, nullptr) << Diagnostic.getMessage().str();
  auto Compiled = Codegen().compile(*M, Arch::AArch64, BinaryFormat::MachO,
                                    M->getTargetTriple().str());
  EXPECT_FALSE(Compiled.Success);
  EXPECT_TRUE(Compiled.ObjectData.empty());
}

void checkAppleChainedRecipes(llvm::StringRef Info,
                              llvm::ArrayRef<uint8_t> Binary,
                              llvm::ArrayRef<MachOAuthenticatedFixup> Fixups) {
  ASSERT_TRUE(Info.contains(kFixupsHeading));
  ASSERT_TRUE(Info.contains(kImportsHeading));
  llvm::SmallVector<llvm::StringRef> Lines;
  Info.split(Lines, '\n');
  uint64_t PreferredAddress = 0;
  forEachMachOLoadCommand(Binary.data(), Binary.size(),
                          [&](const uint8_t *C, uint32_t ID, uint32_t, bool) {
                            if (ID == llvm::MachO::LC_SEGMENT_64) {
                              const auto Segment = readMachOSegment(C, true);
                              if (!Segment.FileOff && Segment.FileSize)
                                PreferredAddress = Segment.VMAddr;
                            }
                          });
  const auto Number = [](llvm::StringRef Value, llvm::StringRef Suffix,
                         uint64_t &Result) {
    return Value.consume_back(Suffix) && !Value.getAsInteger(0, Result);
  };
  for (const auto &Fixup : Fixups) {
    uint64_t FileOffset = 0;
    unsigned Owners = 0;
    forEachMachOLoadCommand(
        Binary.data(), Binary.size(),
        [&](const uint8_t *C, uint32_t ID, uint32_t, bool) {
          if (ID != llvm::MachO::LC_SEGMENT_64)
            return;
          const auto Segment = readMachOSegment(C, true);
          if (Fixup.Address >= Segment.VMAddr &&
              rangeInBounds(Fixup.Address - Segment.VMAddr, kPointerBytes,
                            Segment.FileSize)) {
            ++Owners;
            FileOffset = Segment.FileOff + Fixup.Address - Segment.VMAddr;
          }
        });
    ASSERT_EQ(Owners, 1u);
    unsigned Matches = 0;
    for (auto Line : Lines) {
      llvm::SmallVector<llvm::StringRef> Fields;
      Line.trim().split(Fields, ' ', -1, false);
      uint64_t Address;
      if (Fields.size() != (Fixup.Authenticated ? kDetailFieldCount
                                                : kPlainDetailFieldCount) ||
          !Number(Fields[kDetailAddress], kColon, Address) ||
          Address != FileOffset)
        continue;
      ++Matches;
      uint64_t Raw, Diversity, AddressDiversity, Target;
      ASSERT_FALSE(Fields[kDetailRaw].getAsInteger(0, Raw));
      EXPECT_EQ(Raw, readLE<uint64_t>(Binary.data() + FileOffset));
      const auto TargetField =
          Fixup.Authenticated ? kDetailTarget : kPlainDetailTarget;
      ASSERT_TRUE(Number(Fields[TargetField], kClosingParenthesis, Target));
      ASSERT_TRUE(Fields[kDetailKind].consume_back(kColon));
      const auto &Recipe = Fixup.Recipe;
      if (Fixup.Authenticated) {
        ASSERT_TRUE(Number(Fields[kDetailDiversity], kComma, Diversity));
        ASSERT_TRUE(
            Number(Fields[kDetailAddressDiversity], kComma, AddressDiversity));
        ASSERT_TRUE(Fields[kDetailKey].consume_back(kComma));
        EXPECT_EQ(Diversity, Recipe.Diversity);
        EXPECT_EQ(AddressDiversity, unsigned(Recipe.AddressDiversity));
        EXPECT_EQ(Fields[kDetailKey], kKeyNames[Recipe.Key]);
      }
      EXPECT_EQ(Fields[kDetailKind],
                Recipe.Symbol.empty()
                    ? (Fixup.Authenticated ? kAuthRebase : kPlainRebase)
                    : (Fixup.Authenticated ? kAuthBind : kPlainBind));
      if (Recipe.Symbol.empty()) {
        EXPECT_EQ(Fields[TargetField - 1], kTargetLabel);
        EXPECT_EQ(Target, Recipe.Target - PreferredAddress);
        continue;
      }
      EXPECT_EQ(Fields[TargetField - 1], kImportLabel);
      unsigned Imports = 0;
      for (auto ImportLine : Lines) {
        llvm::SmallVector<llvm::StringRef> Import;
        ImportLine.trim().split(Import, ' ', -1, false);
        const unsigned Weak = unsigned(Recipe.WeakImport);
        uint64_t Ordinal;
        if (Import.size() != kImportFieldCount + Weak ||
            Import[kImportFrom + Weak] != kFromLabel ||
            Import[kImportOrdinal].getAsInteger(0, Ordinal) ||
            Ordinal != Target)
          continue;
        ++Imports;
        if (Weak)
          EXPECT_EQ(Import[kImportFrom], kWeakImportLabel);
        const auto [Symbol, AddendText] =
            Import[kImportSymbol].split(kAddendSeparator);
        uint64_t Addend = 0;
        if (!AddendText.empty())
          ASSERT_FALSE(AddendText.getAsInteger(0, Addend));
        EXPECT_EQ(Symbol, Recipe.Symbol);
        EXPECT_EQ(Addend, uint64_t(Recipe.Addend));
        ASSERT_TRUE(
            Import[kImportModule + Weak].consume_back(kClosingParenthesis));
        auto Module = llvm::sys::path::filename(Recipe.Module)
                          .split(kLibraryVersionSeparator)
                          .first;
        if (Recipe.LibraryOrdinal ==
            llvm::MachO::BIND_SPECIAL_DYLIB_WEAK_LOOKUP)
          Module = kWeakLookupProvider;
        EXPECT_EQ(Import[kImportModule + Weak], Module);
      }
      EXPECT_EQ(Imports, 1u);
    }
    EXPECT_EQ(Matches, 1u);
  }
}

void checkPreservedSignedUnwind(const BinaryImage &Before,
                                const BinaryImage &After) {
  const auto OldCompact = macho_unwind::parseCompactUnwind(Before);
  const auto NewCompact = macho_unwind::parseCompactUnwind(After);
  ASSERT_EQ(OldCompact.ParseStatus, ExceptionParseStatus::Complete);
  ASSERT_EQ(NewCompact.ParseStatus, ExceptionParseStatus::Complete);
  const auto CleanupSymbol =
      llvm::find_if(Before.Symbols, [](const auto &Symbol) {
        return Symbol.Name == kCleanupSymbol;
      });
  ASSERT_NE(CleanupSymbol, Before.Symbols.end());
  const auto CleanupVA = CleanupSymbol->Addr;
  const auto CleanupFrame =
      llvm::find_if(OldCompact.Entries, [&](const auto &E) {
        return E.CodeRange.contains(CleanupVA);
      });
  ASSERT_NE(CleanupFrame, OldCompact.Entries.end());
  ASSERT_TRUE(CleanupFrame->HasLSDA);
  ASSERT_NE(CleanupFrame->NativeEncoding & macho_unwind::kPersonalityMask, 0u);
  ASSERT_GE(NewCompact.Entries.size(), OldCompact.Entries.size());
  ASSERT_LE(OldCompact.Personalities.size(), NewCompact.Personalities.size());
  ASSERT_LE(OldCompact.PersonalitySlots.size(),
            NewCompact.PersonalitySlots.size());
  EXPECT_TRUE(std::equal(OldCompact.Personalities.begin(),
                         OldCompact.Personalities.end(),
                         NewCompact.Personalities.begin()));
  EXPECT_TRUE(std::equal(OldCompact.PersonalitySlots.begin(),
                         OldCompact.PersonalitySlots.end(),
                         NewCompact.PersonalitySlots.begin()));
  for (const auto &Old : OldCompact.Entries) {
    const auto Found =
        llvm::find_if(NewCompact.Entries, [&](const auto &Entry) {
          return Entry.CodeRange.Begin == Old.CodeRange.Begin;
        });
    ASSERT_NE(Found, NewCompact.Entries.end());
    const auto &New = *Found;
    EXPECT_EQ(Old.CodeRange.Begin, New.CodeRange.Begin);
    EXPECT_EQ(Old.CodeRange.End, New.CodeRange.End);
    EXPECT_EQ(Old.NativeEncoding, New.NativeEncoding);
    EXPECT_EQ(Old.PersonalityVA, New.PersonalityVA);
    EXPECT_EQ(Old.ReturnAddressSigningOffset, New.ReturnAddressSigningOffset);
    EXPECT_EQ(Old.LSDAVA, New.LSDAVA);
  }
  ::testing::Test::RecordProperty(kCompactFramesProperty,
                                  std::to_string(OldCompact.Entries.size()));
  size_t SignedRows = 0;
  for (const auto &Function : Before.ExceptionMetadata.Functions) {
    if (!Function.Dwarf)
      continue;
    const auto &Old = *Function.Dwarf;
    const auto *New = After.ExceptionMetadata.findFunction(Old.InitialLocation);
    ASSERT_NE(New, nullptr);
    ASSERT_TRUE(New->Dwarf);
    EXPECT_EQ(Old.AddressRange, New->Dwarf->AddressRange);
    const auto *OldCIE = Before.ExceptionMetadata.findCIE(Old.CIESectionOffset);
    const auto *NewCIE =
        After.ExceptionMetadata.findCIE(New->Dwarf->CIESectionOffset);
    ASSERT_NE(OldCIE, nullptr);
    ASSERT_NE(NewCIE, nullptr);
    EXPECT_EQ(Old.LSDAVA, New->Dwarf->LSDAVA);
    EXPECT_EQ(OldCIE->PersonalitySlotVA, NewCIE->PersonalitySlotVA);
    for (uint64_t Offset = 0; Offset < Old.AddressRange;
         Offset += kInstructionBytes) {
      const auto A = dwarf_eh::evaluateAArch64ReturnAddressState(
          Arch::AArch64, *OldCIE, Old, Old.InitialLocation + Offset);
      const auto B = dwarf_eh::evaluateAArch64ReturnAddressState(
          Arch::AArch64, *NewCIE, *New->Dwarf, Old.InitialLocation + Offset);
      ASSERT_TRUE(A.State);
      ASSERT_TRUE(B.State);
      EXPECT_EQ(*A.State, *B.State);
      if (A.State->Kind != dwarf_eh::AArch64RASigningKind::Unsigned) {
        EXPECT_EQ(A.State->Key, dwarf_eh::AArch64RAKey::B);
        ++SignedRows;
      }
    }
  }
  EXPECT_GT(SignedRows, 0u);
  ::testing::Test::RecordProperty(kSignedRowsProperty,
                                  std::to_string(SignedRows));
}

class MachOArm64eNativeInstallation
    : public ::testing::TestWithParam<std::tuple<const char *, bool, bool>> {};

TEST_P(MachOArm64eNativeInstallation,
       AppleImageKeepsSignedFramesAndDyldRecipes) {
#ifndef __APPLE__
  GTEST_SKIP() << kMissingHost;
#else
  const auto [Optimization, AppendSegments, Threaded] = GetParam();
  RecordProperty(kThreadedProperty, Threaded);
  RecordProperty(kAppendProperty, AppendSegments);
  llvm::SmallString<kPathCapacity> Directory;
  const auto *Root = std::getenv(kArtifactEnvironment);
  if (Root) {
    ASSERT_FALSE(llvm::sys::fs::create_directories(Root));
    llvm::SmallString<kPathCapacity> Prefix(Root);
    llvm::sys::path::append(Prefix, kTemporaryPrefix);
    ASSERT_FALSE(llvm::sys::fs::make_absolute(Prefix));
    ASSERT_FALSE(llvm::sys::fs::createUniqueDirectory(Prefix, Directory));
    RecordProperty(kArtifactProperty, Directory.str().str());
  } else {
    ASSERT_FALSE(
        llvm::sys::fs::createUniqueDirectory(kTemporaryPrefix, Directory));
  }
  const auto Cleanup = llvm::scope_exit([&] {
    if (!Root)
      EXPECT_FALSE(llvm::sys::fs::remove_directories(Directory));
  });
  const auto Path = [&](llvm::StringRef Name) {
    llvm::SmallString<kPathCapacity> Result(Directory);
    llvm::sys::path::append(Result, Name);
    return Result.str().str();
  };
  const auto Source = Path(kSourceFile), DwarfSource = Path(kDwarfSourceFile),
             OriginalPath = Path(kOriginalFile),
             InstalledPath = Path(kInstalledFile), Log = Path(kToolLog),
             InfoPath = Path(kDyldInfoFile);
  const auto Write = [&](llvm::StringRef Filename, llvm::StringRef Bytes) {
    std::error_code Error;
    llvm::raw_fd_ostream Output(Filename, Error);
    if (!Error) {
      Output << Bytes;
      Output.close();
      Error = Output.error();
    }
    return Error;
  };
  ASSERT_FALSE(Write(Source, kSource));
  ASSERT_FALSE(Write(DwarfSource, kDwarfSource));
  const auto Run = [&](llvm::StringRef Tool,
                       llvm::ArrayRef<llvm::StringRef> Args,
                       llvm::StringRef Output = {}) {
    std::string Error;
    const std::optional<llvm::StringRef> Redirects[] = {
        std::nullopt, Output.empty() ? std::nullopt : std::optional(Output),
        Log};
    const auto Status = llvm::sys::ExecuteAndWait(
        Tool, Args, std::nullopt, Redirects, kToolTimeout, 0, &Error);
    auto Diagnostics = llvm::MemoryBuffer::getFile(Log);
    EXPECT_EQ(Status, 0) << Error
                         << (Diagnostics ? (*Diagnostics)->getBuffer().str()
                                         : std::string());
    return Status;
  };
  llvm::SmallVector<llvm::StringRef> Args{kCompiler};
  Args.append(std::begin(kCompileArgs), std::end(kCompileArgs));
  std::string LinkerOption;
  if (Threaded) {
    const auto LinkerPath = Path(kClassicLinkerPath);
    const llvm::StringRef FindArgs[] = {kXcrun, kFindTool, kClassicLinker};
    const std::optional<llvm::StringRef> Redirects[] = {std::nullopt,
                                                        LinkerPath, Log};
    if (llvm::sys::ExecuteAndWait(kXcrun, FindArgs, std::nullopt, Redirects,
                                  kToolTimeout))
      GTEST_SKIP() << kClassicMissing;
    auto Linker = llvm::MemoryBuffer::getFile(LinkerPath);
    ASSERT_TRUE(bool(Linker));
    LinkerOption =
        std::string(kLinkerOption) + (*Linker)->getBuffer().trim().str();
    Args.push_back(LinkerOption);
    Args.push_back(kLegacyDeployment);
    Args.push_back(kThreadedOption);
  } else {
    Args.push_back(kChainedOption);
  }
  Args.push_back(Optimization);
  RecordProperty(kOptimizationProperty, Optimization);
  Args.push_back(Source);
  Args.push_back(DwarfSource);
  Args.append(std::begin(kOutputArgs), std::end(kOutputArgs));
  Args.push_back(OriginalPath);
  ASSERT_EQ(Run(kCompiler, Args), 0);
  auto Input = llvm::MemoryBuffer::getFile(OriginalPath);
  ASSERT_TRUE(bool(Input));
  auto Reader = Loader::create(BinaryFormat::MachO);
  auto Before = Reader->loadBuffer((*Input)->getMemBufferRef());
  ASSERT_TRUE(bool(Before)) << llvm::toString(Before.takeError());
  ASSERT_TRUE(Before->isArm64e());
  auto Signature = macho_signature::inspect(Before->Raw);
  ASSERT_TRUE(bool(Signature)) << llvm::toString(Signature.takeError());
  ASSERT_TRUE(macho_signature::canTransactionallyAdHocResign(*Signature));
  const auto SymbolVA = [&](llvm::StringRef Name) -> va_t {
    for (const auto &Symbol : Before->Symbols)
      if (Symbol.Name == Name)
        return Symbol.Addr;
    return InvalidVA;
  };
  auto Slots = SymbolVA(kSlotSymbol);
  ASSERT_NE(Slots, InvalidVA);
  MachOPatcher Patcher;
  CompiledImage Replacement;
  if (AppendSegments) {
    using Kind = llvm::mc_rewrite::RewriteSectionKind;
    Replacement.Success = true;
    Replacement.InputKind = RewriteInputKind::ReplacementObject;
    Replacement.Format = BinaryFormat::MachO;
    Replacement.TargetArch = Arch::AArch64;
    Replacement.BaseVA =
        Patcher.plannedExecSegmentVA(Before->Raw, Replacement.TargetArch);
    ASSERT_NE(Replacement.BaseVA, InvalidVA);
    Replacement.Bytes.resize(kPageBytes + kSlotCount * kPointerBytes);
    writeLE<uint32_t>(Replacement.Bytes.data(), kReturnInstruction);
    CompiledSection Code, Data;
    Code.Name = kNewCodeSection;
    Code.VA = Replacement.BaseVA;
    Code.Size = kInstructionBytes;
    Code.Kind = Kind::Code;
    Data.Name = kNewDataSegment;
    Data.Offset = kPageBytes;
    Data.VA = Replacement.BaseVA + Data.Offset;
    Data.Size = kSlotCount * kPointerBytes;
    Data.Kind = Kind::WritableData;
    Replacement.Sections = {Code, Data};
    Slots = Data.VA;
  }
  std::vector<MachOAuthenticatedFixup> Fixups;
  for (const auto *Symbol : {kRebaseSymbol, kBindSymbol}) {
    const auto Found =
        Before->MachOAuthenticatedPointers.find(SymbolVA(Symbol));
    ASSERT_NE(Found, Before->MachOAuthenticatedPointers.end());
    for (uint8_t Key = 0; Key < kKeyCount; ++Key)
      for (bool AddressDiversity : {false, true}) {
        auto Recipe = Found->second;
        Recipe.EncodedValue.reset();
        if (!Recipe.Symbol.empty())
          Recipe.Symbol = kInstalledImportSymbol;
        else if (AppendSegments)
          Recipe.Target = Replacement.BaseVA;
        Recipe.Key = Key;
        Recipe.AddressDiversity = AddressDiversity;
        Fixups.push_back({Slots + Fixups.size() * kPointerBytes, Recipe});
      }
  }
  std::vector<uint8_t> Bytes((*Input)->getBufferStart(),
                             (*Input)->getBufferEnd());
  auto Error = AppendSegments
                   ? Patcher.appendReplacementImage(Bytes, Replacement, Fixups)
                   : installMachOAuthenticatedFixups(Bytes, Fixups);
  ASSERT_FALSE(bool(Error)) << llvm::toString(std::move(Error));
  ASSERT_FALSE(Write(InstalledPath, llvm::toStringRef(llvm::ArrayRef(Bytes))));
  Error = macho_signing::adHocResign(InstalledPath, Signature->Slices.front());
  ASSERT_FALSE(bool(Error)) << llvm::toString(std::move(Error));
  Error = macho_signing::verifyStrict(InstalledPath);
  ASSERT_FALSE(bool(Error)) << llvm::toString(std::move(Error));
  Args.assign({kXcrun});
  if (Threaded)
    Args.append(std::begin(kThreadedInfoArgs), std::end(kThreadedInfoArgs));
  else
    Args.append(std::begin(kInfoArgs), std::end(kInfoArgs));
  Args.push_back(InstalledPath);
  ASSERT_EQ(Run(kXcrun, Args, InfoPath), 0);
  auto Info = llvm::MemoryBuffer::getFile(InfoPath);
  ASSERT_TRUE(bool(Info));
  EXPECT_TRUE((*Info)->getBuffer().contains(Threaded ? kThreadedHeading
                                                     : kFixupsHeading));
  if (!Threaded)
    checkAppleChainedRecipes((*Info)->getBuffer(), Bytes, Fixups);
  else {
    // Apple's legacy decoder drops the low key bit and reports only binds.
    llvm::SmallVector<llvm::StringRef> Lines;
    (*Info)->getBuffer().split(Lines, '\n');
    for (const auto &Fixup : Fixups) {
      if (Fixup.Recipe.Symbol.empty())
        continue;
      unsigned Matches = 0;
      for (auto Line : Lines) {
        llvm::SmallVector<llvm::StringRef> Fields;
        Line.trim().split(Fields, ' ', -1, false);
        if (Fields.size() != kThreadedInfoCount)
          continue;
        uint64_t Address;
        if (Fields[kThreadedInfoAddress].getAsInteger(0, Address) ||
            Address != Fixup.Address)
          continue;
        ++Matches;
        unsigned Diversity;
        ASSERT_FALSE(Fields[kThreadedInfoDiversity].getAsInteger(0, Diversity));
        EXPECT_EQ(Diversity, Fixup.Recipe.Diversity);
        EXPECT_EQ(Fields[kThreadedInfoAddressDiversity],
                  Fixup.Recipe.AddressDiversity ? kTrue : kFalse);
        uint64_t Pointer;
        ASSERT_FALSE(Fields[kThreadedInfoPointer].getAsInteger(0, Pointer));
        EXPECT_EQ((Pointer >> kThreadedPointerKeyShift) &
                      kThreadedPointerKeyMask,
                  Fixup.Recipe.Key);
        if (!(Fixup.Recipe.Key & kThreadedToolMissingKeyBit))
          EXPECT_EQ(Fields[kThreadedInfoKey], kKeyNames[Fixup.Recipe.Key]);
        EXPECT_EQ(Fields[kThreadedInfoSymbol], Fixup.Recipe.Symbol);
      }
      EXPECT_EQ(Matches, 1u);
    }
  }
  auto Installed = llvm::MemoryBuffer::getFile(InstalledPath);
  ASSERT_TRUE(bool(Installed));
  auto NewSignature = macho_signature::inspect(
      llvm::arrayRefFromStringRef((*Installed)->getBuffer()));
  ASSERT_TRUE(bool(NewSignature)) << llvm::toString(NewSignature.takeError());
  Error = macho_signature::validateTransactionallyAdHocResigned(*Signature,
                                                                *NewSignature);
  ASSERT_FALSE(bool(Error)) << llvm::toString(std::move(Error));
  Error = verifyMachOAuthenticatedFixups(
      llvm::arrayRefFromStringRef((*Installed)->getBuffer()), Fixups);
  ASSERT_FALSE(bool(Error)) << llvm::toString(std::move(Error));
  auto After = Reader->loadBuffer((*Installed)->getMemBufferRef());
  ASSERT_TRUE(bool(After)) << llvm::toString(After.takeError());
  EXPECT_EQ(macho_unwind::parseCompactUnwind(*Before).Entries.size(),
            macho_unwind::parseCompactUnwind(*After).Entries.size());
  checkPreservedSignedUnwind(*Before, *After);
#endif
}
INSTANTIATE_TEST_SUITE_P(AppleClang, MachOArm64eNativeInstallation,
                         ::testing::Combine(::testing::Values(kO0, kO2),
                                            ::testing::Bool(),
                                            ::testing::Bool()));

class MachOArm64eNativeObjectInstallation
    : public ::testing::TestWithParam<std::tuple<const char *, bool>> {};

enum class NativeObjectOperation {
  Append,
  Publish,
  PublishExecutable,
  PublishGeneratedIR,
  PublishGeneratedExecutable
};

void checkNativeObjectInstallation(
    const char *Optimization, bool Threaded, bool ExecuteNative,
    bool DwarfLanguage = false, bool FreshPersonality = false,
    bool RelocateOriginalLanguage = false, bool ConvertLegacy = false,
    NativeObjectOperation Operation = NativeObjectOperation::Append) {
#ifndef __APPLE__
  GTEST_SKIP() << kMissingHost;
#else
  const bool Generated =
      Operation == NativeObjectOperation::PublishGeneratedIR ||
      Operation == NativeObjectOperation::PublishGeneratedExecutable;
  const bool Executable =
      Operation == NativeObjectOperation::PublishExecutable ||
      Operation == NativeObjectOperation::PublishGeneratedExecutable;
  const auto *IRCompiler = std::getenv(kIRCompilerEnvironment);
  if (Generated && !IRCompiler)
    GTEST_SKIP() << kMissingIRCompiler;
  if (Generated)
    ASSERT_TRUE(llvm::sys::fs::can_execute(IRCompiler));
  const bool Publish = Operation != NativeObjectOperation::Append;
  if (Executable && ExecuteNative && !std::getenv(kExecutableEnvironment))
    GTEST_SKIP() << kExecutableUnavailable;
  ::testing::Test::RecordProperty(kOptimizationProperty, Optimization);
  ::testing::Test::RecordProperty(kThreadedProperty, Threaded);
  llvm::SmallString<kPathCapacity> Directory;
  const auto *Root = std::getenv(kArtifactEnvironment);
  if (Root) {
    ASSERT_FALSE(llvm::sys::fs::create_directories(Root));
    llvm::SmallString<kPathCapacity> Prefix(Root);
    llvm::sys::path::append(Prefix, kObjectArtifactPrefix);
    ASSERT_FALSE(llvm::sys::fs::make_absolute(Prefix));
    ASSERT_FALSE(llvm::sys::fs::createUniqueDirectory(Prefix, Directory));
    ::testing::Test::RecordProperty(kArtifactProperty, Directory.str().str());
  } else
    ASSERT_FALSE(
        llvm::sys::fs::createUniqueDirectory(kObjectArtifactPrefix, Directory));
  const auto Cleanup = llvm::scope_exit([&] {
    if (!Root)
      EXPECT_FALSE(llvm::sys::fs::remove_directories(Directory));
  });
  const auto Path = [&](llvm::StringRef Name) {
    llvm::SmallString<kPathCapacity> Value(Directory);
    llvm::sys::path::append(Value, Name);
    return Value.str().str();
  };
  const auto Write = [&](llvm::StringRef Filename, llvm::StringRef Bytes) {
    std::error_code Error;
    llvm::raw_fd_ostream Out(Filename, Error);
    if (!Error) {
      Out << Bytes;
      Out.close();
      Error = Out.error();
    }
    return Error;
  };
  const auto Source = Path(kSourceFile), Dwarf = Path(kDwarfSourceFile),
             ObjectSource = Path(kObjectSourceFile),
             IRPath = Path(kObjectIRFile), ObjectPath = Path(kObjectFile),
             OriginalPath = Path(kOriginalFile),
             InstalledPath = Path(kInstalledFile), Log = Path(kToolLog),
             InfoPath = Path(kDyldInfoFile);
  const auto Run =
      [&](llvm::StringRef Tool, llvm::ArrayRef<llvm::StringRef> Args,
          llvm::StringRef Output = {}) {
        // ExecuteAndWait redirects without truncating existing files on this
        // host. A marker from the previous child must not satisfy this run.
        if (!Output.empty())
          EXPECT_FALSE(Write(Output, {}));
        EXPECT_FALSE(Write(Log, {}));
        const std::optional<llvm::StringRef> Redirects[] = {
            std::nullopt, Output.empty() ? std::nullopt : std::optional(Output),
            Log};
        std::string Error;
        const int Status = llvm::sys::ExecuteAndWait(
            Tool, Args, std::nullopt, Redirects, kToolTimeout, 0, &Error);
        auto Diagnostic = llvm::MemoryBuffer::getFile(Log);
        EXPECT_EQ(Status, 0)
            << Error
            << (Diagnostic ? (*Diagnostic)->getBuffer().str() : std::string());
        return Status;
      };
  ASSERT_FALSE(Write(Source, kSource));
  ASSERT_FALSE(Write(Dwarf, kDwarfSource));
  ASSERT_FALSE(Write(ObjectSource, kObjectSource));
  llvm::SmallVector<llvm::StringRef> Args{kCompiler};
  const auto ProviderSource = Path(kRuntimeProviderSourceFile),
             RuntimeSource = Path(kRuntimeHarnessFile),
             ControlPath = Path(kRuntimeControlFile),
             LoaderSource = Path(kRuntimeLoaderFile),
             LoaderPath = Path(kRuntimeLoaderBinary),
             DriverPath = Path(kRuntimeDriverFile),
             ProviderPath = Path(kRuntimeProviderFile),
             RuntimeOutput = Path(kRuntimeOutputFile);
  const std::string RpathOption =
      std::string(kRuntimeRpathPrefix) + Directory.str().str();
  if (ExecuteNative || Executable)
    ASSERT_FALSE(Write(RuntimeSource, kRuntimeHarness));
  if (ExecuteNative) {
    ASSERT_FALSE(Write(ProviderSource, kRuntimeProviderSource));
    ASSERT_FALSE(Write(LoaderSource, kRuntimeLoaderSource));
    Args.append(std::begin(kRuntimeCompileArgs), std::end(kRuntimeCompileArgs));
    Args.append(std::begin(kProviderCompileArgs),
                std::end(kProviderCompileArgs));
    if (DwarfLanguage)
      Args.push_back(kDwarfLanguageOption);
    Args.append({Optimization, kRuntimeControlOption, Source, ObjectSource,
                 ProviderSource, RuntimeSource});
    Args.append(std::begin(kOutputArgs), std::end(kOutputArgs));
    Args.push_back(ControlPath);
    ASSERT_EQ(Run(kCompiler, Args), 0);
    Args.assign({kCompiler});
    Args.append(std::begin(kRuntimeCompileArgs), std::end(kRuntimeCompileArgs));
    Args.append({Optimization, LoaderSource});
    Args.append(std::begin(kOutputArgs), std::end(kOutputArgs));
    Args.push_back(LoaderPath);
    ASSERT_EQ(Run(kCompiler, Args), 0);
    Args.assign({kCompiler});
    Args.append(std::begin(kRuntimeCompileArgs), std::end(kRuntimeCompileArgs));
    Args.append(std::begin(kProviderCompileArgs),
                std::end(kProviderCompileArgs));
    Args.append({Optimization, kRuntimeInstallName, ProviderSource});
    Args.append(std::begin(kOutputArgs), std::end(kOutputArgs));
    Args.push_back(ProviderPath);
    ASSERT_EQ(Run(kCompiler, Args), 0);
    Args.assign({kCompiler});
    Args.append(std::begin(kRuntimeCompileArgs), std::end(kRuntimeCompileArgs));
    Args.append({Optimization, RpathOption, RuntimeSource});
    Args.append(std::begin(kOutputArgs), std::end(kOutputArgs));
    Args.push_back(DriverPath);
    ASSERT_EQ(Run(kCompiler, Args), 0);
    auto Signed = macho_signing::verifyStrict(ControlPath);
    ASSERT_FALSE(bool(Signed)) << llvm::toString(std::move(Signed));
    Signed = macho_signing::verifyStrict(LoaderPath);
    ASSERT_FALSE(bool(Signed)) << llvm::toString(std::move(Signed));
    const auto CapabilitySource = Path(kRuntimeCapabilityFile);
    const auto CapabilityBinary = Path(kRuntimeCapabilityBinary);
    ASSERT_FALSE(Write(CapabilitySource, kRuntimeCapabilitySource));
    Args.assign({kCompiler});
    Args.append(std::begin(kRuntimeCompileArgs), std::end(kRuntimeCompileArgs));
    Args.append({Optimization, CapabilitySource});
    Args.append(std::begin(kOutputArgs), std::end(kOutputArgs));
    Args.push_back(CapabilityBinary);
    ASSERT_EQ(Run(kCompiler, Args), 0);
    const std::optional<llvm::StringRef> Redirects[] = {std::nullopt,
                                                        RuntimeOutput, Log};
    std::string Diagnostic;
    ASSERT_FALSE(Write(RuntimeOutput, {}));
    ASSERT_FALSE(Write(Log, {}));
    const int Capability = llvm::sys::ExecuteAndWait(
        CapabilityBinary, {CapabilityBinary}, std::nullopt, Redirects,
        kToolTimeout, 0, &Diagnostic);
    ::testing::Test::RecordProperty(kRuntimeCapabilityProperty, Capability);
    auto CapabilityOutput = llvm::MemoryBuffer::getFile(RuntimeOutput);
    const bool CapabilityStarted =
        CapabilityOutput && (*CapabilityOutput)
                                ->getBuffer()
                                .contains(kRuntimeCapabilityStartedMarker);
    if (Capability < 0 && !CapabilityStarted)
      GTEST_SKIP() << kRuntimeControlUnavailable << Capability << '\n'
                   << Diagnostic;
    ASSERT_TRUE(bool(CapabilityOutput));
    ASSERT_TRUE(CapabilityStarted);
    if (Capability == kRuntimePACUnavailable)
      GTEST_SKIP() << kRuntimeControlUnavailable << Capability;
    ASSERT_EQ(Capability, 0) << Diagnostic;
    ASSERT_TRUE(
        (*CapabilityOutput)->getBuffer().contains(kRuntimeCapabilityMarker));
    // Once launch and PAC enforcement work, a broken compiler-original
    // call/catch/cleanup is a failure, never an environmental skip.
    ASSERT_FALSE(Write(RuntimeOutput, {}));
    ASSERT_FALSE(Write(Log, {}));
    const int Status = llvm::sys::ExecuteAndWait(
        LoaderPath, {LoaderPath, ControlPath}, std::nullopt, Redirects,
        kToolTimeout, 0, &Diagnostic);
    ::testing::Test::RecordProperty(kRuntimeControlResult, Status);
    ASSERT_EQ(Status, 0) << Diagnostic;
    auto Output = llvm::MemoryBuffer::getFile(RuntimeOutput);
    ASSERT_TRUE(bool(Output));
    ASSERT_TRUE((*Output)->getBuffer().contains(kRuntimeMarker));
  }
  Args.assign({kCompiler});
  if (Executable)
    Args.append(std::begin(kExecutableCompileArgs),
                std::end(kExecutableCompileArgs));
  else
    Args.append(std::begin(kCompileArgs), std::end(kCompileArgs));
  if (RelocateOriginalLanguage)
    Args.push_back(kDwarfLanguageOption);
  std::string LinkerOption;
  const auto *LegacyRoot = std::getenv(kLegacyFixtureEnvironment);
  if (Threaded && LegacyRoot) {
    llvm::SmallString<kPathCapacity> LegacyPath(LegacyRoot);
    llvm::sys::path::append(LegacyPath, std::string(Optimization) +
                                            (RelocateOriginalLanguage
                                                 ? kLegacyDwarfSuffix
                                                 : kLegacyCompactSuffix));
    ASSERT_FALSE(llvm::sys::fs::copy_file(LegacyPath, OriginalPath));
    ::testing::Test::RecordProperty(kLegacyFixtureProperty,
                                    LegacyPath.str().str());
  } else {
    if (Threaded) {
      const auto LinkerFile = Path(kClassicLinkerPath);
      ASSERT_EQ(Run(kXcrun, {kXcrun, kFindTool, kClassicLinker}, LinkerFile),
                0);
      auto Linker = llvm::MemoryBuffer::getFile(LinkerFile);
      ASSERT_TRUE(bool(Linker));
      LinkerOption =
          std::string(kLinkerOption) + (*Linker)->getBuffer().trim().str();
      Args.append({LinkerOption, kLegacyDeployment, kThreadedOption});
    } else
      Args.push_back(kChainedOption);
    Args.append({Optimization, Source, Dwarf});
    if (Executable)
      Args.append({RpathOption, RuntimeSource});
    Args.append(std::begin(kOutputArgs), std::end(kOutputArgs));
    Args.push_back(OriginalPath);
    ASSERT_EQ(Run(kCompiler, Args), 0);
  }
  llvm::LLVMContext Context;
  std::unique_ptr<llvm::Module> GeneratedModule;
  const llvm::StringRef Producer = Generated ? IRCompiler : kCompiler;
  Args.assign({Producer});
  if (Generated)
    Args.append(std::begin(kIRCompileArgs), std::end(kIRCompileArgs));
  else
    Args.append(std::begin(kObjectCompileArgs), std::end(kObjectCompileArgs));
  if (DwarfLanguage)
    Args.push_back(kDwarfLanguageOption);
  Args.append({Optimization, ObjectSource});
  Args.append(std::begin(kOutputArgs), std::end(kOutputArgs));
  Args.push_back(Generated ? IRPath : ObjectPath);
  ASSERT_EQ(Run(Producer, Args), 0);
  if (Generated) {
    llvm::SMDiagnostic Diagnostic;
    GeneratedModule = llvm::parseIRFile(IRPath, Diagnostic, Context);
    ASSERT_TRUE(GeneratedModule) << Diagnostic.getMessage().str();
    auto Copy = llvm::CloneModule(*GeneratedModule);
    auto Compiled = Codegen().compile(*Copy, Arch::AArch64, BinaryFormat::MachO,
                                      Copy->getTargetTriple().str());
    ASSERT_TRUE(Compiled.Success);
    ASSERT_FALSE(Compiled.ObjectData.empty());
    ASSERT_FALSE(
        Write(ObjectPath, llvm::StringRef(reinterpret_cast<const char *>(
                                              Compiled.ObjectData.data()),
                                          Compiled.ObjectData.size())));
    ::testing::Test::RecordProperty(kGeneratedIRProperty, true);
    ::testing::Test::RecordProperty(kIRCompilerProperty, IRCompiler);
  }
  Args.assign({kXcrun});
  Args.append(std::begin(kObjectDisassemblyArgs),
              std::end(kObjectDisassemblyArgs));
  Args.push_back(ObjectPath);
  const auto DisassemblyPath = Path(kObjectDisassembly);
  ASSERT_EQ(Run(kXcrun, Args, DisassemblyPath), 0);
  auto Disassembly = llvm::MemoryBuffer::getFile(DisassemblyPath);
  ASSERT_TRUE(bool(Disassembly));
  EXPECT_TRUE((*Disassembly)->getBuffer().contains(kNativeCallA));
  EXPECT_TRUE((*Disassembly)->getBuffer().contains(kNativeCallB));
  EXPECT_TRUE((*Disassembly)->getBuffer().contains(kNativeReturn));
  auto Input = llvm::MemoryBuffer::getFile(OriginalPath);
  auto Object = llvm::MemoryBuffer::getFile(ObjectPath);
  ASSERT_TRUE(bool(Input));
  ASSERT_TRUE(bool(Object));
  std::vector<uint8_t> Bytes((*Input)->getBufferStart(),
                             (*Input)->getBufferEnd());
  auto Loader = neverd::Loader::create(BinaryFormat::MachO);
  auto Before = Loader->loadBuffer((*Input)->getMemBufferRef());
  ASSERT_TRUE(bool(Before)) << llvm::toString(Before.takeError());
  auto Signature = macho_signature::inspect(Bytes);
  ASSERT_TRUE(bool(Signature)) << llvm::toString(Signature.takeError());
  ::testing::Test::RecordProperty(
      kSignaturePageBefore, Signature->Slices.front().CodeDirectoryPageSize);
  ::testing::Test::RecordProperty(kOriginalSubtypeProperty,
                                  std::to_string(Before->MachOCPUSubtype));
  ::testing::Test::RecordProperty(
      kObjectSubtypeProperty,
      std::to_string(reinterpret_cast<const llvm::MachO::mach_header_64 *>(
                         (*Object)->getBufferStart())
                         ->cpusubtype));
  if (Executable && ExecuteNative) {
    auto Error = macho_signing::verifyStrict(OriginalPath);
    ASSERT_FALSE(bool(Error)) << llvm::toString(std::move(Error));
    ASSERT_EQ(Run(OriginalPath, {OriginalPath}, RuntimeOutput), 0);
    auto Output = llvm::MemoryBuffer::getFile(RuntimeOutput);
    ASSERT_TRUE(bool(Output));
    ASSERT_TRUE((*Output)->getBuffer().contains(kExecutableBaselineMarker));
  }
  if (RelocateOriginalLanguage) {
    auto Compact =
        reserveMachOSourceUnwind(Bytes, Arch::AArch64, 0, kObjectFunctionCount);
    ASSERT_TRUE(bool(Compact)) << llvm::toString(Compact.takeError());
    const auto Region = findMachOEHFrameRegion(Bytes);
    ASSERT_TRUE(Region);
    // Block in-place extension without making the request larger than
    // every other gap in the linked image. Keep the original terminator.
    const auto *Section = reinterpret_cast<const llvm::MachO::section_64 *>(
        Bytes.data() + Region->SectionHeaderOff);
    const uint64_t SectionEnd = Region->SectionFileOff + Section->size;
    ASSERT_LE(Region->AppendFileOff, SectionEnd);
    ASSERT_LE(SectionEnd, Region->LimitFileOff);
    const bool BlockTail = SectionEnd < Region->LimitFileOff;
    if (BlockTail) {
      ASSERT_EQ(Bytes[SectionEnd], 0u);
      Bytes[SectionEnd] = kOccupiedMetadataPadding;
    }
    auto Moved = reserveMachOSourceUnwind(
        Bytes, Arch::AArch64,
        SectionEnd - Region->AppendFileOff + kPointerBytes, 0);
    ASSERT_TRUE(bool(Moved)) << llvm::toString(Moved.takeError());
    ASSERT_TRUE(*Moved);
    const auto NewRegion = findMachOEHFrameRegion(Bytes);
    ASSERT_TRUE(NewRegion);
    ASSERT_NE(NewRegion->SectionVA, Region->SectionVA);
    if (BlockTail)
      EXPECT_EQ(Bytes[SectionEnd], kOccupiedMetadataPadding);
  }
  std::vector<SourcePatchDependency> Dependencies = {
      {kProviderLibrary, kProviderFunction,
       SourcePatchDependencyKind::Function},
      {kProviderLibrary, kProviderData, SourcePatchDependencyKind::Data}};
  if (FreshPersonality)
    Dependencies.push_back(
        {kCxxLibrary, kCxxPersonality, SourcePatchDependencyKind::Function});
  if (!Executable && !RelocateOriginalLanguage && !Threaded &&
      llvm::StringRef(Optimization) == kO0) {
    uint64_t Tail = 0, TextEnd = 0;
    forEachMachOLoadCommand(
        Bytes.data(), Bytes.size(),
        [&](const uint8_t *C, uint32_t ID, uint32_t, bool) {
          if (ID != llvm::MachO::LC_SEGMENT_64)
            return;
          const auto *Segment =
              reinterpret_cast<const llvm::MachO::segment_command_64 *>(C);
          if (readMachOName(Segment->segname) != kOriginalTextSegment)
            return;
          TextEnd = Segment->fileoff + Segment->filesize;
          const auto *Sections =
              reinterpret_cast<const llvm::MachO::section_64 *>(Segment + 1);
          for (unsigned I = 0; I < Segment->nsects; ++I)
            if (Sections[I].offset)
              Tail = std::max(Tail, Sections[I].offset + Sections[I].size);
        });
    ASSERT_LT(Tail, TextEnd);
    ASSERT_TRUE(llvm::all_of(llvm::ArrayRef(Bytes).slice(Tail, TextEnd - Tail),
                             [](uint8_t Byte) { return Byte == 0; }));
    const auto RejectReservation = [&](std::vector<uint8_t> Candidate) {
      // This linker profile reserves header padding. Occupying only the TEXT
      // tail leaves a valid, independent location for the compact registry.
      const auto Header = parseMachOHeader(Candidate.data(), Candidate.size());
      const auto End = Header.HeaderSize + Header.SizeOfCmds;
      const auto First =
          findFirstMachODataOffset(Candidate.data(), Candidate.size());
      ASSERT_LE(End, First);
      std::fill(Candidate.begin() + End, Candidate.begin() + First,
                kOccupiedMetadataPadding);
      const auto Snapshot = Candidate;
      auto Grown = reserveMachOSourceUnwind(Candidate, Arch::AArch64, 0,
                                            kObjectFunctionCount);
      EXPECT_FALSE(bool(Grown));
      llvm::consumeError(Grown.takeError());
      EXPECT_EQ(Candidate, Snapshot);
    };
    auto Occupied = Bytes;
    std::fill(Occupied.begin() + Tail, Occupied.begin() + TextEnd,
              kOccupiedMetadataPadding);
    RejectReservation(std::move(Occupied));
    auto Noted = Bytes;
    const auto Header = parseMachOHeader(Noted.data(), Noted.size());
    const auto NoteOffset = Header.HeaderSize + Header.SizeOfCmds;
    ASSERT_LT(NoteOffset + sizeof(llvm::MachO::note_command),
              findFirstMachODataOffset(Noted.data(), Noted.size()));
    auto *Note = reinterpret_cast<llvm::MachO::note_command *>(Noted.data() +
                                                               NoteOffset);
    *Note = {};
    Note->cmd = llvm::MachO::LC_NOTE;
    Note->cmdsize = sizeof(*Note);
    std::copy(std::begin(kPaddingNoteOwner), std::end(kPaddingNoteOwner),
              Note->data_owner);
    Note->offset = Tail;
    Note->size = TextEnd - Tail;
    setMachOHeaderCmds(Noted.data(), true, Header.NCmds + 1,
                       Header.SizeOfCmds + sizeof(*Note));
    RejectReservation(std::move(Noted));
    std::optional<std::pair<uint64_t, uint64_t>> AuthField;
    std::optional<uint64_t> PlainRelocation, ExceptionTable;
    std::optional<uint64_t> PrivateWeakSymbol;
    auto ObjectBytes = llvm::arrayRefFromStringRef((*Object)->getBuffer());
    forEachMachOLoadCommand(
        ObjectBytes.data(), ObjectBytes.size(),
        [&](const uint8_t *C, uint32_t ID, uint32_t, bool) {
          if (ID == llvm::MachO::LC_SYMTAB) {
            const auto *Symbols =
                reinterpret_cast<const llvm::MachO::symtab_command *>(C);
            for (unsigned I = 0; I < Symbols->nsyms; ++I) {
              const auto Offset =
                  Symbols->symoff + I * sizeof(llvm::MachO::nlist_64);
              const auto *Symbol =
                  reinterpret_cast<const llvm::MachO::nlist_64 *>(
                      ObjectBytes.data() + Offset);
              if ((Symbol->n_desc & llvm::MachO::N_WEAK_DEF) &&
                  (Symbol->n_type & llvm::MachO::N_PEXT))
                PrivateWeakSymbol = Offset;
            }
          }
          if (ID != llvm::MachO::LC_SEGMENT_64)
            return;
          const auto *Segment =
              reinterpret_cast<const llvm::MachO::segment_command_64 *>(C);
          const auto *Sections =
              reinterpret_cast<const llvm::MachO::section_64 *>(Segment + 1);
          for (unsigned I = 0; I < Segment->nsects; ++I) {
            if (readMachOName(Sections[I].sectname) == kExceptionSection)
              ExceptionTable = Sections[I].offset;
            for (unsigned J = 0; J < Sections[I].nreloc; ++J) {
              const uint64_t Offset =
                  Sections[I].reloff +
                  J * sizeof(llvm::MachO::any_relocation_info) +
                  sizeof(uint32_t);
              const auto Info = readLE<uint32_t>(ObjectBytes.data() + Offset);
              if (readMachOName(Sections[I].segname) == kObjectDataSegment &&
                  (Info >> kObjectRelocTypeShift) ==
                      llvm::MachO::ARM64_RELOC_UNSIGNED)
                PlainRelocation = Offset;
            }
            if (readMachOName(Sections[I].segname) == kObjectConstSegment &&
                Sections[I].nreloc)
              AuthField = {
                  Sections[I].offset +
                      readLE<uint32_t>(ObjectBytes.data() + Sections[I].reloff),
                  Sections[I].reloff};
          }
        });
    ASSERT_TRUE(AuthField);
    ASSERT_TRUE(PlainRelocation);
    ASSERT_TRUE(ExceptionTable);
    const auto Check = [&](auto Change) {
      auto Corrupt = (*Object)->getBuffer().str();
      Change(reinterpret_cast<uint8_t *>(Corrupt.data()));
      auto Candidate = Bytes;
      auto Rejected = appendMachOObjectImage(Candidate, Corrupt, Dependencies);
      EXPECT_FALSE(bool(Rejected));
      llvm::consumeError(Rejected.takeError());
      EXPECT_EQ(Candidate, Bytes);
    };
    Check([&](uint8_t *Data) {
      writeLE<uint64_t>(Data + AuthField->first,
                        readLE<uint64_t>(Data + AuthField->first) |
                            kObjectReservedBit);
    });
    Check([&](uint8_t *Data) {
      writeLE<uint64_t>(Data + AuthField->first,
                        readLE<uint64_t>(Data + AuthField->first) &
                            ~kObjectAuthenticationBit);
    });
    Check([&](uint8_t *Data) {
      auto *Flags = Data + AuthField->second + sizeof(uint32_t);
      writeLE<uint32_t>(Flags, readLE<uint32_t>(Flags) &
                                   ~kObjectExternalRelocationBit);
    });
    Check([&](uint8_t *Data) {
      reinterpret_cast<llvm::MachO::mach_header_64 *>(Data)->cpusubtype =
          llvm::MachO::CPU_SUBTYPE_ARM64_ALL;
    });
    Check([&](uint8_t *Data) {
      reinterpret_cast<llvm::MachO::mach_header_64 *>(Data)->cpusubtype ^=
          kObjectABIVersionBit;
    });
    Check([&](uint8_t *Data) {
      writeLE<uint32_t>(Data + *PlainRelocation,
                        readLE<uint32_t>(Data + *PlainRelocation) |
                            kObjectPCRelativeBit);
    });
    Check([&](uint8_t *Data) {
      Data[*ExceptionTable] = kUnsupportedLSDAEncoding;
    });
    ASSERT_TRUE(PrivateWeakSymbol);
    Check([&](uint8_t *Data) {
      reinterpret_cast<llvm::MachO::nlist_64 *>(Data + *PrivateWeakSymbol)
          ->n_type &= ~llvm::MachO::N_PEXT;
    });
  }
  if (Executable && !ExecuteNative) {
    // Exercise the modern main header even with a local compiler whose linker
    // still emits ABI0 executables. This is a format check, not execution
    // proof.
    auto Main = Bytes;
    reinterpret_cast<llvm::MachO::mach_header_64 *>(Main.data())->cpusubtype =
        kMacOSMainSubtype;
    auto Candidate = Main;
    auto Accepted =
        appendMachOObjectImage(Candidate, (*Object)->getBuffer(), Dependencies);
    ASSERT_TRUE(bool(Accepted)) << llvm::toString(Accepted.takeError());
    const auto Reject = [&](auto Change) {
      auto Candidate = Main;
      auto Native = (*Object)->getBuffer().str();
      Change(Candidate, Native);
      const auto Snapshot = Candidate;
      auto Result = appendMachOObjectImage(Candidate, Native, Dependencies);
      EXPECT_FALSE(bool(Result));
      llvm::consumeError(Result.takeError());
      EXPECT_EQ(Candidate, Snapshot);
    };
    Reject([](auto &Candidate, auto &) {
      reinterpret_cast<llvm::MachO::mach_header_64 *>(Candidate.data())
          ->filetype = llvm::MachO::MH_DYLIB;
    });
    for (const auto Subtype : {kUnsupportedObjectSubtype, kKernelMainSubtype})
      Reject([&](auto &Candidate, auto &) {
        reinterpret_cast<llvm::MachO::mach_header_64 *>(Candidate.data())
            ->cpusubtype = Subtype;
      });
    Reject([](auto &, auto &Native) {
      reinterpret_cast<llvm::MachO::mach_header_64 *>(Native.data())
          ->cpusubtype = kUnsupportedObjectSubtype;
    });
    const auto ChangePlatform = [](auto &Data) {
      unsigned Changed = 0;
      forEachMachOLoadCommand(
          reinterpret_cast<const uint8_t *>(Data.data()), Data.size(),
          [&](const uint8_t *Command, uint32_t ID, uint32_t, bool) {
            if (ID != llvm::MachO::LC_BUILD_VERSION)
              return;
            auto *Build =
                reinterpret_cast<llvm::MachO::build_version_command *>(
                    const_cast<uint8_t *>(Command));
            Build->platform = llvm::MachO::PLATFORM_IOS;
            ++Changed;
          });
      EXPECT_EQ(Changed, 1u);
    };
    Reject([&](auto &Candidate, auto &) { ChangePlatform(Candidate); });
    Reject([&](auto &, auto &Native) { ChangePlatform(Native); });
    uint64_t Tail = 0, End = 0, TailVA = 0, HeaderVA = 0;
    forEachMachOLoadCommand(
        Main.data(), Main.size(),
        [&](const uint8_t *Command, uint32_t ID, uint32_t, bool) {
          if (ID != llvm::MachO::LC_SEGMENT_64)
            return;
          const auto *Segment =
              reinterpret_cast<const llvm::MachO::segment_command_64 *>(
                  Command);
          if (!Segment->fileoff && Segment->filesize)
            HeaderVA = Segment->vmaddr;
          if (!(Segment->flags & llvm::MachO::SG_READ_ONLY))
            return;
          End = Segment->fileoff + Segment->filesize;
          const auto *Sections =
              reinterpret_cast<const llvm::MachO::section_64 *>(Segment + 1);
          for (unsigned I = 0; I < Segment->nsects; ++I)
            if (Sections[I].offset)
              Tail = std::max(Tail, Sections[I].offset + Sections[I].size);
          Tail = llvm::alignTo(Tail, kPointerBytes);
          TailVA = Segment->vmaddr + Tail - Segment->fileoff;
        });
    ASSERT_LT(Tail, End);
    ASSERT_TRUE(llvm::all_of(llvm::ArrayRef(Main).slice(Tail, End - Tail),
                             [](uint8_t Byte) { return Byte == 0; }));
    // A zero chain word rebasing to the image base still owns its storage,
    // even when it lies after the original section inventory.
    MachOAuthenticatedFixup Existing;
    Existing.Address = TailVA;
    Existing.Recipe.Target = HeaderVA;
    Existing.ReadOnlyAfterFixups = true;
    Existing.Authenticated = false;
    auto OwnedZero = Main;
    auto Error = installMachOAuthenticatedFixups(OwnedZero, {Existing});
    ASSERT_FALSE(bool(Error)) << llvm::toString(std::move(Error));
    ASSERT_EQ(readLE<uint64_t>(OwnedZero.data() + Tail), 0u);
    auto WithOwnedZero =
        appendMachOObjectImage(OwnedZero, (*Object)->getBuffer(), Dependencies);
    ASSERT_TRUE(bool(WithOwnedZero))
        << llvm::toString(WithOwnedZero.takeError());
    for (const auto &Section : WithOwnedZero->Sections)
      if (Section.ReadOnlyAfterFixups && Section.Size)
        EXPECT_GE(Section.VA, TailVA + kPointerBytes);
    Error = verifyMachOAuthenticatedFixups(OwnedZero, {Existing});
    EXPECT_FALSE(bool(Error)) << llvm::toString(std::move(Error));
    Reject([&](auto &Candidate, auto &) {
      std::fill(Candidate.begin() + Tail, Candidate.begin() + End,
                kOccupiedMetadataPadding);
    });
    Reject([&](auto &Candidate, auto &) {
      const auto Header = parseMachOHeader(Candidate.data(), Candidate.size());
      const auto Offset = Header.HeaderSize + Header.SizeOfCmds;
      ASSERT_LT(Offset + sizeof(llvm::MachO::note_command),
                findFirstMachODataOffset(Candidate.data(), Candidate.size()));
      auto *Note = reinterpret_cast<llvm::MachO::note_command *>(
          Candidate.data() + Offset);
      *Note = {};
      Note->cmd = llvm::MachO::LC_NOTE;
      Note->cmdsize = sizeof(*Note);
      std::copy(std::begin(kPaddingNoteOwner), std::end(kPaddingNoteOwner),
                Note->data_owner);
      Note->offset = Tail;
      Note->size = End - Tail;
      setMachOHeaderCmds(Candidate.data(), true, Header.NCmds + 1,
                         Header.SizeOfCmds + sizeof(*Note));
    });
  }
  const auto BeforeInstallation = Bytes;
  std::vector<SourcePatchMapping> Mappings;
  if (Publish) {
    const char *Names[] = {kObjectDispatch, kObjectCatch, kObjectCleanup};
    for (unsigned I = 0; I != std::size(Names); ++I) {
      auto Found = llvm::find_if(Before->Symbols, [&](const auto &Symbol) {
        return Symbol.Name == kPublishedEntries[I];
      });
      ASSERT_NE(Found, Before->Symbols.end());
      Mappings.push_back({Names[I], Found->Addr});
    }
  }
  auto Image =
      appendMachOObjectImage(Bytes, (*Object)->getBuffer(), Dependencies);
  ASSERT_TRUE(bool(Image)) << llvm::toString(Image.takeError());
  ASSERT_TRUE(Image->Success);
  if (DwarfLanguage && !ExecuteNative) {
    // PTR_TO_GOT has no addend. Changing only its unresolved assembler word
    // must produce the same placed bytes, even when that word underflows the
    // object's logical address before linking.
    auto Variant = (*Object)->getBuffer().str();
    auto *Data = reinterpret_cast<uint8_t *>(Variant.data());
    unsigned Changed = 0;
    forEachMachOLoadCommand(
        Data, Variant.size(),
        [&](const uint8_t *C, uint32_t ID, uint32_t, bool) {
          if (ID != llvm::MachO::LC_SEGMENT_64)
            return;
          const auto *Segment =
              reinterpret_cast<const llvm::MachO::segment_command_64 *>(C);
          const auto *Sections =
              reinterpret_cast<const llvm::MachO::section_64 *>(Segment + 1);
          for (unsigned I = 0; I < Segment->nsects; ++I) {
            const auto &Section = Sections[I];
            if (readMachOName(Section.sectname) != kEHFrameSection)
              continue;
            for (unsigned J = 0; J < Section.nreloc; ++J) {
              const auto *Relocation =
                  Data + Section.reloff +
                  J * sizeof(llvm::MachO::any_relocation_info);
              if ((readLE<uint32_t>(Relocation + sizeof(uint32_t)) >>
                   kObjectRelocTypeShift) !=
                  llvm::MachO::ARM64_RELOC_POINTER_TO_GOT)
                continue;
              writeLE<uint32_t>(Data + Section.offset +
                                    readLE<uint32_t>(Relocation),
                                kPersonalityPlaceholder);
              ++Changed;
            }
          }
        });
    ASSERT_GT(Changed, 0u);
    auto Candidate = BeforeInstallation;
    auto Placed = appendMachOObjectImage(Candidate, Variant, Dependencies);
    ASSERT_TRUE(bool(Placed)) << llvm::toString(Placed.takeError());
    EXPECT_EQ(Candidate, Bytes);
    EXPECT_EQ(Placed->SymbolAddrs, Image->SymbolAddrs);
  }
  forEachMachOLoadCommand(
      Bytes.data(), Bytes.size(),
      [&](const uint8_t *C, uint32_t ID, uint32_t, bool) {
        if (ID != llvm::MachO::LC_SEGMENT_64)
          return;
        const auto *Segment =
            reinterpret_cast<const llvm::MachO::segment_command_64 *>(C);
        const auto *Sections =
            reinterpret_cast<const llvm::MachO::section_64 *>(Segment + 1);
        for (unsigned I = 1; I < Segment->nsects; ++I)
          EXPECT_LE(Sections[I - 1].addr + Sections[I - 1].size,
                    Sections[I].addr);
      });
  EXPECT_EQ(Image->InputKind, RewriteInputKind::ReplacementObject);
  auto Recipes = collectMachOAuthenticatedFixups(Bytes, *Image, Dependencies);
  ASSERT_TRUE(bool(Recipes)) << llvm::toString(Recipes.takeError());
  EXPECT_EQ(
      llvm::count_if(*Recipes, [](const auto &R) { return R.Authenticated; }),
      kKeyCount + unsigned(FreshPersonality));
  const auto TypeBinds = llvm::count_if(*Recipes, [](const auto &R) {
    return !R.Authenticated && R.Recipe.Symbol == kTypeInfo;
  });
  EXPECT_LE(TypeBinds, 1);
  EXPECT_EQ(Recipes->size(), kKeyCount + kOrdinaryObjectPointers + TypeBinds +
                                 unsigned(FreshPersonality));
  const char *Slots[] = {kObjectLocalCode, kObjectExternalCode,
                         kObjectLocalData, kObjectExternalData};
  for (unsigned Key = 0; Key != kKeyCount; ++Key) {
    ASSERT_TRUE(Image->SymbolAddrs.contains(Slots[Key]));
    const auto Address = Image->SymbolAddrs.at(Slots[Key]);
    const auto Fixup = llvm::find_if(
        *Recipes, [&](const auto &R) { return R.Address == Address; });
    ASSERT_NE(Fixup, Recipes->end());
    EXPECT_EQ(Fixup->Recipe.Key, Key);
    EXPECT_EQ(Fixup->Recipe.Diversity, kObjectDiversity);
    EXPECT_TRUE(Fixup->Recipe.AddressDiversity);
    if (Key == 0)
      EXPECT_EQ(Fixup->Recipe.Target, Image->SymbolAddrs.at(kObjectFunction));
    else if (Key == 2)
      EXPECT_EQ(Fixup->Recipe.Target,
                Image->SymbolAddrs.at(kObjectData) + kObjectDataAddend);
    else {
      EXPECT_EQ(Fixup->Recipe.Module, kProviderLibrary);
      EXPECT_EQ(Fixup->Recipe.Symbol,
                Key == 1 ? kProviderNativeFunction : kProviderNativeData);
      EXPECT_EQ(Fixup->Recipe.Addend, Key == 1 ? 0 : kObjectDataAddend);
    }
  }
  for (const auto *Name : {kPlainLocal, kPlainExternal}) {
    const auto Address = Image->SymbolAddrs.at(Name);
    auto Fixup = llvm::find_if(
        *Recipes, [&](const auto &R) { return R.Address == Address; });
    ASSERT_NE(Fixup, Recipes->end());
    EXPECT_FALSE(Fixup->Authenticated);
    if (llvm::StringRef(Name) == kPlainLocal)
      EXPECT_EQ(Fixup->Recipe.Target,
                Image->SymbolAddrs.at(kObjectData) + kObjectDataAddend);
    else {
      EXPECT_EQ(Fixup->Recipe.Symbol, kProviderNativeData);
      EXPECT_EQ(Fixup->Recipe.Addend, kObjectDataAddend);
    }
  }
  unsigned ReadOnly = 0, Mutable = 0, TotalReadOnly = 0;
  forEachMachOLoadCommand(
      Bytes.data(), Bytes.size(),
      [&](const uint8_t *C, uint32_t ID, uint32_t, bool) {
        if (ID != llvm::MachO::LC_SEGMENT_64)
          return;
        const auto *S =
            reinterpret_cast<const llvm::MachO::segment_command_64 *>(C);
        const auto Name = readMachOName(S->segname);
        TotalReadOnly += (S->flags & llvm::MachO::SG_READ_ONLY) != 0;
        if (Name == kNewConstSegment) {
          ++ReadOnly;
          EXPECT_EQ(S->flags, kReadOnlySegmentFlag);
          EXPECT_EQ(S->initprot,
                    llvm::MachO::VM_PROT_READ | llvm::MachO::VM_PROT_WRITE);
          EXPECT_EQ(S->vmaddr % kPageBytes, 0u);
          EXPECT_EQ(S->vmsize % kPageBytes, 0u);
        }
        if (Name == kNewDataSegment) {
          ++Mutable;
          EXPECT_EQ(S->flags, 0u);
        }
      });
  EXPECT_EQ(ReadOnly, Executable ? 0u : 1u);
  if (Executable)
    EXPECT_EQ(TotalReadOnly, 1u);
  EXPECT_EQ(Mutable, 1u);
  const bool Converted = Threaded && (ExecuteNative || ConvertLegacy);
  if (Converted) {
    auto Error = convertMachOThreadedFixupsToChains(Bytes);
    ASSERT_FALSE(bool(Error)) << llvm::toString(std::move(Error));
  }
  ::testing::Test::RecordProperty(kConvertedThreadedProperty, Converted);
  if (Publish) {
    const auto Snapshot = Before->Raw;
    bool ReceiptSeen = false;
    if (Generated) {
      const auto Triple = GeneratedModule->getTargetTriple();
      GeneratedModule->setTargetTriple(llvm::Triple(kWrongIRTriple));
      auto Wrong = bindSourcePatch(*GeneratedModule, *Before, Mappings);
      EXPECT_FALSE(bool(Wrong));
      llvm::consumeError(Wrong.takeError());
      GeneratedModule->setTargetTriple(Triple);
      auto Bound = bindSourcePatch(*GeneratedModule, *Before, Mappings);
      ASSERT_TRUE(bool(Bound)) << llvm::toString(Bound.takeError());
      Mappings = std::move(*Bound);
      auto Foreign = *Before;
      Foreign.DynInfo.MachOPlatforms = {llvm::MachO::PLATFORM_IOS};
      auto BadPlatform = sourcePatchTargetTriple(Foreign);
      EXPECT_FALSE(bool(BadPlatform));
      llvm::consumeError(BadPlatform.takeError());
      Foreign = *Before;
      Foreign.MachOCPUSubtype = kUnsupportedObjectSubtype;
      auto BadABI = sourcePatchTargetTriple(Foreign);
      EXPECT_FALSE(bool(BadABI));
      llvm::consumeError(BadABI.takeError());
      auto Analysis = llvm::CloneModule(*GeneratedModule);
      Analysis->addModuleFlag(llvm::Module::Error, kArm64eAnalysisModuleFlag,
                              true);
      auto Refused =
          installSourcePatch(*Analysis, *Before, Mappings, InstalledPath, {},
                             {}, nullptr, Dependencies);
      EXPECT_FALSE(bool(Refused));
      llvm::consumeError(Refused.takeError());
      auto WrongProviders = Dependencies;
      WrongProviders.front().Kind = SourcePatchDependencyKind::Data;
      auto BadProvider =
          installSourcePatch(*GeneratedModule, *Before, Mappings, InstalledPath,
                             {}, {}, nullptr, WrongProviders);
      EXPECT_FALSE(bool(BadProvider));
      llvm::consumeError(BadProvider.takeError());
      EXPECT_FALSE(llvm::sys::fs::exists(InstalledPath));
      EXPECT_EQ(Before->Raw, Snapshot);
    }
    const auto Install =
        [&](llvm::function_ref<llvm::Error(const PatchResult &)> Receipt = {}) {
          if (Generated)
            return installSourcePatch(*GeneratedModule, *Before, Mappings,
                                      InstalledPath, {}, Receipt, nullptr,
                                      Dependencies);
          return installMachOObjectPatch((*Object)->getBuffer(), *Before,
                                         Mappings, {}, InstalledPath, Receipt,
                                         nullptr, nullptr, Dependencies);
        };
    auto Rejected = Install([&](const PatchResult &) -> llvm::Error {
      ReceiptSeen = true;
      return llvm::createStringError(llvm::errc::not_supported,
                                     kReceiptRejected);
    });
    ASSERT_FALSE(bool(Rejected));
    EXPECT_EQ(llvm::toString(Rejected.takeError()), kReceiptRejected);
    EXPECT_TRUE(ReceiptSeen);
    EXPECT_FALSE(llvm::sys::fs::exists(InstalledPath));
    auto Published = Install();
    ASSERT_TRUE(bool(Published)) << llvm::toString(Published.takeError());
    ASSERT_TRUE(Published->Success);
    EXPECT_EQ(Published->TrampolineCount, Mappings.size());
    for (const auto &Entry : Published->PatchedFunctions)
      EXPECT_EQ(Entry.OwnerVA, Image->SymbolAddrs.at(Entry.SourceFunction));
    EXPECT_EQ(Before->Raw, Snapshot);
    auto Again = Install();
    EXPECT_FALSE(bool(Again));
    llvm::consumeError(Again.takeError());
    ::testing::Test::RecordProperty(kPublishedProperty, true);
    ::testing::Test::RecordProperty(kExecutableProperty, Executable);
  } else {
    ASSERT_FALSE(
        Write(InstalledPath, llvm::toStringRef(llvm::ArrayRef(Bytes))));
    auto Error =
        macho_signing::adHocResign(InstalledPath, Signature->Slices.front());
    ASSERT_FALSE(bool(Error)) << llvm::toString(std::move(Error));
  }
  auto Error = macho_signing::verifyStrict(InstalledPath);
  ASSERT_FALSE(bool(Error)) << llvm::toString(std::move(Error));
  Args.assign({kXcrun});
  if (Threaded && !Converted)
    Args.append(std::begin(kThreadedInfoArgs), std::end(kThreadedInfoArgs));
  else
    Args.append(std::begin(kInfoArgs), std::end(kInfoArgs));
  Args.push_back(InstalledPath);
  ASSERT_EQ(Run(kXcrun, Args, InfoPath), 0);
  auto Info = llvm::MemoryBuffer::getFile(InfoPath);
  ASSERT_TRUE(bool(Info));
  EXPECT_TRUE((*Info)->getBuffer().contains(kProviderNativeFunction));
  EXPECT_TRUE((*Info)->getBuffer().contains(kProviderNativeData));
  if (!Threaded || Converted)
    checkAppleChainedRecipes((*Info)->getBuffer(), Bytes, *Recipes);
  Input = llvm::MemoryBuffer::getFile(InstalledPath);
  ASSERT_TRUE(bool(Input));
  auto Final = llvm::arrayRefFromStringRef((*Input)->getBuffer());
  if (Publish) {
    auto FinalSignature = macho_signature::inspect(Final);
    ASSERT_TRUE(bool(FinalSignature))
        << llvm::toString(FinalSignature.takeError());
    ::testing::Test::RecordProperty(
        kSignaturePageAfter,
        FinalSignature->Slices.front().CodeDirectoryPageSize);
    EXPECT_EQ(FinalSignature->Slices.front().CodeDirectoryPageSize,
              Signature->Slices.front().CodeDirectoryPageSize);
  }
  Error = verifyMachOAuthenticatedFixups(Final, *Recipes);
  ASSERT_FALSE(bool(Error)) << llvm::toString(std::move(Error));
  auto MissingProtection = std::vector<uint8_t>(Final.begin(), Final.end());
  forEachMachOLoadCommand(
      MissingProtection.data(), MissingProtection.size(),
      [&](const uint8_t *C, uint32_t ID, uint32_t, bool) {
        if (ID == llvm::MachO::LC_SEGMENT_64) {
          const auto *S =
              reinterpret_cast<const llvm::MachO::segment_command_64 *>(C);
          if (S->flags & llvm::MachO::SG_READ_ONLY)
            writeLE<uint32_t>(
                MissingProtection.data() + (C - MissingProtection.data()) +
                    offsetof(llvm::MachO::segment_command_64, flags),
                0);
        }
      });
  Error = verifyMachOAuthenticatedFixups(MissingProtection, *Recipes);
  EXPECT_TRUE(bool(Error));
  llvm::consumeError(std::move(Error));
  auto Parsed = Loader->loadBuffer((*Input)->getMemBufferRef());
  ASSERT_TRUE(bool(Parsed)) << llvm::toString(Parsed.takeError());
  for (const auto &Fixup : *Recipes) {
    const auto *Section = Parsed->getSectionFor(Fixup.Address);
    ASSERT_NE(Section, nullptr) << Fixup.Address;
    EXPECT_TRUE(rangeInBounds(Fixup.Address - Section->VA, kPointerBytes,
                              Section->FileSz));
    if (Section->SegmentName == kNewConstSegment)
      EXPECT_EQ(Section->Name, section_names::macho::Const);
    if (Section->SegmentName == kNewDataSegment)
      EXPECT_EQ(Section->Name, section_names::macho::Data);
  }
  checkPreservedSignedUnwind(*Before, *Parsed);
  if (RelocateOriginalLanguage) {
    for (const auto *Name : {kCleanupSymbol, kOriginalCatch}) {
      const auto Symbol = llvm::find_if(
          Before->Symbols, [&](const auto &S) { return S.Name == Name; });
      ASSERT_NE(Symbol, Before->Symbols.end());
      const auto *Old = Before->ExceptionMetadata.findFunction(Symbol->Addr);
      const auto *New = Parsed->ExceptionMetadata.findFunction(Symbol->Addr);
      ASSERT_NE(Old, nullptr);
      ASSERT_NE(New, nullptr);
      ASSERT_TRUE(Old->Dwarf);
      ASSERT_TRUE(New->Dwarf);
      ASSERT_TRUE(Old->Itanium);
      ASSERT_TRUE(New->Itanium);
      EXPECT_EQ(New->ParseStatus, ExceptionParseStatus::Complete);
      EXPECT_EQ(New->CodeRange.Begin, Old->CodeRange.Begin);
      EXPECT_EQ(New->CodeRange.End, Old->CodeRange.End);
      EXPECT_EQ(New->Personality, Old->Personality);
      EXPECT_EQ(New->Dwarf->LSDAVA, Old->Dwarf->LSDAVA);
      EXPECT_EQ(New->HandlerDataVA, Old->HandlerDataVA);
      EXPECT_EQ(New->Itanium->CallSites.size(), Old->Itanium->CallSites.size());
      EXPECT_EQ(New->Itanium->TypeTable.size(), Old->Itanium->TypeTable.size());
      const auto *OldCIE =
          Before->ExceptionMetadata.findCIE(Old->Dwarf->CIESectionOffset);
      const auto *NewCIE =
          Parsed->ExceptionMetadata.findCIE(New->Dwarf->CIESectionOffset);
      ASSERT_NE(OldCIE, nullptr);
      ASSERT_NE(NewCIE, nullptr);
      EXPECT_EQ(NewCIE->PersonalitySlotVA, OldCIE->PersonalitySlotVA);
      EXPECT_TRUE(NewCIE->UsesArm64eReturnAddressABI);
    }
    ::testing::Test::RecordProperty(kOriginalDwarfProperty,
                                    kOriginalLanguageFrames);
  }
  for (const auto &Recipe : *Recipes) {
    if (Recipe.Recipe.Symbol.empty() || Recipe.Recipe.Addend)
      continue;
    EXPECT_EQ(resolveRoutineName(*Parsed, 0, Recipe.Address),
              Recipe.Recipe.Symbol);
    EXPECT_FALSE(Parsed->ImportPtrSlots.contains(Recipe.Address));
    if (Recipe.Authenticated)
      EXPECT_FALSE(Parsed->ImportStorageSlots.contains(Recipe.Address));
  }
  for (const auto &Section : Image->Sections) {
    if (Section.Kind != llvm::mc_rewrite::RewriteSectionKind::Code)
      continue;
    const auto *Installed = Parsed->readVA(Section.VA, Section.Size);
    ASSERT_NE(Installed, nullptr);
    EXPECT_TRUE(std::equal(Image->Bytes.begin() + Section.Offset,
                           Image->Bytes.begin() + Section.Offset + Section.Size,
                           Installed));
  }
  if (Executable) {
    EXPECT_EQ(Parsed->DynInfo.MachOFileType, llvm::MachO::MH_EXECUTE);
    EXPECT_EQ(Parsed->MachOCPUSubtype, Before->MachOCPUSubtype);
    EXPECT_EQ(Parsed->Entry, Before->Entry);
    EXPECT_NE(Parsed->Entry, 0u);
  }
  const auto Native = macho_unwind::parseCompactUnwind(*Parsed);
  ASSERT_EQ(Native.ParseStatus, ExceptionParseStatus::Complete);
  EXPECT_TRUE(llvm::any_of(Native.Entries, [&](const auto &Entry) {
    return Entry.CodeRange.Begin == Image->SymbolAddrs.at(kObjectFunction) &&
           Entry.SemanticStatus == CompactUnwindSemanticStatus::Complete;
  }));
  EXPECT_TRUE(llvm::any_of(Native.Entries, [&](const auto &Entry) {
    return Entry.CodeRange.Begin == Image->SymbolAddrs.at(kObjectDispatch) &&
           Entry.SemanticStatus == CompactUnwindSemanticStatus::Complete;
  }));
  bool ObjectDwarfLanguage = DwarfLanguage;
  if (Generated && !DwarfLanguage) {
    // The producer can choose DWARF for the catch and cleanup even without
    // an explicit request. Read its object before inspecting the installation.
    auto InputObject = llvm::object::ObjectFile::createObjectFile(ObjectPath);
    ASSERT_TRUE(bool(InputObject)) << llvm::toString(InputObject.takeError());
    auto Debug = llvm::DWARFContext::create(
        *InputObject->getBinary(),
        llvm::DWARFContext::ProcessDebugRelocations::Ignore);
    auto Frames = Debug->getEHFrame();
    ASSERT_TRUE(bool(Frames)) << llvm::toString(Frames.takeError());
    ObjectDwarfLanguage = llvm::any_of((*Frames)->entries(), [](auto &Entry) {
      const auto *FDE = llvm::dyn_cast<llvm::dwarf::FDE>(&Entry);
      return FDE && FDE->getLSDAAddress().has_value();
    });
  }
  std::string DwarfInfo;
  if (ObjectDwarfLanguage) {
    const auto DwarfInfoPath = Path(kDwarfInfoFile);
    Args.assign({kXcrun});
    Args.append(std::begin(kDwarfInfoArgs), std::end(kDwarfInfoArgs));
    Args.push_back(InstalledPath);
    ASSERT_EQ(Run(kXcrun, Args, DwarfInfoPath), 0);
    auto Dump = llvm::MemoryBuffer::getFile(DwarfInfoPath);
    ASSERT_TRUE(bool(Dump));
    DwarfInfo = (*Dump)->getBuffer().str();
    EXPECT_TRUE(
        llvm::StringRef(DwarfInfo).contains(kDwarfLanguageAugmentation));
  }
  for (const auto *Name : {kObjectCatch, kObjectCleanup}) {
    const auto *Function =
        Parsed->ExceptionMetadata.findFunction(Image->SymbolAddrs.at(Name));
    ASSERT_NE(Function, nullptr);
    EXPECT_EQ(Function->ParseStatus, ExceptionParseStatus::Complete);
    EXPECT_EQ(Function->Personality, ExceptionPersonality::GxxPersonalityV0);
    ASSERT_TRUE(Function->Itanium);
    EXPECT_FALSE(Function->Itanium->CallSites.empty());
    ASSERT_TRUE(Function->Compact);
    EXPECT_TRUE(Function->Compact->HasLSDA);
    EXPECT_EQ(Function->Dwarf.has_value(), ObjectDwarfLanguage);
    if (Function->Dwarf) {
      const auto *CIE =
          Parsed->ExceptionMetadata.findCIE(Function->Dwarf->CIESectionOffset);
      ASSERT_NE(CIE, nullptr);
      EXPECT_TRUE(CIE->UsesArm64eReturnAddressABI);
      EXPECT_NE(CIE->PersonalitySlotVA, 0u);
      EXPECT_EQ(Function->Dwarf->LSDAVA, Function->HandlerDataVA);
      const auto Address = [](uint64_t VA, unsigned Width) {
        auto Text = llvm::utohexstr(VA, true);
        return std::string(Width > Text.size() ? Width - Text.size() : 0, '0') +
               Text;
      };
      EXPECT_TRUE(llvm::StringRef(DwarfInfo).contains(
          kDwarfPCPrefix + Address(Function->CodeRange.Begin, kDwarfPCDigits) +
          kDwarfRangeSeparator +
          Address(Function->CodeRange.End, kDwarfPCDigits)));
      EXPECT_TRUE(llvm::StringRef(DwarfInfo).contains(
          kDwarfLSDAPrefix +
          Address(Function->HandlerDataVA, kDwarfAddressDigits)));
    }
    if (llvm::StringRef(Name) == kObjectCatch) {
      const auto &Types = Function->Itanium->TypeTable;
      const auto Type = llvm::find_if(
          Types, [](const auto &T) { return T.TypeName == kTypeInfo; });
      ASSERT_NE(Type, Types.end());
      if (TypeBinds) {
        EXPECT_TRUE(llvm::any_of(*Recipes, [&](const auto &R) {
          return R.Address == Type->TypeInfoSlotVA && !R.Authenticated &&
                 R.Recipe.Symbol == kTypeInfo;
        }));
      } else {
        const auto Existing = Before->DyldBindSlots.find(Type->TypeInfoSlotVA);
        ASSERT_NE(Existing, Before->DyldBindSlots.end());
        EXPECT_EQ(Existing->second.Name, kTypeInfo);
        EXPECT_EQ(Existing->second.Addend, 0);
      }
    }
  }
  if (ExecuteNative) {
    auto Anchor = llvm::find_if(Parsed->Symbols, [&](const auto &Symbol) {
      return Symbol.Name == kSlotSymbol;
    });
    ASSERT_NE(Anchor, Parsed->Symbols.end());
    const auto Entry = [&](llvm::StringRef Name) {
      for (const auto &Mapping : Mappings)
        if (Mapping.Symbol == Name)
          return Mapping.OriginalVA;
      return Image->SymbolAddrs.at(Name.str());
    };
    const std::string Offsets[] = {
        std::to_string(Entry(kObjectDispatch) - Anchor->Addr),
        std::to_string(Image->SymbolAddrs.at(kObjectLocalData) - Anchor->Addr),
        std::to_string(Image->SymbolAddrs.at(kObjectExternalData) -
                       Anchor->Addr),
        std::to_string(Image->SymbolAddrs.at(kObjectLocalCode) - Anchor->Addr),
        std::to_string(Entry(kObjectCatch) - Anchor->Addr),
        std::to_string(Entry(kObjectCleanup) - Anchor->Addr),
        std::to_string(Image->SymbolAddrs.at(kObjectCleanups) - Anchor->Addr)};
    const auto &Runner = Executable ? InstalledPath : DriverPath;
    Args.assign({Runner, InstalledPath});
    Args.append(std::begin(Offsets), std::end(Offsets));
    ASSERT_EQ(Run(Runner, Args, RuntimeOutput), 0);
    auto Output = llvm::MemoryBuffer::getFile(RuntimeOutput);
    ASSERT_TRUE(bool(Output));
    EXPECT_TRUE((*Output)->getBuffer().contains(kRuntimeMarker));
  }
#endif
}

TEST_P(MachOArm64eNativeObjectInstallation,
       NativeRelocationsAdmitProvidersAndRelro) {
  const auto [Optimization, Threaded] = GetParam();
  checkNativeObjectInstallation(Optimization, Threaded, false);
}

TEST_P(MachOArm64eNativeObjectInstallation,
       AuthenticatedCallsAndSignedUnwindExecute) {
  const auto [Optimization, Threaded] = GetParam();
  checkNativeObjectInstallation(Optimization, Threaded, true);
}

TEST_P(MachOArm64eNativeObjectInstallation,
       NativeDwarfLanguageAndOrdinaryPointers) {
  const auto [Optimization, Threaded] = GetParam();
  checkNativeObjectInstallation(Optimization, Threaded, false, true);
}

TEST_P(MachOArm64eNativeObjectInstallation,
       NativeDwarfLanguageCallsAndCleanup) {
  const auto [Optimization, Threaded] = GetParam();
  checkNativeObjectInstallation(Optimization, Threaded, true, true);
}

TEST_P(MachOArm64eNativeObjectInstallation,
       InstallsNewAuthenticatedPersonalitySlot) {
  const auto [Optimization, Threaded] = GetParam();
  checkNativeObjectInstallation(Optimization, Threaded, false, true, true);
}

TEST_P(MachOArm64eNativeObjectInstallation,
       RelocatesOriginalSignedLanguageFrames) {
  const auto [Optimization, Threaded] = GetParam();
  checkNativeObjectInstallation(Optimization, Threaded, false, true, false,
                                true);
}

TEST_P(MachOArm64eNativeObjectInstallation,
       ConvertsThreadedImagesAndRetainsSignedLanguageFrames) {
  const auto [Optimization, Threaded] = GetParam();
  checkNativeObjectInstallation(Optimization, Threaded, false, true, false,
                                true, true);
}

TEST_P(MachOArm64eNativeObjectInstallation,
       RelocatedSignedLanguageFramesExecute) {
  const auto [Optimization, Threaded] = GetParam();
  checkNativeObjectInstallation(Optimization, Threaded, true, true, false,
                                true);
}

// The second parameter selects DWARF for publication; legacy fixtures retain
// their separate explicit threaded-to-chained migration contract above.
TEST_P(MachOArm64eNativeObjectInstallation, PublishesSignedNativeObject) {
  const auto [Optimization, DwarfLanguage] = GetParam();
  checkNativeObjectInstallation(Optimization, false, false, DwarfLanguage,
                                false, false, false,
                                NativeObjectOperation::Publish);
}

TEST_P(MachOArm64eNativeObjectInstallation, PublishedEntriesExecute) {
  const auto [Optimization, DwarfLanguage] = GetParam();
  checkNativeObjectInstallation(Optimization, false, true, DwarfLanguage, false,
                                false, false, NativeObjectOperation::Publish);
}

TEST_P(MachOArm64eNativeObjectInstallation, PublishesMigratedThreadedObject) {
  const auto [Optimization, DwarfLanguage] = GetParam();
  checkNativeObjectInstallation(Optimization, true, false, DwarfLanguage, false,
                                false, true, NativeObjectOperation::Publish);
}

TEST_P(MachOArm64eNativeObjectInstallation, PublishedThreadedEntriesExecute) {
  const auto [Optimization, DwarfLanguage] = GetParam();
  checkNativeObjectInstallation(Optimization, true, true, DwarfLanguage, false,
                                false, true, NativeObjectOperation::Publish);
}

TEST_P(MachOArm64eNativeObjectInstallation, PublishesSignedExecutable) {
  const auto [Optimization, DwarfLanguage] = GetParam();
  checkNativeObjectInstallation(Optimization, false, false, DwarfLanguage,
                                false, false, false,
                                NativeObjectOperation::PublishExecutable);
}

TEST_P(MachOArm64eNativeObjectInstallation, PublishedExecutableStarts) {
  const auto [Optimization, DwarfLanguage] = GetParam();
  checkNativeObjectInstallation(Optimization, false, true, DwarfLanguage, false,
                                false, false,
                                NativeObjectOperation::PublishExecutable);
}

TEST_P(MachOArm64eNativeObjectInstallation, PublishesGeneratedIR) {
  const auto [Optimization, DwarfLanguage] = GetParam();
  checkNativeObjectInstallation(Optimization, false, false, DwarfLanguage,
                                false, false, false,
                                NativeObjectOperation::PublishGeneratedIR);
}

TEST_P(MachOArm64eNativeObjectInstallation, GeneratedIREntriesExecute) {
  const auto [Optimization, DwarfLanguage] = GetParam();
  checkNativeObjectInstallation(Optimization, false, true, DwarfLanguage, false,
                                false, false,
                                NativeObjectOperation::PublishGeneratedIR);
}

TEST_P(MachOArm64eNativeObjectInstallation, PublishesGeneratedExecutable) {
  const auto [Optimization, DwarfLanguage] = GetParam();
  checkNativeObjectInstallation(
      Optimization, false, false, DwarfLanguage, false, false, false,
      NativeObjectOperation::PublishGeneratedExecutable);
}

TEST_P(MachOArm64eNativeObjectInstallation, GeneratedExecutableStarts) {
  const auto [Optimization, DwarfLanguage] = GetParam();
  checkNativeObjectInstallation(
      Optimization, false, true, DwarfLanguage, false, false, false,
      NativeObjectOperation::PublishGeneratedExecutable);
}

INSTANTIATE_TEST_SUITE_P(AppleClang, MachOArm64eNativeObjectInstallation,
                         ::testing::Combine(::testing::Values(kO0, kO2),
                                            ::testing::Bool()));

enum class PublishedFrame { Compact, Indexed, DwarfOnly, Mixed };
enum class AnonymousInitializer { None, Plain, Authenticated };

class MachOArm64ePublishedFrames
    : public ::testing::TestWithParam<
          std::tuple<PublishedFrame, AnonymousInitializer>> {};

TEST_P(MachOArm64ePublishedFrames, PublishesAndUnwinds) {
#if defined(__APPLE__) && defined(__aarch64__)
  const auto [Format, Init] = GetParam();
  const bool Anonymous =
      Format == PublishedFrame::Compact || Format == PublishedFrame::Indexed;
  llvm::SmallString<kPathCapacity> Directory;
  ASSERT_FALSE(
      llvm::sys::fs::createUniqueDirectory(kObjectArtifactPrefix, Directory));
  const std::filesystem::path Root(Directory.str().str());
  auto Cleanup = llvm::scope_exit([&] { std::filesystem::remove_all(Root); });
  const auto Assembly = (Root / kDwarfSourceFile).string();
  const auto Source = (Root / kSourceFile).string();
  const auto DriverSource = (Root / kRuntimeLoaderFile).string();
  const auto Driver = (Root / kRuntimeLoaderBinary).string();
  const auto Object = (Root / kObjectFile).string();
  const auto RejectedObject = (Root / kAnonymousRejectedObject).string();
  const auto Original = (Root / kOriginalFile).string();
  const auto Apple = (Root / kAnonymousAppleImage).string();
  const auto Published = (Root / kInstalledFile).string();
  std::ofstream(Assembly) << (Anonymous ? kAnonymousFrameAssembly
                                        : kStandaloneFDEAssembly);
  std::ofstream(Source) << kAnonymousFrameSource;
  std::ofstream(DriverSource) << kAnonymousFrameDriver;

  const auto Compile = [&](llvm::ArrayRef<llvm::StringLiteral> Profile,
                           llvm::ArrayRef<llvm::StringRef> Inputs,
                           llvm::StringRef Output) {
    llvm::SmallVector<llvm::StringRef> Args{kCompiler};
    llvm::append_range(Args, Profile);
    llvm::append_range(Args, Inputs);
    llvm::append_range(Args, kOutputArgs);
    Args.push_back(Output);
    return llvm::sys::ExecuteAndWait(kCompiler, Args);
  };
  const char *Profile = std::getenv(kExecutableEnvironment);
  const bool Native =
      Profile && llvm::StringRef(Profile) == kAnonymousSystemProfile;
  const llvm::ArrayRef<llvm::StringLiteral> DriverArgs =
      Native ? llvm::ArrayRef(kRuntimeCompileArgs)
             : llvm::ArrayRef(kBindingDriverArgs);
  ASSERT_EQ(Compile(DriverArgs, {DriverSource}, Driver), 0);
  RecordProperty(kAnonymousDriverProperty, DriverArgs[1].str());
  ASSERT_EQ(Compile(kCompileArgs, {kO2, Source}, Original), 0);
  const auto Run = [&](llvm::StringRef Image, llvm::StringRef Symbol,
                       llvm::StringRef Result, llvm::StringRef Count) {
    return llvm::sys::ExecuteAndWait(Driver,
                                     {Driver, Image, Symbol, Result, Count});
  };
  ASSERT_EQ(Run(Original, kAnonymousOriginalEntry, kAnonymousOriginalResult,
                kAnonymousNoInit),
            0);
  llvm::SmallVector<llvm::StringRef> Options;
  if (Format == PublishedFrame::Indexed)
    Options.push_back(kAnonymousIndexOption);
  if (Format == PublishedFrame::Mixed)
    Options.push_back(kMixedFDEOption);
  if (Init != AnonymousInitializer::None)
    Options.push_back(kAnonymousInitOption);
  if (Init == AnonymousInitializer::Authenticated)
    Options.push_back(kAnonymousSignedInitOption);
  Options.push_back(Assembly);
  ASSERT_EQ(Compile(kObjectCompileArgs, Options, Object), 0);
  ASSERT_EQ(Compile(kCompileArgs, {kO2, Source, Object}, Apple), 0);
  const auto *Count =
      Init == AnonymousInitializer::None ? kAnonymousNoInit : kAnonymousOneInit;
  ASSERT_EQ(Run(Apple, kAnonymousEntry, kAnonymousPublishedResult, Count), 0);

  auto Buffer = llvm::MemoryBuffer::getFile(Object);
  ASSERT_TRUE(bool(Buffer));
  auto Producer =
      llvm::object::ObjectFile::createObjectFile((*Buffer)->getMemBufferRef());
  ASSERT_TRUE(bool(Producer)) << llvm::toString(Producer.takeError());
  bool HasCompact = false, HasDwarf = false;
  uint64_t TextSize = 0;
  for (const auto &Section : (*Producer)->sections()) {
    auto Name = Section.getName();
    ASSERT_TRUE(bool(Name)) << llvm::toString(Name.takeError());
    HasCompact |= *Name == section_names::macho::CompactUnwind;
    HasDwarf |= *Name == kEHFrameSection;
    if (Section.isText())
      TextSize = Section.getSize();
  }
  ASSERT_NE(TextSize, 0u);
  EXPECT_EQ(HasCompact, Format != PublishedFrame::DwarfOnly);
  EXPECT_EQ(HasDwarf, Format != PublishedFrame::Compact);
  // The text atom and section-start symbol can name only the mapped entry.
  for (const auto &Symbol : (*Producer)->symbols()) {
    auto Section = Symbol.getSection();
    ASSERT_TRUE(bool(Section)) << llvm::toString(Section.takeError());
    if (*Section == (*Producer)->section_end() || !(*Section)->isText())
      continue;
    auto Address = Symbol.getAddress();
    ASSERT_TRUE(bool(Address)) << llvm::toString(Address.takeError());
    ASSERT_EQ(*Address, (*Section)->getAddress());
  }

  auto Reader = Loader::create(BinaryFormat::MachO);
  auto Before = Reader->load(Original);
  ASSERT_TRUE(bool(Before)) << llvm::toString(Before.takeError());
  if (!Anonymous)
    ASSERT_FALSE(Before->hasSection(kEHFrameSection));
  const auto *Entry = Before->findSymbol(kAnonymousOriginalSymbol);
  ASSERT_TRUE(Entry);
  const SourcePatchMapping Map{kAnonymousEntry, Entry->Addr};
  const auto Install = [&](llvm::StringRef Bytes) {
    return installMachOObjectPatch(Bytes, *Before, {Map}, {}, Published, {});
  };
  const auto Reject = [&](llvm::StringRef Option, llvm::StringRef Expected) {
    auto RejectedOptions = Options;
    RejectedOptions.insert(RejectedOptions.begin(), Option);
    ASSERT_EQ(Compile(kObjectCompileArgs, RejectedOptions, RejectedObject), 0);
    auto Rejected = llvm::MemoryBuffer::getFile(RejectedObject);
    ASSERT_TRUE(bool(Rejected));
    auto Result = Install((*Rejected)->getBuffer());
    ASSERT_FALSE(bool(Result));
    const auto Error = llvm::toString(Result.takeError());
    EXPECT_TRUE(llvm::StringRef(Error).contains(Expected)) << Error;
    EXPECT_FALSE(std::filesystem::exists(Published));
  };
  if (Init != AnonymousInitializer::None)
    ASSERT_NO_FATAL_FAILURE(
        Reject(kAnonymousInteriorOption, kAnonymousInitError));
  if (Init == AnonymousInitializer::Authenticated)
    ASSERT_NO_FATAL_FAILURE(
        Reject(kAnonymousBadInitKeyOption, kAnonymousInitError));
  if (Format == PublishedFrame::Compact && Init == AnonymousInitializer::None)
    ASSERT_NO_FATAL_FAILURE(Reject(kAnonymousDataOption, kAnonymousCodeError));
  if (Format == PublishedFrame::Indexed && Init == AnonymousInitializer::None)
    ASSERT_NO_FATAL_FAILURE(
        Reject(kAnonymousWrongKeyOption, kAnonymousSigningError));

  if (Format == PublishedFrame::DwarfOnly) {
    ASSERT_NO_FATAL_FAILURE(
        Reject(kConflictingExtentOption, kConflictingExtentError));
    ASSERT_NO_FATAL_FAILURE(Reject(kMissingWriterOption, kMissingWriterError));
  }
  auto Patch = Install((*Buffer)->getBuffer());
  ASSERT_TRUE(bool(Patch)) << llvm::toString(Patch.takeError());
  ASSERT_EQ(Patch->PatchedFunctions.size(), 1u);
  auto After = Reader->load(Published);
  ASSERT_TRUE(bool(After)) << llvm::toString(After.takeError());
  const va_t Owner = Patch->PatchedFunctions.front().OwnerVA;
  va_t FrameVA = Owner;
  if (Anonymous) {
    const auto *Word = After->readVA(Owner, kInstructionBytes);
    ASSERT_TRUE(Word);
    size_t Length;
    const auto Target =
        decodeDirectBranchTarget(After->Arch, InstructionMode::Default, Word,
                                 kInstructionBytes, Owner, Length);
    ASSERT_EQ(Target, Owner + kInstructionBytes);
    FrameVA = *Target;
  }
  const auto Unwind = macho_unwind::parseCompactUnwind(*After);
  ASSERT_EQ(Unwind.ParseStatus, ExceptionParseStatus::Complete);
  const auto ExpectedFrames = Anonymous
                                  ? kAnonymousProducerFrames
                                  : 1u + (Format == PublishedFrame::Mixed);
  const auto FrameCount = llvm::count_if(Unwind.Entries, [&](const auto &F) {
    return F.CodeRange.Begin >= Owner && F.CodeRange.Begin < Owner + TextSize;
  });
  EXPECT_EQ(FrameCount, ExpectedFrames);
  RecordProperty(kPublishedFrameCountProperty, FrameCount);
  const auto Frame = llvm::find_if(Unwind.Entries, [&](const auto &F) {
    return F.CodeRange.Begin == FrameVA;
  });
  ASSERT_NE(Frame, Unwind.Entries.end());
  EXPECT_EQ(Frame->NativeEncoding & macho_unwind::kModeMask,
            Format == PublishedFrame::Compact ? macho_unwind::kARM64ModeFrame
                                              : macho_unwind::kARM64ModeDwarf);
  auto Signature = macho_signing::verifyStrict(Published);
  ASSERT_FALSE(bool(Signature)) << llvm::toString(std::move(Signature));
  EXPECT_EQ(
      Run(Published, kAnonymousOriginalEntry, kAnonymousPublishedResult, Count),
      0);
#else
  GTEST_SKIP() << kMissingHost;
#endif
}

INSTANTIATE_TEST_SUITE_P(
    AppleClang, MachOArm64ePublishedFrames,
    ::testing::Combine(::testing::Values(PublishedFrame::Compact,
                                         PublishedFrame::Indexed),
                       ::testing::Values(AnonymousInitializer::None,
                                         AnonymousInitializer::Plain,
                                         AnonymousInitializer::Authenticated)));

INSTANTIATE_TEST_SUITE_P(
    StandaloneFDE, MachOArm64ePublishedFrames,
    ::testing::Combine(::testing::Values(PublishedFrame::DwarfOnly,
                                         PublishedFrame::Mixed),
                       ::testing::Values(AnonymousInitializer::None)));

class MachOArm64eNativeBindings
    : public ::testing::TestWithParam<std::tuple<bool, bool, bool>> {};

TEST_P(MachOArm64eNativeBindings, PublishesDirectAndGOTReferences) {
#ifndef __APPLE__
  GTEST_SKIP() << kMissingHost;
#else
  const auto [OriginalWeak, ObjectWeak, DefinesFallback] = GetParam();
  llvm::SmallString<kPathCapacity> Directory;
  ASSERT_FALSE(
      llvm::sys::fs::createUniqueDirectory(kObjectArtifactPrefix, Directory));
  const std::filesystem::path Root(Directory.str().str());
  auto Cleanup = llvm::scope_exit([&] { std::filesystem::remove_all(Root); });
  const auto Source = (Root / kDwarfSourceFile).string();
  const auto Object = (Root / kObjectFile).string();
  auto Original = (Root / kOriginalFile).string();
  const auto Published = (Root / kInstalledFile).string();
  const auto Info = (Root / kDyldInfoFile).string();
  std::ofstream(Source) << kBindingAssembly;
  const auto Compile = [&](llvm::ArrayRef<llvm::StringLiteral> Profile,
                           bool Weak, llvm::ArrayRef<llvm::StringRef> Inputs,
                           llvm::StringRef Output) {
    llvm::SmallVector<llvm::StringRef> Args{kCompiler};
    llvm::append_range(Args, Profile);
    if (Weak)
      Args.push_back(kBindingWeakOption);
    llvm::append_range(Args, Inputs);
    llvm::append_range(Args, kOutputArgs);
    Args.push_back(Output);
    return llvm::sys::ExecuteAndWait(kCompiler, Args);
  };
  const auto Library = (Root / kRuntimeProviderFile).string();
  const auto WeakLibraryOption = kBindingWeakLibrary + Library;
  const auto ProviderSource = (Root / kRuntimeProviderSourceFile).string();
  std::ofstream(ProviderSource) << kBindingProvider;
  ASSERT_EQ(Compile(kCompileArgs, false, {ProviderSource}, Library), 0);
  llvm::SmallVector<llvm::StringRef> Link{
      kChainedOption, Source, OriginalWeak ? Library : WeakLibraryOption};
  ASSERT_EQ(Compile(kCompileArgs, OriginalWeak, Link, Original), 0);
  // Make the dylib optional while keeping its required on-disk symbol.
  // This exercises both ways of reusing a sealed cell under a weak load
  // command.
  const bool RequiredInWeakDylib = !OriginalWeak && !DefinesFallback;
  if (OriginalWeak || RequiredInWeakDylib) {
    const llvm::StringRef WeakLoad =
        OriginalWeak ? llvm::StringRef(Library) : kBindingSystemLibrary;
    auto Input = llvm::MemoryBuffer::getFile(Original);
    ASSERT_TRUE(bool(Input));
    std::vector<uint8_t> Data((*Input)->getBufferStart(),
                              (*Input)->getBufferEnd());
    auto Signature = macho_signature::inspect(Data);
    ASSERT_TRUE(bool(Signature)) << llvm::toString(Signature.takeError());
    unsigned Changed = 0;
    forEachMachOLoadCommand(
        Data.data(), Data.size(),
        [&](const uint8_t *C, uint32_t ID, uint32_t Size, bool) {
          if (ID != llvm::MachO::LC_LOAD_DYLIB)
            return;
          const auto Name =
              reinterpret_cast<const llvm::MachO::dylib_command *>(C)
                  ->dylib.name;
          if (Name >= Size ||
              readFixedName(reinterpret_cast<const char *>(C + Name),
                            Size - Name) != WeakLoad)
            return;
          writeLE<uint32_t>(Data.data() + (C - Data.data()),
                            llvm::MachO::LC_LOAD_WEAK_DYLIB);
          ++Changed;
        });
    ASSERT_EQ(Changed, 1u);
    Original = (Root / kBindingWeakLoadImage).string();
    ASSERT_TRUE(
        std::ofstream(Original, std::ios::binary)
            .write(reinterpret_cast<const char *>(Data.data()), Data.size()));
    auto Signed =
        macho_signing::adHocResign(Original, Signature->Slices.front());
    ASSERT_FALSE(bool(Signed)) << llvm::toString(std::move(Signed));
  }
  llvm::SmallVector<llvm::StringRef> Input{kBindingObjectOption};
  if (!DefinesFallback)
    Input.push_back(kBindingReferenceOnlyOption);
  Input.push_back(Source);
  ASSERT_EQ(Compile(kObjectCompileArgs, ObjectWeak, Input, Object), 0);
  auto Reader = Loader::create(BinaryFormat::MachO);
  auto Before = Reader->load(Original);
  ASSERT_TRUE(bool(Before)) << llvm::toString(Before.takeError());
  const auto *Entry = Before->findSymbol(kBindingEntrySymbol);
  const auto *OldFields = Before->findSymbol(kBindingFieldsSymbol);
  ASSERT_NE(Entry, nullptr);
  ASSERT_NE(OldFields, nullptr);
  auto Old = Before->MachOAuthenticatedPointers.find(OldFields->Addr);
  ASSERT_NE(Old, Before->MachOAuthenticatedPointers.end());
  ASSERT_EQ(Old->second.WeakImport, OriginalWeak);
  ASSERT_EQ(Old->second.Symbol, kInstalledImportSymbol);
  ASSERT_EQ(Old->second.Key, kAArch64PAuthKeyDA);
  ASSERT_EQ(Old->second.Diversity, kObjectDiversity);
  ASSERT_TRUE(Old->second.AddressDiversity);
  ASSERT_EQ(Old->second.Addend, -kObjectDataAddend);
  ASSERT_EQ(OriginalWeak,
            llvm::any_of(Before->DyldBindSlots, [](const auto &B) {
              return B.second.Name == kCoalescedSymbol;
            }));
  const auto *Marker = Before->findSymbol(kBindingMarkerSymbol);
  const auto *OriginalCode = Before->findSymbol(kBindingOriginalCode);
  ASSERT_NE(Marker, nullptr);
  ASSERT_NE(OriginalCode, nullptr);
  const auto *CodeFields =
      Before->readVA(OriginalCode->Addr, 2 * kPointerBytes);
  ASSERT_NE(CodeFields, nullptr);
  const SourcePatchMapping Map{ObjectWeak ? kBindingEntry : kBindingExactEntry,
                               Entry->Addr};
  const SourcePatchMapping MarkerMap{kBindingExactMarker, Marker->Addr};
  llvm::SmallVector<SourcePatchBinding> Bindings;
  if (DefinesFallback) {
    // Cross-bind the addresses so an implicit lookup cannot pass the control.
    Bindings.push_back({kBindingExactCode, SourcePatchBindingKind::Function,
                        readLE<uint64_t>(CodeFields + kPointerBytes)});
    Bindings.push_back({kBindingExactBareCode, SourcePatchBindingKind::Function,
                        readLE<uint64_t>(CodeFields)});
  }
  const SourcePatchDependency Dependency{
      OriginalWeak ? Library : Old->second.Module, kOptionalBinding,
      SourcePatchDependencyKind::Data};
  auto Bytes = llvm::MemoryBuffer::getFile(Object);
  ASSERT_TRUE(bool(Bytes));
  const auto Install = [&](llvm::ArrayRef<SourcePatchMapping> Entries,
                           llvm::ArrayRef<SourcePatchBinding> References,
                           llvm::ArrayRef<SourcePatchDependency> Dependencies) {
    return installMachOObjectPatch((*Bytes)->getBuffer(), *Before, Entries,
                                   References, Published, {}, nullptr, nullptr,
                                   Dependencies);
  };
  auto Missing = Install({Map, MarkerMap}, Bindings, {});
  ASSERT_FALSE(bool(Missing));
  llvm::consumeError(Missing.takeError());
  ASSERT_FALSE(std::filesystem::exists(Published));
  llvm::SmallVector<SourcePatchDependency> Dependencies{Dependency};
  if (DefinesFallback)
    Dependencies.push_back(
        {Library, kVerbatimBinding, SourcePatchDependencyKind::Function});
  for (const auto Name : {kBindingInterior, kBindingAbsentSpelling,
                          kBindingExactData, kBindingExactEntry}) {
    auto Invalid = Install({Map, {Name, Marker->Addr}}, Bindings, Dependencies);
    ASSERT_FALSE(bool(Invalid));
    llvm::consumeError(Invalid.takeError());
    ASSERT_FALSE(std::filesystem::exists(Published));
  }
  if (DefinesFallback) {
    auto Aliased = Bindings;
    auto Alias = Bindings.front();
    Alias.Symbol = kBindingCode;
    Aliased.push_back(std::move(Alias));
    auto Invalid = Install({Map, MarkerMap}, Aliased, Dependencies);
    ASSERT_FALSE(bool(Invalid));
    llvm::consumeError(Invalid.takeError());
    ASSERT_FALSE(std::filesystem::exists(Published));

    auto Conflicting = Dependencies;
    Conflicting.push_back(
        {Library, kBindingCode, SourcePatchDependencyKind::Function});
    Invalid = Install({Map, MarkerMap}, Bindings, Conflicting);
    ASSERT_FALSE(bool(Invalid));
    const auto Detail = llvm::toString(Invalid.takeError());
    EXPECT_TRUE(llvm::StringRef(Detail).contains(kConflictingBinding))
        << Detail;
    ASSERT_FALSE(std::filesystem::exists(Published));
  }
  auto Patch = Install({Map, MarkerMap}, Bindings, Dependencies);
  ASSERT_TRUE(bool(Patch)) << llvm::toString(Patch.takeError());
  ASSERT_EQ(Patch->PatchedFunctions.size(), 2u);
  EXPECT_EQ(Patch->PatchedFunctions[0].SourceFunction, Map.Symbol);
  EXPECT_EQ(Patch->PatchedFunctions[1].SourceFunction, MarkerMap.Symbol);
  EXPECT_EQ(Patch->PatchedFunctions[0].OwnerSymbol, kBindingEntry);
  EXPECT_EQ(Patch->PatchedFunctions[1].OwnerSymbol, kBindingMarker);
  auto After = Reader->load(Published);
  ASSERT_TRUE(bool(After)) << llvm::toString(After.takeError());
  const auto Unwind = macho_unwind::parseCompactUnwind(*After);
  ASSERT_EQ(Unwind.ParseStatus, ExceptionParseStatus::Complete);
  const auto MarkerFrame = llvm::find_if(Unwind.Entries, [&](const auto &E) {
    return E.CodeRange.contains(Patch->PatchedFunctions[1].OwnerVA);
  });
  ASSERT_NE(MarkerFrame, Unwind.Entries.end());
  EXPECT_EQ(MarkerFrame->NativeEncoding & macho_unwind::kModeMask,
            DefinesFallback ? macho_unwind::kARM64ModeFrameless
                            : macho_unwind::kARM64ModeDwarf);
  auto Signature = macho_signing::verifyStrict(Published);
  ASSERT_FALSE(bool(Signature)) << llvm::toString(std::move(Signature));

  const va_t OldVA = OldFields->Addr + Patch->OriginalSectionSlide;
  auto Preserved = After->MachOAuthenticatedPointers.find(OldVA);
  ASSERT_NE(Preserved, After->MachOAuthenticatedPointers.end());
  EXPECT_EQ(Preserved->second.WeakImport, Old->second.WeakImport);
  const auto New =
      llvm::find_if(After->MachOAuthenticatedPointers, [&](const auto &P) {
        return P.first != OldVA && P.second.Symbol == kInstalledImportSymbol &&
               P.second.Key == kAArch64PAuthKeyDA;
      });
  ASSERT_NE(New, After->MachOAuthenticatedPointers.end());
  ASSERT_TRUE(New->second.LibraryOrdinal);
  const auto &ExpectedImport = New->second;
  EXPECT_EQ(ExpectedImport.WeakImport, ObjectWeak);
  EXPECT_EQ(New->second.Module, Old->second.Module);
  EXPECT_EQ(New->second.LibraryOrdinal, Old->second.LibraryOrdinal);
  EXPECT_EQ(New->second.Addend, Old->second.Addend);
  EXPECT_EQ(New->second.Diversity, Old->second.Diversity);
  EXPECT_EQ(New->second.AddressDiversity, Old->second.AddressDiversity);
  struct Fields {
    uint64_t Auth, Direct, GOT;
    int32_t Relative;
    uint32_t Padding;
    uint64_t Optional;
    uint64_t CoalescedAuth, CoalescedDirect;
    int32_t CoalescedRelative;
    uint32_t CoalescedPadding;
    uint64_t OptionalDirect, OptionalGOT;
    uint64_t BareAuth, BareDirect;
    int32_t BareRelative;
    uint32_t BarePadding;
    uint64_t LocalAuth, LocalDirect, LocalValue;
  };
  const va_t FieldsVA = New->first;
  const auto *Data = After->readVA(FieldsVA, sizeof(Fields));
  ASSERT_NE(Data, nullptr);
  const va_t GOT = readLE<uint64_t>(Data + offsetof(Fields, GOT));
  EXPECT_TRUE(After->MachOResolvedChainedPointerSlots.contains(
      FieldsVA + offsetof(Fields, GOT)));
  EXPECT_EQ(GOT, FieldsVA + offsetof(Fields, Relative) +
                     readLE<int32_t>(Data + offsetof(Fields, Relative)));
  const auto *OldData = Before->readVA(OldFields->Addr, sizeof(Fields));
  ASSERT_NE(OldData, nullptr);
  const va_t OldGOT = OldVA + offsetof(Fields, Relative) +
                      readLE<int32_t>(OldData + offsetof(Fields, Relative));
  EXPECT_EQ(OldGOT == GOT, OriginalWeak == ObjectWeak);
  auto PreservedGOT = After->DyldBindSlots.find(OldGOT);
  ASSERT_NE(PreservedGOT, After->DyldBindSlots.end());
  EXPECT_EQ(PreservedGOT->second.WeakImport, Old->second.WeakImport);
  const auto &ExpectedOriginal = Preserved->second;
  std::vector<MachOAuthenticatedFixup> Recipes{{OldVA, ExpectedOriginal},
                                               {FieldsVA, ExpectedImport}};
  const auto CheckPlain =
      [&](va_t Slot, const MachOAuthenticatedPointer &Target, int64_t Addend) {
        auto Bound = After->DyldBindSlots.find(Slot);
        ASSERT_NE(Bound, After->DyldBindSlots.end());
        EXPECT_EQ(Bound->second.Name, Target.Symbol);
        EXPECT_EQ(Bound->second.WeakImport, Target.WeakImport);
        EXPECT_EQ(Bound->second.LibraryOrdinal, Target.LibraryOrdinal);
        EXPECT_EQ(Bound->second.Addend, Addend);
        MachOAuthenticatedPointer Expected;
        Expected.Symbol = Target.Symbol;
        Expected.WeakImport = Target.WeakImport;
        Expected.Module = Target.Module;
        Expected.LibraryOrdinal = Target.LibraryOrdinal;
        Expected.Addend = Addend;
        Recipes.push_back({Slot, Expected, true, false});
      };
  CheckPlain(GOT, ExpectedImport, 0);
  CheckPlain(FieldsVA + offsetof(Fields, Direct), ExpectedImport,
             kBindingDirectAddend);
  const auto Coalesced = After->MachOAuthenticatedPointers.find(
      FieldsVA + offsetof(Fields, CoalescedAuth));
  ASSERT_NE(Coalesced, After->MachOAuthenticatedPointers.end());
  EXPECT_EQ(Coalesced->second.Symbol, kCoalescedSymbol);
  EXPECT_EQ(Coalesced->second.LibraryOrdinal,
            llvm::MachO::BIND_SPECIAL_DYLIB_WEAK_LOOKUP);
  EXPECT_FALSE(Coalesced->second.WeakImport);
  EXPECT_EQ(Coalesced->second.Addend, kObjectDataAddend);
  EXPECT_EQ(Coalesced->second.Key, kAArch64PAuthKeyDA);
  EXPECT_EQ(Coalesced->second.Diversity, kObjectDiversity);
  EXPECT_TRUE(Coalesced->second.AddressDiversity);
  Recipes.push_back({Coalesced->first, Coalesced->second});
  const va_t CoalescedGOT =
      FieldsVA + offsetof(Fields, CoalescedRelative) +
      readLE<int32_t>(Data + offsetof(Fields, CoalescedRelative));
  CheckPlain(CoalescedGOT, Coalesced->second, 0);
  CheckPlain(FieldsVA + offsetof(Fields, CoalescedDirect), Coalesced->second,
             0);
  MachOAuthenticatedPointer Rebase;
  Rebase.Target = GOT;
  Recipes.push_back({FieldsVA + offsetof(Fields, GOT), Rebase, true, false});
  const auto Optional = After->MachOAuthenticatedPointers.find(
      FieldsVA + offsetof(Fields, Optional));
  ASSERT_NE(Optional, After->MachOAuthenticatedPointers.end());
  EXPECT_EQ(Optional->second.Symbol, kOptionalBinding);
  EXPECT_TRUE(Optional->second.WeakImport);
  EXPECT_EQ(Optional->second.Key, kAArch64PAuthKeyDB);
  EXPECT_EQ(Optional->second.Diversity, kObjectDiversity);
  EXPECT_TRUE(Optional->second.AddressDiversity);
  const auto Provider =
      llvm::find_if(After->MachODylibReferences, [&](const auto &D) {
        return D.Name == Dependency.Library;
      });
  ASSERT_NE(Provider, After->MachODylibReferences.end());
  EXPECT_EQ(Provider->Weak, OriginalWeak || RequiredInWeakDylib);
  EXPECT_EQ(Optional->second.LibraryOrdinal,
            1 + std::distance(After->MachODylibReferences.begin(), Provider));
  EXPECT_EQ(Optional->second.Module, Dependency.Library);
  Recipes.push_back({Optional->first, Optional->second});
  CheckPlain(FieldsVA + offsetof(Fields, OptionalDirect), Optional->second, 0);
  const va_t OptionalGOT =
      readLE<uint64_t>(Data + offsetof(Fields, OptionalGOT));
  EXPECT_TRUE(After->MachOResolvedChainedPointerSlots.contains(
      FieldsVA + offsetof(Fields, OptionalGOT)));
  CheckPlain(OptionalGOT, Optional->second, 0);
  Rebase.Target = OptionalGOT;
  Recipes.push_back(
      {FieldsVA + offsetof(Fields, OptionalGOT), Rebase, true, false});

  const va_t LocalVA = FieldsVA + offsetof(Fields, LocalValue);
  const auto Local = After->MachOAuthenticatedPointers.find(
      FieldsVA + offsetof(Fields, LocalAuth));
  ASSERT_NE(Local, After->MachOAuthenticatedPointers.end());
  EXPECT_EQ(Local->second.Target, LocalVA);
  EXPECT_EQ(Local->second.Key, kAArch64PAuthKeyDA);
  EXPECT_EQ(Local->second.Diversity, kObjectDiversity);
  EXPECT_TRUE(Local->second.AddressDiversity);
  Recipes.push_back({Local->first, Local->second});
  Rebase.Target = LocalVA;
  Recipes.push_back(
      {FieldsVA + offsetof(Fields, LocalDirect), Rebase, true, false});
  EXPECT_EQ(readLE<uint64_t>(Data + offsetof(Fields, LocalDirect)), LocalVA);

  const auto Bare = After->MachOAuthenticatedPointers.find(
      FieldsVA + offsetof(Fields, BareAuth));
  ASSERT_NE(Bare, After->MachOAuthenticatedPointers.end());
  EXPECT_EQ(Bare->second.Symbol, kBareBinding);
  EXPECT_EQ(Bare->second.Module, Library);
  EXPECT_NE(Bare->second.LibraryOrdinal, ExpectedImport.LibraryOrdinal);
  EXPECT_EQ(Bare->second.Key, kAArch64PAuthKeyDB);
  EXPECT_EQ(Bare->second.Diversity, kObjectDiversity);
  EXPECT_TRUE(Bare->second.AddressDiversity);
  EXPECT_EQ(Bare->second.WeakImport, !ObjectWeak);
  const auto &ExpectedBare = Bare->second;
  Recipes.push_back({Bare->first, ExpectedBare});
  const va_t BareGOT = FieldsVA + offsetof(Fields, BareRelative) +
                       readLE<int32_t>(Data + offsetof(Fields, BareRelative));
  EXPECT_NE(BareGOT, GOT);
  CheckPlain(BareGOT, ExpectedBare, 0);
  CheckPlain(FieldsVA + offsetof(Fields, BareDirect), ExpectedBare, 0);

  const va_t Owner = Patch->PatchedFunctions.front().OwnerVA;
  const auto LoadTarget = [&](va_t PC) -> std::optional<va_t> {
    const auto *Code = After->readVA(PC, 2 * kInstructionBytes);
    if (!Code)
      return std::nullopt;
    auto Page = aarch64::decodePCRelativeAddress(readLE<uint32_t>(Code), PC);
    const auto Load = readLE<uint32_t>(Code + kInstructionBytes);
    if (!Page ||
        (Load & aarch64::kLDR_X16_X16_Mask) != aarch64::kLDR_X16_X16_Match)
      return std::nullopt;
    return *Page +
           ((Load >> aarch64::kLDR_Imm12Shift) & aarch64::kLDR_Imm12Mask) *
               kPointerBytes;
  };
  EXPECT_EQ(LoadTarget(Owner + kInstructionBytes), GOT);
  const va_t Branch = Owner + kBindingCallWord * kInstructionBytes;
  const auto *Code = After->readVA(Branch, kInstructionBytes);
  ASSERT_NE(Code, nullptr);
  size_t Length = 0;
  auto Stub = decodeDirectBranchTarget(After->Arch, InstructionMode::Default,
                                       Code, kInstructionBytes, Branch, Length);
  ASSERT_TRUE(Stub);
  const va_t OldBranch = Entry->Addr + kBindingCallWord * kInstructionBytes;
  Code = Before->readVA(OldBranch, kInstructionBytes);
  ASSERT_NE(Code, nullptr);
  auto OldStub =
      decodeDirectBranchTarget(Before->Arch, InstructionMode::Default, Code,
                               kInstructionBytes, OldBranch, Length);
  ASSERT_TRUE(OldStub);
  EXPECT_EQ(*OldStub + Patch->OriginalSectionSlide == *Stub,
            ObjectWeak == OriginalWeak);
  if (ObjectWeak != OriginalWeak) {
    auto CallSlot = LoadTarget(*Stub);
    ASSERT_TRUE(CallSlot);
    ASSERT_NE(*CallSlot, GOT);
    auto Callable = After->MachOAuthenticatedPointers.find(*CallSlot);
    ASSERT_NE(Callable, After->MachOAuthenticatedPointers.end());
    EXPECT_EQ(Callable->second.Symbol, kInstalledImportSymbol);
    EXPECT_EQ(Callable->second.WeakImport, ExpectedImport.WeakImport);
    EXPECT_EQ(Callable->second.LibraryOrdinal, Old->second.LibraryOrdinal);
    EXPECT_EQ(Callable->second.Key, kAArch64PAuthKeyIA);
    EXPECT_FALSE(Callable->second.AddressDiversity);
    EXPECT_EQ(Callable->second.Diversity, 0u);
    Recipes.push_back({*CallSlot, Callable->second});
  }
  llvm::SmallVector<llvm::StringRef> Args{kXcrun};
  llvm::append_range(Args, kInfoArgs);
  Args.push_back(Published);
  const std::optional<llvm::StringRef> Redirects[] = {std::nullopt, Info,
                                                      std::nullopt};
  ASSERT_EQ(llvm::sys::ExecuteAndWait(kXcrun, Args, std::nullopt, Redirects,
                                      kToolTimeout),
            0);
  auto Listing = llvm::MemoryBuffer::getFile(Info);
  ASSERT_TRUE(bool(Listing));
  checkAppleChainedRecipes((*Listing)->getBuffer(), After->Raw, Recipes);
  if (std::getenv(kBindingNativeEnv)) {
    const auto DriverSource = (Root / kRuntimeHarnessFile).string();
    const auto Driver = (Root / kRuntimeDriverFile).string();
    std::ofstream(DriverSource) << kBindingDriver;
    const auto HostDriver = (Root / kBindingHostDriver).string();
    ASSERT_EQ(Compile(kBindingDriverArgs, false, {DriverSource}, Driver), 0);
    ASSERT_EQ(Compile(kBindingDriverArgs, false,
                      {DriverSource, kBindingHostFallback}, HostDriver),
              0);
    for (bool Present : {true, false}) {
      if (!Present)
        ASSERT_TRUE(std::filesystem::remove(Library));
      for (const auto &Program : {Driver, HostDriver}) {
        const auto Run = [&](llvm::ArrayRef<llvm::StringRef> Args,
                             bool Required) {
          const auto Log = (Root / kToolLog).string();
          const std::optional<llvm::StringRef> Redirects[] = {
              std::nullopt, std::nullopt, Log};
          const int Status = llvm::sys::ExecuteAndWait(
              Program, Args, std::nullopt, Redirects, kToolTimeout);
          auto Errors = llvm::MemoryBuffer::getFile(Log);
          ASSERT_TRUE(bool(Errors));
          if (!Present && Required) {
            EXPECT_EQ(Status, kBindingDlopenFailure);
            EXPECT_TRUE((*Errors)->getBuffer().contains(kMissingBareImport))
                << (*Errors)->getBuffer().str();
          } else {
            EXPECT_EQ(Status, 0) << (*Errors)->getBuffer().str();
          }
        };
        Run({Program, Original, Library}, OriginalWeak);
        // Retained original fixups and newly published fixups both bind.
        llvm::SmallVector<llvm::StringRef> PublishedArgs{
            Program, Published, Library, kPublishedProperty};
        if (DefinesFallback)
          PublishedArgs.push_back(kBindingSwapped);
        Run(PublishedArgs, OriginalWeak || ObjectWeak);
      }
    }
  }
#endif
}

INSTANTIATE_TEST_SUITE_P(AppleClang, MachOArm64eNativeBindings,
                         ::testing::Combine(::testing::Bool(),
                                            ::testing::Bool(),
                                            ::testing::Bool()));

TEST(MachONativeRelro, DyldProtectsConstantsAndKeepsMutablePagesWritable) {
#if !defined(__APPLE__) || !defined(__aarch64__)
  GTEST_SKIP() << kRelroNativeHost;
#else
  llvm::SmallString<kPathCapacity> Directory;
  ASSERT_FALSE(
      llvm::sys::fs::createUniqueDirectory(kObjectArtifactPrefix, Directory));
  const auto Cleanup = llvm::scope_exit(
      [&] { EXPECT_FALSE(llvm::sys::fs::remove_directories(Directory)); });
  const auto Path = [&](llvm::StringRef Name) {
    llvm::SmallString<kPathCapacity> Value(Directory);
    llvm::sys::path::append(Value, Name);
    return Value.str().str();
  };
  const auto Source = Path(kSourceFile), Runtime = Path(kRuntimeHarnessFile),
             Original = Path(kOriginalFile), Installed = Path(kInstalledFile),
             Driver = Path(kRuntimeDriverFile),
             Output = Path(kRuntimeOutputFile);
  const auto Write = [&](llvm::StringRef Name, llvm::StringRef Contents) {
    std::error_code Error;
    llvm::raw_fd_ostream Stream(Name, Error);
    if (!Error) {
      Stream << Contents;
      Stream.close();
      Error = Stream.error();
    }
    return Error;
  };
  ASSERT_FALSE(Write(Source, kRelroOriginalSource));
  ASSERT_FALSE(Write(Runtime, kRelroRuntimeSource));
  llvm::SmallVector<llvm::StringRef> Args{kCompiler};
  Args.append(std::begin(kRelroCompileArgs), std::end(kRelroCompileArgs));
  Args.append(std::begin(kProviderCompileArgs), std::end(kProviderCompileArgs));
  Args.append({kRelroHeaderOption, Source});
  Args.append(std::begin(kOutputArgs), std::end(kOutputArgs));
  Args.push_back(Original);
  ASSERT_EQ(llvm::sys::ExecuteAndWait(kCompiler, Args, std::nullopt, {},
                                      kToolTimeout),
            0);
  Args.assign({kCompiler});
  Args.append(std::begin(kRelroCompileArgs), std::end(kRelroCompileArgs));
  Args.push_back(Runtime);
  Args.append(std::begin(kOutputArgs), std::end(kOutputArgs));
  Args.push_back(Driver);
  ASSERT_EQ(llvm::sys::ExecuteAndWait(kCompiler, Args, std::nullopt, {},
                                      kToolTimeout),
            0);
  auto Input = llvm::MemoryBuffer::getFile(Original);
  ASSERT_TRUE(bool(Input));
  std::vector<uint8_t> Bytes((*Input)->getBufferStart(),
                             (*Input)->getBufferEnd());
  auto Signature = macho_signature::inspect(Bytes);
  ASSERT_TRUE(bool(Signature)) << llvm::toString(Signature.takeError());
  auto Reader = Loader::create(BinaryFormat::MachO);
  auto Before = Reader->loadBuffer((*Input)->getMemBufferRef());
  ASSERT_TRUE(bool(Before)) << llvm::toString(Before.takeError());
  const auto Anchor = llvm::find_if(Before->Symbols, [](const auto &Symbol) {
    return Symbol.Name == kSlotSymbol;
  });
  ASSERT_NE(Anchor, Before->Symbols.end());
  MachOPatcher Patcher;
  CompiledImage Image;
  Image.Success = true;
  Image.Format = BinaryFormat::MachO;
  Image.TargetArch = Arch::AArch64;
  Image.InputKind = RewriteInputKind::ReplacementObject;
  Image.BaseVA = Patcher.plannedExecSegmentVA(Bytes, Image.TargetArch);
  ASSERT_NE(Image.BaseVA, 0u);
  Image.Bytes.resize(2 * kPageBytes + kPointerBytes);
  writeLE<uint32_t>(Image.Bytes.data(), kReturnInstruction);
  writeLE<uint64_t>(Image.Bytes.data() + kPageBytes, kRelroRuntimeValue);
  CompiledSection Code, Constant, Mutable;
  Code.Name = kNewCodeSection;
  Code.VA = Image.BaseVA;
  Code.Size = kInstructionBytes;
  Code.Kind = llvm::mc_rewrite::RewriteSectionKind::Code;
  Constant.Name = kNewConstSegment;
  Constant.Offset = kPageBytes;
  Constant.VA = Image.BaseVA + Constant.Offset;
  Constant.Size = kPointerBytes;
  Constant.Kind = llvm::mc_rewrite::RewriteSectionKind::WritableData;
  Constant.ReadOnlyAfterFixups = true;
  Mutable = Constant;
  Mutable.Name = kNewDataSegment;
  Mutable.Offset += kPageBytes;
  Mutable.VA += kPageBytes;
  Mutable.ReadOnlyAfterFixups = false;
  Image.Sections = {Code, Constant, Mutable};
  auto Error = Patcher.appendReplacementImage(Bytes, Image);
  ASSERT_FALSE(bool(Error)) << llvm::toString(std::move(Error));
  ASSERT_FALSE(Write(Installed, llvm::toStringRef(llvm::ArrayRef(Bytes))));
  Error = macho_signing::adHocResign(Installed, Signature->Slices.front());
  ASSERT_FALSE(bool(Error)) << llvm::toString(std::move(Error));
  Error = macho_signing::verifyStrict(Installed);
  ASSERT_FALSE(bool(Error)) << llvm::toString(std::move(Error));
  const auto ConstantOffset = std::to_string(Constant.VA - Anchor->Addr);
  const auto MutableOffset = std::to_string(Mutable.VA - Anchor->Addr);
  const std::optional<llvm::StringRef> Redirects[] = {std::nullopt, Output,
                                                      std::nullopt};
  ASSERT_EQ(llvm::sys::ExecuteAndWait(
                Driver, {Driver, Installed, ConstantOffset, MutableOffset},
                std::nullopt, Redirects, kToolTimeout),
            0);
  auto Result = llvm::MemoryBuffer::getFile(Output);
  ASSERT_TRUE(bool(Result));
  EXPECT_TRUE((*Result)->getBuffer().contains(kRelroRuntimeMarker));
#endif
}
} // namespace
