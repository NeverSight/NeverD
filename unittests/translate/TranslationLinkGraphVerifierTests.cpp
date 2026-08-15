//===- TranslationLinkGraphVerifierTests.cpp - LinkGraph boundary tests ---===//

#include "gtest/gtest.h"

#include "neverd/translate/GuestState.h"
#include "neverd/translate/TranslationLinkGraphVerifier.h"
#include "neverd/translate/TranslationObjectRequest.h"

#include "llvm/ADT/SmallVector.h"
#include "llvm/Object/ObjectFile.h"
#include "llvm/ObjectYAML/yaml2obj.h"
#include "llvm/Support/Error.h"

#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

using namespace neverd::translate;

namespace {

constexpr uint64_t EntryPC = 0x401000;
constexpr llvm::StringLiteral BlockIRName("nvd_x86_64_block_0000000000401000");

TranslationOptions aarch64Options(llvm::StringRef Triple) {
  TranslationOptions Options;
  Options.Guest = GuestArchitecture::X86_64;
  Options.Mode = TranslationMode::AOT;
  Options.Target.Kind = HostTargetKind::Explicit;
  Options.Target.Architecture = GuestArchitecture::AArch64;
  Options.Target.Triple = Triple.str();
  Options.UnsupportedInstructions = UnsupportedInstructionPolicy::Fail;
  Options.Optimization = TranslationOptimizationPolicy::ProvenSemanticAndLLVM;
  Options.LLVMLevel = LLVMOptimizationLevel::O2;
  Options.BlockCache = BlockCachePolicy::Disabled;
  Options.CodeInvalidation = CodeInvalidationPolicy::RejectExecutableWrites;
  return Options;
}

GuestState fixtureState() {
  GuestState State =
      llvm::cantFail(createZeroedGuestState(GuestArchitecture::X86_64));
  State.Memory.push_back({EntryPC,
                          MemoryPermission::Read | MemoryPermission::Execute,
                          19,
                          {0x48, 0x89, 0xf8, 0x48, 0x83, 0xc0, 0x01, 0xc3}});
  return State;
}

llvm::Expected<TranslationObjectResultV1>
compileFixture(llvm::StringRef Triple) {
  GuestState State = fixtureState();
  return compileTranslationObjectRequestV1(
      TranslationObjectRequestV1(State, EntryPC, aarch64Options(Triple)));
}

void expectGraphError(llvm::Expected<TranslationLinkGraphAuditV1> Result,
                      TranslationLinkGraphErrorCode ExpectedCode) {
  ASSERT_FALSE(static_cast<bool>(Result));
  bool SawTypedError = false;
  llvm::Error Unhandled = llvm::handleErrors(
      Result.takeError(), [&](const TranslationLinkGraphError &Error) {
        SawTypedError = true;
        EXPECT_EQ(Error.code(), ExpectedCode) << Error.detail().str();
      });
  if (Unhandled)
    ADD_FAILURE() << llvm::toString(std::move(Unhandled));
  EXPECT_TRUE(SawTypedError);
}

std::vector<uint8_t> yamlArtifact(llvm::StringRef YAML) {
  llvm::SmallVector<char, 0> Storage;
  std::string Diagnostic;
  std::unique_ptr<llvm::object::ObjectFile> Object =
      llvm::yaml::yaml2ObjectFile(Storage, YAML, [&](const llvm::Twine &Text) {
        Diagnostic = Text.str();
      });
  EXPECT_NE(Object, nullptr) << Diagnostic;
  if (!Object)
    return {};
  return {reinterpret_cast<const uint8_t *>(Storage.data()),
          reinterpret_cast<const uint8_t *>(Storage.data() + Storage.size())};
}

TEST(TranslationLinkGraphVerifier,
     AcceptsCompilerProducedAArch64ELFBeforeAllocation) {
  llvm::Expected<TranslationObjectResultV1> ResultOrErr =
      compileFixture("aarch64-unknown-linux-gnu");
  ASSERT_TRUE(static_cast<bool>(ResultOrErr))
      << llvm::toString(ResultOrErr.takeError());
  TranslationObjectResultV1 Result = std::move(*ResultOrErr);
  llvm::Expected<TranslationLinkGraphAuditV1> AuditOrErr =
      verifyTranslationLinkGraphV1(Result.artifact());
  ASSERT_TRUE(static_cast<bool>(AuditOrErr))
      << llvm::toString(AuditOrErr.takeError());
  EXPECT_FALSE(AuditOrErr->GraphTriple.empty());
  EXPECT_GE(AuditOrErr->SectionCount, 1u);
  EXPECT_GE(AuditOrErr->BlockCount, 1u);
  EXPECT_EQ(AuditOrErr->ExternalSymbolCount, 1u);
  EXPECT_GE(AuditOrErr->EdgeCount, 1u);
}

TEST(TranslationLinkGraphVerifier,
     AcceptsCompilerProducedAArch64MachOBeforeAllocation) {
  llvm::Expected<TranslationObjectResultV1> ResultOrErr =
      compileFixture("aarch64-apple-macosx");
  ASSERT_TRUE(static_cast<bool>(ResultOrErr))
      << llvm::toString(ResultOrErr.takeError());
  TranslationObjectResultV1 Result = std::move(*ResultOrErr);
  llvm::Expected<TranslationLinkGraphAuditV1> AuditOrErr =
      verifyTranslationLinkGraphV1(Result.artifact());
  ASSERT_TRUE(static_cast<bool>(AuditOrErr))
      << llvm::toString(AuditOrErr.takeError());
  EXPECT_EQ(AuditOrErr->SectionCount, 1u);
  EXPECT_GE(AuditOrErr->BlockCount, 1u);
  EXPECT_EQ(AuditOrErr->ExternalSymbolCount, 1u);
  EXPECT_GE(AuditOrErr->EdgeCount, 1u);
}

TEST(TranslationLinkGraphVerifier,
     RejectsTruncatedAndCorruptObjectsWithoutMutatingInput) {
  llvm::Expected<TranslationObjectResultV1> ResultOrErr =
      compileFixture("aarch64-unknown-linux-gnu");
  ASSERT_TRUE(static_cast<bool>(ResultOrErr))
      << llvm::toString(ResultOrErr.takeError());
  TranslationObjectResultV1 Result = std::move(*ResultOrErr);
  std::vector<uint8_t> Truncated(Result.artifact().bytes().begin(),
                                 Result.artifact().bytes().begin() + 4);
  const std::vector<uint8_t> TruncatedBefore = Truncated;
  expectGraphError(
      verifyTranslationLinkGraphV1(Truncated, Result.artifact().hostTarget(),
                                   Result.artifact().blockSymbols(),
                                   Result.artifact().runtimeSymbols(),
                                   Result.artifact().runtimeRegistryIdentity()),
      TranslationLinkGraphErrorCode::ObjectGraphCreationFailed);
  EXPECT_EQ(Truncated, TruncatedBefore);

  std::vector<uint8_t> Corrupt(Result.artifact().bytes().begin(),
                               Result.artifact().bytes().end());
  ASSERT_FALSE(Corrupt.empty());
  Corrupt.front() ^= 0xff;
  const std::vector<uint8_t> CorruptBefore = Corrupt;
  expectGraphError(
      verifyTranslationLinkGraphV1(Corrupt, Result.artifact().hostTarget(),
                                   Result.artifact().blockSymbols(),
                                   Result.artifact().runtimeSymbols(),
                                   Result.artifact().runtimeRegistryIdentity()),
      TranslationLinkGraphErrorCode::ObjectGraphCreationFailed);
  EXPECT_EQ(Corrupt, CorruptBefore);
}

TEST(TranslationLinkGraphVerifier, RejectsAnExtraUndefinedSymbol) {
  llvm::Expected<TranslationObjectResultV1> ResultOrErr =
      compileFixture("aarch64-unknown-linux-gnu");
  ASSERT_TRUE(static_cast<bool>(ResultOrErr))
      << llvm::toString(ResultOrErr.takeError());
  TranslationObjectResultV1 Result = std::move(*ResultOrErr);
  const std::vector<uint8_t> Bytes = yamlArtifact(R"(
--- !ELF
FileHeader:
  Class:   ELFCLASS64
  Data:    ELFDATA2LSB
  Type:    ET_REL
  Machine: EM_AARCH64
Sections:
  - Name:         .text
    Type:         SHT_PROGBITS
    Flags:        [ SHF_ALLOC, SHF_EXECINSTR ]
    AddressAlign: 4
    Content:      "00000094"
  - Name: .rela.text
    Type: SHT_RELA
    Info: .text
    Link: .symtab
    Relocations:
      - Offset: 0
        Type:   R_AARCH64_CALL26
        Symbol: nvd_rt_v1_load64_le
Symbols:
  - Name:    nvd_x86_64_block_0000000000401000
    Type:    STT_FUNC
    Section: .text
    Binding: STB_GLOBAL
    Other:   [ STV_HIDDEN ]
    Size:    4
  - Name:    nvd_rt_v1_load64_le
    Type:    STT_FUNC
    Binding: STB_GLOBAL
  - Name:    unexpected_external
    Type:    STT_FUNC
    Binding: STB_GLOBAL
)");
  ASSERT_FALSE(Bytes.empty());
  const std::vector<uint8_t> Before = Bytes;
  expectGraphError(
      verifyTranslationLinkGraphV1(Bytes, Result.artifact().hostTarget(),
                                   Result.artifact().blockSymbols(),
                                   Result.artifact().runtimeSymbols(),
                                   Result.artifact().runtimeRegistryIdentity()),
      TranslationLinkGraphErrorCode::RuntimeSymbolManifestMismatch);
  EXPECT_EQ(Bytes, Before);
}

TEST(TranslationLinkGraphVerifier,
     RejectsNonPowerOfTwoELFSectionAlignmentWithoutAsserting) {
  llvm::Expected<TranslationObjectResultV1> ResultOrErr =
      compileFixture("aarch64-unknown-linux-gnu");
  ASSERT_TRUE(static_cast<bool>(ResultOrErr))
      << llvm::toString(ResultOrErr.takeError());
  TranslationObjectResultV1 Result = std::move(*ResultOrErr);
  const std::vector<uint8_t> Bytes = yamlArtifact(R"(
--- !ELF
FileHeader:
  Class:   ELFCLASS64
  Data:    ELFDATA2LSB
  Type:    ET_REL
  Machine: EM_AARCH64
Sections:
  - Name:         .text
    Type:         SHT_PROGBITS
    Flags:        [ SHF_ALLOC, SHF_EXECINSTR ]
    AddressAlign: 3
    Content:      "C0035FD6"
Symbols:
  - Name:    nvd_x86_64_block_0000000000401000
    Type:    STT_FUNC
    Section: .text
    Binding: STB_GLOBAL
    Other:   [ STV_HIDDEN ]
    Size:    4
)");
  ASSERT_FALSE(Bytes.empty());
  expectGraphError(
      verifyTranslationLinkGraphV1(Bytes, Result.artifact().hostTarget(),
                                   Result.artifact().blockSymbols(),
                                   Result.artifact().runtimeSymbols(),
                                   Result.artifact().runtimeRegistryIdentity()),
      TranslationLinkGraphErrorCode::ObjectGraphCreationFailed);
}

TEST(TranslationLinkGraphVerifier,
     RejectsExtraLocalCallableExecutableBytesInELF) {
  llvm::Expected<TranslationObjectResultV1> ResultOrErr =
      compileFixture("aarch64-unknown-linux-gnu");
  ASSERT_TRUE(static_cast<bool>(ResultOrErr))
      << llvm::toString(ResultOrErr.takeError());
  TranslationObjectResultV1 Result = std::move(*ResultOrErr);
  const std::vector<uint8_t> Bytes = yamlArtifact(R"(
--- !ELF
FileHeader:
  Class:   ELFCLASS64
  Data:    ELFDATA2LSB
  Type:    ET_REL
  Machine: EM_AARCH64
Sections:
  - Name:         .text
    Type:         SHT_PROGBITS
    Flags:        [ SHF_ALLOC, SHF_EXECINSTR ]
    AddressAlign: 4
    Content:      "1F2003D5C0035FD6"
Symbols:
  - Name:    nvd_x86_64_block_0000000000401000
    Type:    STT_FUNC
    Section: .text
    Binding: STB_GLOBAL
    Other:   [ STV_HIDDEN ]
    Size:    4
  - Name:    hidden_payload
    Type:    STT_FUNC
    Section: .text
    Value:   4
    Size:    4
)");
  ASSERT_FALSE(Bytes.empty());
  expectGraphError(
      verifyTranslationLinkGraphV1(Bytes, Result.artifact().hostTarget(),
                                   Result.artifact().blockSymbols(),
                                   Result.artifact().runtimeSymbols(),
                                   Result.artifact().runtimeRegistryIdentity()),
      TranslationLinkGraphErrorCode::BlockSymbolManifestMismatch);
}

TEST(TranslationLinkGraphVerifier,
     RejectsDefinedBranch26TargetsWithExactELFBlockCoverage) {
  llvm::Expected<TranslationObjectResultV1> ResultOrErr =
      compileFixture("aarch64-unknown-linux-gnu");
  ASSERT_TRUE(static_cast<bool>(ResultOrErr))
      << llvm::toString(ResultOrErr.takeError());
  TranslationObjectResultV1 Result = std::move(*ResultOrErr);
  const std::vector<uint8_t> Bytes = yamlArtifact(R"(
--- !ELF
FileHeader:
  Class:   ELFCLASS64
  Data:    ELFDATA2LSB
  Type:    ET_REL
  Machine: EM_AARCH64
Sections:
  - Name:         .text
    Type:         SHT_PROGBITS
    Flags:        [ SHF_ALLOC, SHF_EXECINSTR ]
    AddressAlign: 4
    Content:      "01000014C0035FD6"
  - Name: .rela.text
    Type: SHT_RELA
    Info: .text
    Link: .symtab
    Relocations:
      - Offset: 0
        Type:   R_AARCH64_JUMP26
        Symbol: nvd_x86_64_block_0000000000401004
Symbols:
  - Name:    nvd_x86_64_block_0000000000401000
    Type:    STT_FUNC
    Section: .text
    Binding: STB_GLOBAL
    Other:   [ STV_HIDDEN ]
    Size:    4
  - Name:    nvd_x86_64_block_0000000000401004
    Type:    STT_FUNC
    Section: .text
    Binding: STB_GLOBAL
    Other:   [ STV_HIDDEN ]
    Value:   4
    Size:    4
)");
  std::vector<TranslationObjectSymbolV1> Blocks(
      Result.artifact().blockSymbols().begin(),
      Result.artifact().blockSymbols().end());
  Blocks.push_back({"nvd_x86_64_block_0000000000401004",
                    "nvd_x86_64_block_0000000000401004"});
  expectGraphError(
      verifyTranslationLinkGraphV1(Bytes, Result.artifact().hostTarget(),
                                   Blocks, Result.artifact().runtimeSymbols(),
                                   Result.artifact().runtimeRegistryIdentity()),
      TranslationLinkGraphErrorCode::EdgePolicyViolation);
}

TEST(TranslationLinkGraphVerifier, RejectsOverlappingExpectedELFBlockRanges) {
  llvm::Expected<TranslationObjectResultV1> ResultOrErr =
      compileFixture("aarch64-unknown-linux-gnu");
  ASSERT_TRUE(static_cast<bool>(ResultOrErr))
      << llvm::toString(ResultOrErr.takeError());
  TranslationObjectResultV1 Result = std::move(*ResultOrErr);
  const std::vector<uint8_t> Bytes = yamlArtifact(R"(
--- !ELF
FileHeader:
  Class:   ELFCLASS64
  Data:    ELFDATA2LSB
  Type:    ET_REL
  Machine: EM_AARCH64
Sections:
  - Name:         .text
    Type:         SHT_PROGBITS
    Flags:        [ SHF_ALLOC, SHF_EXECINSTR ]
    AddressAlign: 4
    Content:      "C0035FD6"
Symbols:
  - Name:    nvd_x86_64_block_0000000000401000
    Type:    STT_FUNC
    Section: .text
    Binding: STB_GLOBAL
    Other:   [ STV_HIDDEN ]
    Size:    4
  - Name:    nvd_x86_64_block_0000000000401004
    Type:    STT_FUNC
    Section: .text
    Binding: STB_GLOBAL
    Other:   [ STV_HIDDEN ]
    Size:    4
)");
  std::vector<TranslationObjectSymbolV1> Blocks(
      Result.artifact().blockSymbols().begin(),
      Result.artifact().blockSymbols().end());
  Blocks.push_back({"nvd_x86_64_block_0000000000401004",
                    "nvd_x86_64_block_0000000000401004"});
  expectGraphError(
      verifyTranslationLinkGraphV1(Bytes, Result.artifact().hostTarget(),
                                   Blocks, Result.artifact().runtimeSymbols(),
                                   Result.artifact().runtimeRegistryIdentity()),
      TranslationLinkGraphErrorCode::BlockSymbolManifestMismatch);
}

TEST(TranslationLinkGraphVerifier, RejectsDuplicateELFBranch26Fixups) {
  llvm::Expected<TranslationObjectResultV1> ResultOrErr =
      compileFixture("aarch64-unknown-linux-gnu");
  ASSERT_TRUE(static_cast<bool>(ResultOrErr))
      << llvm::toString(ResultOrErr.takeError());
  TranslationObjectResultV1 Result = std::move(*ResultOrErr);
  const std::vector<uint8_t> Bytes = yamlArtifact(R"(
--- !ELF
FileHeader:
  Class:   ELFCLASS64
  Data:    ELFDATA2LSB
  Type:    ET_REL
  Machine: EM_AARCH64
Sections:
  - Name:         .text
    Type:         SHT_PROGBITS
    Flags:        [ SHF_ALLOC, SHF_EXECINSTR ]
    AddressAlign: 4
    Content:      "00000094"
  - Name: .rela.text
    Type: SHT_RELA
    Info: .text
    Link: .symtab
    Relocations:
      - Offset: 0
        Type:   R_AARCH64_CALL26
        Symbol: nvd_rt_v1_load64_le
      - Offset: 0
        Type:   R_AARCH64_CALL26
        Symbol: nvd_rt_v1_load64_le
Symbols:
  - Name:    nvd_x86_64_block_0000000000401000
    Type:    STT_FUNC
    Section: .text
    Binding: STB_GLOBAL
    Other:   [ STV_HIDDEN ]
    Size:    4
  - Name:    nvd_rt_v1_load64_le
    Type:    STT_FUNC
    Binding: STB_GLOBAL
)");
  expectGraphError(
      verifyTranslationLinkGraphV1(Bytes, Result.artifact().hostTarget(),
                                   Result.artifact().blockSymbols(),
                                   Result.artifact().runtimeSymbols(),
                                   Result.artifact().runtimeRegistryIdentity()),
      TranslationLinkGraphErrorCode::EdgePolicyViolation);
}

TEST(TranslationLinkGraphVerifier,
     RejectsNonzeroEncodedELFBranch26ImmediateWithZeroEdgeAddend) {
  llvm::Expected<TranslationObjectResultV1> ResultOrErr =
      compileFixture("aarch64-unknown-linux-gnu");
  ASSERT_TRUE(static_cast<bool>(ResultOrErr))
      << llvm::toString(ResultOrErr.takeError());
  TranslationObjectResultV1 Result = std::move(*ResultOrErr);
  const std::vector<uint8_t> Bytes = yamlArtifact(R"(
--- !ELF
FileHeader:
  Class:   ELFCLASS64
  Data:    ELFDATA2LSB
  Type:    ET_REL
  Machine: EM_AARCH64
Sections:
  - Name:         .text
    Type:         SHT_PROGBITS
    Flags:        [ SHF_ALLOC, SHF_EXECINSTR ]
    AddressAlign: 4
    Content:      "01000094"
  - Name: .rela.text
    Type: SHT_RELA
    Info: .text
    Link: .symtab
    Relocations:
      - Offset: 0
        Type:   R_AARCH64_CALL26
        Symbol: nvd_rt_v1_load64_le
        Addend: 0
Symbols:
  - Name:    nvd_x86_64_block_0000000000401000
    Type:    STT_FUNC
    Section: .text
    Binding: STB_GLOBAL
    Other:   [ STV_HIDDEN ]
    Size:    4
  - Name:    nvd_rt_v1_load64_le
    Type:    STT_FUNC
    Binding: STB_GLOBAL
)");
  ASSERT_FALSE(Bytes.empty());
  expectGraphError(
      verifyTranslationLinkGraphV1(Bytes, Result.artifact().hostTarget(),
                                   Result.artifact().blockSymbols(),
                                   Result.artifact().runtimeSymbols(),
                                   Result.artifact().runtimeRegistryIdentity()),
      TranslationLinkGraphErrorCode::EdgePolicyViolation);
}

TEST(TranslationLinkGraphVerifier,
     RejectsAnonymousELFTextTailsAndMisalignedTextBlocks) {
  llvm::Expected<TranslationObjectResultV1> ResultOrErr =
      compileFixture("aarch64-unknown-linux-gnu");
  ASSERT_TRUE(static_cast<bool>(ResultOrErr))
      << llvm::toString(ResultOrErr.takeError());
  TranslationObjectResultV1 Result = std::move(*ResultOrErr);
  const std::vector<uint8_t> Tail = yamlArtifact(R"(
--- !ELF
FileHeader:
  Class:   ELFCLASS64
  Data:    ELFDATA2LSB
  Type:    ET_REL
  Machine: EM_AARCH64
Sections:
  - Name:         .text
    Type:         SHT_PROGBITS
    Flags:        [ SHF_ALLOC, SHF_EXECINSTR ]
    AddressAlign: 4
    Content:      "C0035FD61F2003D5"
Symbols:
  - Name:    nvd_x86_64_block_0000000000401000
    Type:    STT_FUNC
    Section: .text
    Binding: STB_GLOBAL
    Other:   [ STV_HIDDEN ]
    Size:    4
)");
  expectGraphError(
      verifyTranslationLinkGraphV1(Tail, Result.artifact().hostTarget(),
                                   Result.artifact().blockSymbols(),
                                   Result.artifact().runtimeSymbols(),
                                   Result.artifact().runtimeRegistryIdentity()),
      TranslationLinkGraphErrorCode::BlockSymbolManifestMismatch);

  const std::vector<uint8_t> Misaligned = yamlArtifact(R"(
--- !ELF
FileHeader:
  Class:   ELFCLASS64
  Data:    ELFDATA2LSB
  Type:    ET_REL
  Machine: EM_AARCH64
Sections:
  - Name:         .text
    Type:         SHT_PROGBITS
    Flags:        [ SHF_ALLOC, SHF_EXECINSTR ]
    AddressAlign: 1
    Content:      "C0035FD6"
Symbols:
  - Name:    nvd_x86_64_block_0000000000401000
    Type:    STT_FUNC
    Section: .text
    Binding: STB_GLOBAL
    Other:   [ STV_HIDDEN ]
    Size:    4
)");
  expectGraphError(
      verifyTranslationLinkGraphV1(Misaligned, Result.artifact().hostTarget(),
                                   Result.artifact().blockSymbols(),
                                   Result.artifact().runtimeSymbols(),
                                   Result.artifact().runtimeRegistryIdentity()),
      TranslationLinkGraphErrorCode::SectionPolicyViolation);
}

TEST(TranslationLinkGraphVerifier, RejectsDuplicateMachOBranch26Fixups) {
  llvm::Expected<TranslationObjectResultV1> ResultOrErr =
      compileFixture("aarch64-apple-darwin");
  ASSERT_TRUE(static_cast<bool>(ResultOrErr))
      << llvm::toString(ResultOrErr.takeError());
  TranslationObjectResultV1 Result = std::move(*ResultOrErr);
  const std::vector<uint8_t> Bytes = yamlArtifact(R"(
--- !mach-o
FileHeader:
  magic:      0xFEEDFACF
  cputype:    0x0100000C
  cpusubtype: 0x00000000
  filetype:   0x00000001
  ncmds:      2
  sizeofcmds: 176
  flags:      0x00002000
  reserved:   0x00000000
LoadCommands:
  - cmd:      LC_SEGMENT_64
    cmdsize:  152
    segname:  ''
    vmaddr:   0
    vmsize:   4
    fileoff:  208
    filesize: 4
    maxprot:  5
    initprot: 5
    nsects:   1
    flags:    0
    Sections:
      - sectname:  __text
        segname:   __TEXT
        addr:      0
        size:      4
        offset:    208
        align:     2
        reloff:    212
        nreloc:    2
        flags:     0x80000400
        reserved1: 0
        reserved2: 0
        reserved3: 0
        content:   00000094
        relocations:
          - address:   0
            symbolnum: 1
            pcrel:     true
            length:    2
            extern:    true
            type:      2
            scattered: false
            value:     0
          - address:   0
            symbolnum: 1
            pcrel:     true
            length:    2
            extern:    true
            type:      2
            scattered: false
            value:     0
  - cmd:      LC_SYMTAB
    cmdsize:  24
    symoff:   228
    nsyms:    2
    stroff:   260
    strsize:  57
LinkEditData:
  NameList:
    - n_strx:  1
      n_type:  0x1F
      n_sect:  1
      n_desc:  0
      n_value: 0
    - n_strx:  36
      n_type:  0x01
      n_sect:  0
      n_desc:  0
      n_value: 0
  StringTable:
    - ''
    - _nvd_x86_64_block_0000000000401000
    - _nvd_rt_v1_load64_le
)");
  expectGraphError(
      verifyTranslationLinkGraphV1(Bytes, Result.artifact().hostTarget(),
                                   Result.artifact().blockSymbols(),
                                   Result.artifact().runtimeSymbols(),
                                   Result.artifact().runtimeRegistryIdentity()),
      TranslationLinkGraphErrorCode::EdgePolicyViolation);
}

TEST(TranslationLinkGraphVerifier,
     RejectsDefinedBranch26TargetsWithExactMachOBlockCoverage) {
  llvm::Expected<TranslationObjectResultV1> ResultOrErr =
      compileFixture("aarch64-apple-darwin");
  ASSERT_TRUE(static_cast<bool>(ResultOrErr))
      << llvm::toString(ResultOrErr.takeError());
  TranslationObjectResultV1 Result = std::move(*ResultOrErr);
  const std::vector<uint8_t> Bytes = yamlArtifact(R"(
--- !mach-o
FileHeader:
  magic:      0xFEEDFACF
  cputype:    0x0100000C
  cpusubtype: 0x00000000
  filetype:   0x00000001
  ncmds:      2
  sizeofcmds: 176
  flags:      0x00002000
  reserved:   0x00000000
LoadCommands:
  - cmd:      LC_SEGMENT_64
    cmdsize:  152
    segname:  ''
    vmaddr:   0
    vmsize:   8
    fileoff:  208
    filesize: 8
    maxprot:  5
    initprot: 5
    nsects:   1
    flags:    0
    Sections:
      - sectname:  __text
        segname:   __TEXT
        addr:      0
        size:      8
        offset:    208
        align:     2
        reloff:    216
        nreloc:    1
        flags:     0x80000400
        reserved1: 0
        reserved2: 0
        reserved3: 0
        content:   00000014C0035FD6
        relocations:
          - address:   0
            symbolnum: 1
            pcrel:     true
            length:    2
            extern:    true
            type:      2
            scattered: false
            value:     0
  - cmd:      LC_SYMTAB
    cmdsize:  24
    symoff:   224
    nsyms:    2
    stroff:   256
    strsize:  71
LinkEditData:
  NameList:
    - n_strx:  1
      n_type:  0x1F
      n_sect:  1
      n_desc:  0
      n_value: 0
    - n_strx:  36
      n_type:  0x1F
      n_sect:  1
      n_desc:  0
      n_value: 4
  StringTable:
    - ''
    - _nvd_x86_64_block_0000000000401000
    - _nvd_x86_64_block_0000000000401004
)");
  std::vector<TranslationObjectSymbolV1> Blocks(
      Result.artifact().blockSymbols().begin(),
      Result.artifact().blockSymbols().end());
  Blocks.push_back({"nvd_x86_64_block_0000000000401004",
                    "_nvd_x86_64_block_0000000000401004"});
  expectGraphError(
      verifyTranslationLinkGraphV1(Bytes, Result.artifact().hostTarget(),
                                   Blocks, Result.artifact().runtimeSymbols(),
                                   Result.artifact().runtimeRegistryIdentity()),
      TranslationLinkGraphErrorCode::EdgePolicyViolation);
}

TEST(TranslationLinkGraphVerifier,
     RejectsAnonymousMachOTextPrefixesAndMisalignedTextBlocks) {
  llvm::Expected<TranslationObjectResultV1> ResultOrErr =
      compileFixture("aarch64-apple-darwin");
  ASSERT_TRUE(static_cast<bool>(ResultOrErr))
      << llvm::toString(ResultOrErr.takeError());
  TranslationObjectResultV1 Result = std::move(*ResultOrErr);
  const std::vector<uint8_t> Prefix = yamlArtifact(R"(
--- !mach-o
FileHeader:
  magic:      0xFEEDFACF
  cputype:    0x0100000C
  cpusubtype: 0x00000000
  filetype:   0x00000001
  ncmds:      2
  sizeofcmds: 176
  flags:      0x00002000
  reserved:   0x00000000
LoadCommands:
  - cmd:      LC_SEGMENT_64
    cmdsize:  152
    segname:  ''
    vmaddr:   0
    vmsize:   8
    fileoff:  208
    filesize: 8
    maxprot:  5
    initprot: 5
    nsects:   1
    flags:    0
    Sections:
      - sectname:  __text
        segname:   __TEXT
        addr:      0
        size:      8
        offset:    208
        align:     2
        reloff:    0
        nreloc:    0
        flags:     0x80000400
        reserved1: 0
        reserved2: 0
        reserved3: 0
        content:   1F2003D5C0035FD6
  - cmd:      LC_SYMTAB
    cmdsize:  24
    symoff:   216
    nsyms:    1
    stroff:   232
    strsize:  36
LinkEditData:
  NameList:
    - n_strx:  1
      n_type:  0x1F
      n_sect:  1
      n_desc:  0
      n_value: 4
  StringTable:
    - ''
    - _nvd_x86_64_block_0000000000401000
)");
  expectGraphError(
      verifyTranslationLinkGraphV1(Prefix, Result.artifact().hostTarget(),
                                   Result.artifact().blockSymbols(),
                                   Result.artifact().runtimeSymbols(),
                                   Result.artifact().runtimeRegistryIdentity()),
      TranslationLinkGraphErrorCode::BlockSymbolManifestMismatch);

  std::string MisalignedYAML = R"(
--- !mach-o
FileHeader:
  magic:      0xFEEDFACF
  cputype:    0x0100000C
  cpusubtype: 0x00000000
  filetype:   0x00000001
  ncmds:      2
  sizeofcmds: 176
  flags:      0x00002000
  reserved:   0x00000000
LoadCommands:
  - cmd:      LC_SEGMENT_64
    cmdsize:  152
    segname:  ''
    vmaddr:   0
    vmsize:   4
    fileoff:  208
    filesize: 4
    maxprot:  5
    initprot: 5
    nsects:   1
    flags:    0
    Sections:
      - sectname:  __text
        segname:   __TEXT
        addr:      0
        size:      4
        offset:    208
        align:     1
        reloff:    0
        nreloc:    0
        flags:     0x80000400
        reserved1: 0
        reserved2: 0
        reserved3: 0
        content:   C0035FD6
  - cmd:      LC_SYMTAB
    cmdsize:  24
    symoff:   212
    nsyms:    1
    stroff:   228
    strsize:  36
LinkEditData:
  NameList:
    - n_strx:  1
      n_type:  0x1F
      n_sect:  1
      n_desc:  0
      n_value: 0
  StringTable:
    - ''
    - _nvd_x86_64_block_0000000000401000
)";
  const std::vector<uint8_t> Misaligned = yamlArtifact(MisalignedYAML);
  expectGraphError(
      verifyTranslationLinkGraphV1(Misaligned, Result.artifact().hostTarget(),
                                   Result.artifact().blockSymbols(),
                                   Result.artifact().runtimeSymbols(),
                                   Result.artifact().runtimeRegistryIdentity()),
      TranslationLinkGraphErrorCode::SectionPolicyViolation);
}

TEST(TranslationLinkGraphVerifier, RejectsHostAndBlockManifestMismatch) {
  llvm::Expected<TranslationObjectResultV1> ResultOrErr =
      compileFixture("aarch64-unknown-linux-gnu");
  ASSERT_TRUE(static_cast<bool>(ResultOrErr))
      << llvm::toString(ResultOrErr.takeError());
  TranslationObjectResultV1 Result = std::move(*ResultOrErr);
  const TranslationObjectArtifactV1 &Artifact = Result.artifact();

  llvm::Expected<ResolvedHostTarget> MachOTargetOrErr =
      resolveHostTarget(aarch64Options("aarch64-apple-macosx"));
  ASSERT_TRUE(static_cast<bool>(MachOTargetOrErr))
      << llvm::toString(MachOTargetOrErr.takeError());
  std::vector<TranslationObjectSymbolV1> MachOBlocks(
      Artifact.blockSymbols().begin(), Artifact.blockSymbols().end());
  std::vector<TranslationObjectSymbolV1> MachORuntime(
      Artifact.runtimeSymbols().begin(), Artifact.runtimeSymbols().end());
  for (TranslationObjectSymbolV1 &Symbol : MachOBlocks)
    Symbol.ObjectName = "_" + Symbol.IRName;
  for (TranslationObjectSymbolV1 &Symbol : MachORuntime)
    Symbol.ObjectName = "_" + Symbol.IRName;
  expectGraphError(verifyTranslationLinkGraphV1(
                       Artifact.bytes(), *MachOTargetOrErr, MachOBlocks,
                       MachORuntime, Artifact.runtimeRegistryIdentity()),
                   TranslationLinkGraphErrorCode::GraphTargetMismatch);

  std::vector<TranslationObjectSymbolV1> WrongBlocks(
      Artifact.blockSymbols().begin(), Artifact.blockSymbols().end());
  ASSERT_EQ(WrongBlocks.size(), 1u);
  WrongBlocks.front().IRName = (BlockIRName + "_wrong").str();
  WrongBlocks.front().ObjectName = WrongBlocks.front().IRName;
  expectGraphError(
      verifyTranslationLinkGraphV1(Artifact.bytes(), Artifact.hostTarget(),
                                   WrongBlocks, Artifact.runtimeSymbols(),
                                   Artifact.runtimeRegistryIdentity()),
      TranslationLinkGraphErrorCode::BlockSymbolManifestMismatch);
}

} // namespace
