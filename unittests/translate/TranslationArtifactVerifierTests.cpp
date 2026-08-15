//===- TranslationArtifactVerifierTests.cpp - Artifact boundary tests ----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "neverd/translate/TranslationArtifactVerifier.h"

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/ADT/Twine.h"
#include "llvm/Object/ObjectFile.h"
#include "llvm/ObjectYAML/yaml2obj.h"
#include "llvm/Support/Error.h"
#include "llvm/Support/MemoryBufferRef.h"
#include "llvm/TargetParser/Triple.h"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace {

using neverd::translate::TranslationArtifactPolicyV1;
using neverd::translate::TranslationArtifactVerificationError;
using neverd::translate::TranslationArtifactViolation;

const llvm::Triple HostTriple("x86_64-unknown-linux-gnu");

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

void expectViolation(llvm::Error Error,
                     TranslationArtifactViolation ExpectedReason) {
  ASSERT_TRUE(static_cast<bool>(Error));
  bool Seen = false;
  llvm::handleAllErrors(
      std::move(Error),
      [&](const TranslationArtifactVerificationError &Failure) {
        Seen = true;
        EXPECT_EQ(Failure.reason(), ExpectedReason);
      });
  EXPECT_TRUE(Seen);
}

TEST(TranslationArtifactVerifier,
     AcceptsObjectLocalAndExactRuntimeRelocations) {
  const std::vector<uint8_t> Bytes = yamlArtifact(R"(
--- !ELF
FileHeader:
  Class:   ELFCLASS64
  Data:    ELFDATA2LSB
  Type:    ET_REL
  Machine: EM_X86_64
Sections:
  - Name:    .text
    Type:    SHT_PROGBITS
    Flags:   [ SHF_ALLOC, SHF_EXECINSTR ]
    Content: "0000000000000000"
  - Name: .rela.text
    Type: SHT_RELA
    Info: .text
    Link: .symtab
    Relocations:
      - Offset: 0
        Type:   R_X86_64_PC32
        Symbol: local_block
      - Offset: 4
        Type:   R_X86_64_PC32
        Symbol: nvd_rt_helper
Symbols:
  - Name:    local_block
    Type:    STT_FUNC
    Section: .text
  - Name:    nvd_rt_helper
    Type:    STT_FUNC
    Binding: STB_GLOBAL
)");
  const llvm::StringRef Allowed[] = {"nvd_rt_helper"};
  const llvm::StringRef Data(reinterpret_cast<const char *>(Bytes.data()),
                             Bytes.size());
  EXPECT_FALSE(static_cast<bool>(neverd::translate::verifyTranslationArtifact(
      llvm::MemoryBufferRef(Data, "canonical translation object"), HostTriple,
      Allowed)));
}

TEST(TranslationArtifactVerifier, AcceptsCanonicalCOFFObject) {
  const std::vector<uint8_t> Bytes = yamlArtifact(R"(
--- !COFF
header:
  Machine:         IMAGE_FILE_MACHINE_AMD64
  Characteristics: [ ]
sections:
  - Name:            .text
    Characteristics: [ IMAGE_SCN_CNT_CODE, IMAGE_SCN_MEM_EXECUTE, IMAGE_SCN_MEM_READ ]
    Alignment:       4
    SectionData:     "0000000000000000"
    Relocations:
      - VirtualAddress: 0
        SymbolName:     local_block
        Type:           IMAGE_REL_AMD64_REL32
      - VirtualAddress: 4
        SymbolName:     nvd_rt_helper
        Type:           IMAGE_REL_AMD64_REL32
symbols:
  - Name:            local_block
    Value:           0
    SectionNumber:   1
    SimpleType:      IMAGE_SYM_TYPE_NULL
    ComplexType:     IMAGE_SYM_DTYPE_NULL
    StorageClass:    IMAGE_SYM_CLASS_STATIC
  - Name:            nvd_rt_helper
    Value:           0
    SectionNumber:   0
    SimpleType:      IMAGE_SYM_TYPE_NULL
    ComplexType:     IMAGE_SYM_DTYPE_NULL
    StorageClass:    IMAGE_SYM_CLASS_EXTERNAL
)");
  const llvm::StringRef Allowed[] = {"nvd_rt_helper"};
  EXPECT_FALSE(static_cast<bool>(neverd::translate::verifyTranslationArtifact(
      Bytes, llvm::Triple("x86_64-pc-windows-msvc"), Allowed)));
}

TEST(TranslationArtifactVerifier, AcceptsCanonicalMachOObject) {
  const std::vector<uint8_t> Bytes = yamlArtifact(R"(
--- !mach-o
FileHeader:
  magic:      0xFEEDFACF
  cputype:    0x01000007
  cpusubtype: 0x00000003
  filetype:   0x00000001
  ncmds:      1
  sizeofcmds: 232
  flags:      0x00002000
  reserved:   0x00000000
LoadCommands:
  - cmd:      LC_SEGMENT_64
    cmdsize:  232
    segname:  ''
    vmaddr:   0
    vmsize:   4
    fileoff:  392
    filesize: 4
    maxprot:  7
    initprot: 7
    nsects:   1
    flags:    0
    Sections:
      - sectname:  __data
        segname:   __DATA
        addr:      0
        size:      4
        offset:    392
        align:     2
        reloff:    0
        nreloc:    0
        flags:     0
        reserved1: 0
        reserved2: 0
        reserved3: 0
        content:   CDAB3412
)");
  EXPECT_FALSE(static_cast<bool>(neverd::translate::verifyTranslationArtifact(
      Bytes, llvm::Triple("x86_64-apple-macosx"))));
}

TEST(TranslationArtifactVerifier, RejectsMachOLinkerLoadCommands) {
  const std::vector<uint8_t> Bytes = yamlArtifact(R"(
--- !mach-o
FileHeader:
  magic:      0xFEEDFACF
  cputype:    0x01000007
  cpusubtype: 0x00000003
  filetype:   0x00000001
  ncmds:      1
  sizeofcmds: 16
  flags:      0x00002000
  reserved:   0x00000000
LoadCommands:
  - cmd:          LC_LINKER_OPTION
    cmdsize:      16
    count:        1
    PayloadBytes: [ 0x2D, 0x6C, 0x63, 0x0 ]
)");
  expectViolation(neverd::translate::verifyTranslationArtifact(
                      Bytes, llvm::Triple("x86_64-apple-macosx")),
                  TranslationArtifactViolation::UnsupportedLoadCommand);
}

TEST(TranslationArtifactVerifier, RejectsMalformedObject) {
  const uint8_t Bytes[] = {0x7f, 'E', 'L', 'F'};
  expectViolation(
      neverd::translate::verifyTranslationArtifact(Bytes, HostTriple),
      TranslationArtifactViolation::MalformedObject);
}

TEST(TranslationArtifactVerifier, RejectsFormatAndArchitectureMismatch) {
  const std::vector<uint8_t> Bytes = yamlArtifact(R"(
--- !ELF
FileHeader:
  Class:   ELFCLASS64
  Data:    ELFDATA2LSB
  Type:    ET_REL
  Machine: EM_X86_64
Sections:
  - Name: .text
    Type: SHT_PROGBITS
)");
  expectViolation(neverd::translate::verifyTranslationArtifact(
                      Bytes, llvm::Triple("x86_64-pc-windows-msvc")),
                  TranslationArtifactViolation::ObjectFormatMismatch);
  expectViolation(neverd::translate::verifyTranslationArtifact(
                      Bytes, llvm::Triple("aarch64-unknown-linux-gnu")),
                  TranslationArtifactViolation::HostArchitectureMismatch);

  const std::vector<uint8_t> BigEndian = yamlArtifact(R"(
--- !ELF
FileHeader:
  Class:   ELFCLASS64
  Data:    ELFDATA2MSB
  Type:    ET_REL
  Machine: EM_X86_64
Sections:
  - Name: .text
    Type: SHT_PROGBITS
)");
  expectViolation(
      neverd::translate::verifyTranslationArtifact(BigEndian, HostTriple),
      TranslationArtifactViolation::HostArchitectureMismatch);
}

TEST(TranslationArtifactVerifier, RejectsLinkedArtifact) {
  const std::vector<uint8_t> Bytes = yamlArtifact(R"(
--- !ELF
FileHeader:
  Class:   ELFCLASS64
  Data:    ELFDATA2LSB
  Type:    ET_DYN
  Machine: EM_X86_64
Sections:
  - Name: .text
    Type: SHT_PROGBITS
)");
  expectViolation(
      neverd::translate::verifyTranslationArtifact(Bytes, HostTriple),
      TranslationArtifactViolation::UnsupportedArtifactKind);
}

TEST(TranslationArtifactVerifier, RejectsSegmentsInRelocatableELFObjects) {
  const std::vector<uint8_t> Bytes = yamlArtifact(R"(
--- !ELF
FileHeader:
  Class:   ELFCLASS64
  Data:    ELFDATA2LSB
  Type:    ET_REL
  Machine: EM_X86_64
Sections:
  - Name:         .text
    Type:         SHT_PROGBITS
    Flags:        [ SHF_ALLOC, SHF_EXECINSTR ]
    AddressAlign: 16
    Content:      "00000000"
ProgramHeaders:
  - Type:     PT_LOAD
    Flags:    [ PF_X, PF_R ]
    FirstSec: .text
    LastSec:  .text
)");
  expectViolation(
      neverd::translate::verifyTranslationArtifact(Bytes, HostTriple),
      TranslationArtifactViolation::UnsupportedArtifactKind);
}

TEST(TranslationArtifactVerifier, RejectsInvalidRuntimeSymbolPolicy) {
  const std::vector<uint8_t> Bytes = yamlArtifact(R"(
--- !ELF
FileHeader:
  Class:   ELFCLASS64
  Data:    ELFDATA2LSB
  Type:    ET_REL
  Machine: EM_X86_64
)");
  const llvm::StringRef Duplicate[] = {"nvd_rt_helper", "nvd_rt_helper"};
  expectViolation(neverd::translate::verifyTranslationArtifact(
                      Bytes, HostTriple, Duplicate),
                  TranslationArtifactViolation::InvalidPolicy);
}

TEST(TranslationArtifactVerifier,
     LegacyOverloadAcceptsExplicitEmptyRuntimeAllowlist) {
  const std::vector<uint8_t> Bytes = yamlArtifact(R"(
--- !ELF
FileHeader:
  Class:   ELFCLASS64
  Data:    ELFDATA2LSB
  Type:    ET_REL
  Machine: EM_X86_64
)");
  EXPECT_FALSE(static_cast<bool>(
      neverd::translate::verifyTranslationArtifact(Bytes, HostTriple, {})));
}

TEST(TranslationArtifactVerifier, RejectsV1PolicyWithoutRequiredBlock) {
  const std::vector<uint8_t> Bytes = yamlArtifact(R"(
--- !ELF
FileHeader:
  Class:   ELFCLASS64
  Data:    ELFDATA2LSB
  Type:    ET_REL
  Machine: EM_X86_64
)");
  const TranslationArtifactPolicyV1 Policy(llvm::ArrayRef<llvm::StringRef>{});
  expectViolation(
      neverd::translate::verifyTranslationArtifact(Bytes, HostTriple, Policy),
      TranslationArtifactViolation::InvalidPolicy);

  const llvm::StringRef DuplicateBlocks[] = {"translated_block",
                                             "translated_block"};
  const TranslationArtifactPolicyV1 DuplicatePolicy{DuplicateBlocks, {}};
  expectViolation(neverd::translate::verifyTranslationArtifact(
                      Bytes, HostTriple, DuplicatePolicy),
                  TranslationArtifactViolation::InvalidPolicy);

  const llvm::StringRef RequiredBlocks[] = {"translated_block"};
  const llvm::StringRef OverlappingAllowed[] = {"translated_block"};
  const TranslationArtifactPolicyV1 OverlappingPolicy{RequiredBlocks,
                                                      OverlappingAllowed};
  expectViolation(neverd::translate::verifyTranslationArtifact(
                      Bytes, HostTriple, OverlappingPolicy),
                  TranslationArtifactViolation::InvalidPolicy);
}

TEST(TranslationArtifactVerifier, RejectsMissingV1RequiredBlock) {
  const std::vector<uint8_t> Bytes = yamlArtifact(R"(
--- !ELF
FileHeader:
  Class:   ELFCLASS64
  Data:    ELFDATA2LSB
  Type:    ET_REL
  Machine: EM_X86_64
)");
  const llvm::StringRef RequiredBlocks[] = {"translated_block"};
  const TranslationArtifactPolicyV1 Policy{RequiredBlocks, {}};
  expectViolation(
      neverd::translate::verifyTranslationArtifact(Bytes, HostTriple, Policy),
      TranslationArtifactViolation::RequiredBlockMissing);
}

TEST(TranslationArtifactVerifier, RejectsZeroSizedV1BlockDefinition) {
  const std::vector<uint8_t> Bytes = yamlArtifact(R"(
--- !ELF
FileHeader:
  Class:   ELFCLASS64
  Data:    ELFDATA2LSB
  Type:    ET_REL
  Machine: EM_X86_64
Sections:
  - Name:    .text
    Type:    SHT_PROGBITS
    Flags:   [ SHF_ALLOC, SHF_EXECINSTR ]
    Content: "C3"
Symbols:
  - Name:    translated_block
    Type:    STT_FUNC
    Section: .text
    Size:    0
)");
  const llvm::StringRef RequiredBlocks[] = {"translated_block"};
  const TranslationArtifactPolicyV1 Policy{RequiredBlocks, {}};
  expectViolation(
      neverd::translate::verifyTranslationArtifact(Bytes, HostTriple, Policy),
      TranslationArtifactViolation::InvalidBlockDefinition);
}

TEST(TranslationArtifactVerifier, AcceptsV1RequiredBlockManifest) {
  const std::vector<uint8_t> Bytes = yamlArtifact(R"(
--- !ELF
FileHeader:
  Class:   ELFCLASS64
  Data:    ELFDATA2LSB
  Type:    ET_REL
  Machine: EM_X86_64
Sections:
  - Name:    .text
    Type:    SHT_PROGBITS
    Flags:   [ SHF_ALLOC, SHF_EXECINSTR ]
    Content: "C3"
Symbols:
  - Name:    translated_block
    Type:    STT_FUNC
    Section: .text
    Size:    1
)");
  const llvm::StringRef RequiredBlocks[] = {"translated_block"};
  const TranslationArtifactPolicyV1 Policy{RequiredBlocks, {}};
  EXPECT_FALSE(static_cast<bool>(
      neverd::translate::verifyTranslationArtifact(Bytes, HostTriple, Policy)));
}

TEST(TranslationArtifactVerifier, RejectsInvalidV1BlockDefinitions) {
  const std::vector<uint8_t> DataBlock = yamlArtifact(R"(
--- !ELF
FileHeader:
  Class:   ELFCLASS64
  Data:    ELFDATA2LSB
  Type:    ET_REL
  Machine: EM_X86_64
Sections:
  - Name:    .data
    Type:    SHT_PROGBITS
    Flags:   [ SHF_ALLOC, SHF_WRITE ]
    Content: "00"
Symbols:
  - Name:    translated_block
    Type:    STT_FUNC
    Section: .data
    Size:    1
)");
  const llvm::StringRef RequiredBlocks[] = {"translated_block"};
  const TranslationArtifactPolicyV1 Policy{RequiredBlocks, {}};
  expectViolation(neverd::translate::verifyTranslationArtifact(
                      DataBlock, HostTriple, Policy),
                  TranslationArtifactViolation::InvalidBlockDefinition);

  const std::vector<uint8_t> PreemptibleBlock = yamlArtifact(R"(
--- !ELF
FileHeader:
  Class:   ELFCLASS64
  Data:    ELFDATA2LSB
  Type:    ET_REL
  Machine: EM_X86_64
Sections:
  - Name:    .text
    Type:    SHT_PROGBITS
    Flags:   [ SHF_ALLOC, SHF_EXECINSTR ]
    Content: "C3"
Symbols:
  - Name:    translated_block
    Type:    STT_FUNC
    Binding: STB_GLOBAL
    Section: .text
    Size:    1
)");
  expectViolation(neverd::translate::verifyTranslationArtifact(
                      PreemptibleBlock, HostTriple, Policy),
                  TranslationArtifactViolation::InvalidBlockDefinition);
}

TEST(TranslationArtifactVerifier,
     RejectsExternallyResolvableBlockOutsideV1Manifest) {
  const std::vector<uint8_t> Bytes = yamlArtifact(R"(
--- !ELF
FileHeader:
  Class:   ELFCLASS64
  Data:    ELFDATA2LSB
  Type:    ET_REL
  Machine: EM_X86_64
Sections:
  - Name:    .text
    Type:    SHT_PROGBITS
    Flags:   [ SHF_ALLOC, SHF_EXECINSTR ]
    Content: "C390"
Symbols:
  - Name:    translated_block
    Type:    STT_FUNC
    Section: .text
    Size:    1
  - Name:    undeclared_block
    Type:    STT_FUNC
    Binding: STB_GLOBAL
    Other:   [ STV_HIDDEN ]
    Section: .text
    Value:   1
    Size:    1
)");
  const llvm::StringRef RequiredBlocks[] = {"translated_block"};
  const TranslationArtifactPolicyV1 Policy{RequiredBlocks, {}};
  expectViolation(
      neverd::translate::verifyTranslationArtifact(Bytes, HostTriple, Policy),
      TranslationArtifactViolation::UnexpectedBlockDefinition);
}

TEST(TranslationArtifactVerifier,
     RejectsAbsoluteELFRuntimeHelperRelocationInV1Policy) {
  const std::vector<uint8_t> Bytes = yamlArtifact(R"(
--- !ELF
FileHeader:
  Class:   ELFCLASS64
  Data:    ELFDATA2LSB
  Type:    ET_REL
  Machine: EM_X86_64
Sections:
  - Name:    .text
    Type:    SHT_PROGBITS
    Flags:   [ SHF_ALLOC, SHF_EXECINSTR ]
    Content: "0000000000000000"
  - Name: .rela.text
    Type: SHT_RELA
    Info: .text
    Link: .symtab
    Relocations:
      - Offset: 0
        Type:   R_X86_64_64
        Symbol: nvd_rt_helper
Symbols:
  - Name:    translated_block
    Type:    STT_FUNC
    Section: .text
    Size:    8
  - Name:    nvd_rt_helper
    Type:    STT_FUNC
    Binding: STB_GLOBAL
)");
  const llvm::StringRef RequiredBlocks[] = {"translated_block"};
  const llvm::StringRef Allowed[] = {"nvd_rt_helper"};
  const TranslationArtifactPolicyV1 Policy{RequiredBlocks, Allowed};
  expectViolation(
      neverd::translate::verifyTranslationArtifact(Bytes, HostTriple, Policy),
      TranslationArtifactViolation::RelocationTypeNotAllowed);
}

TEST(TranslationArtifactVerifier, AcceptsDirectELFRuntimeHelperCallInV1Policy) {
  const std::vector<uint8_t> Bytes = yamlArtifact(R"(
--- !ELF
FileHeader:
  Class:   ELFCLASS64
  Data:    ELFDATA2LSB
  Type:    ET_REL
  Machine: EM_X86_64
Sections:
  - Name:    .text
    Type:    SHT_PROGBITS
    Flags:   [ SHF_ALLOC, SHF_EXECINSTR ]
    Content: "E800000000C3"
  - Name: .rela.text
    Type: SHT_RELA
    Info: .text
    Link: .symtab
    Relocations:
      - Offset: 1
        Type:   R_X86_64_PC32
        Symbol: nvd_rt_helper
Symbols:
  - Name:    translated_block
    Type:    STT_FUNC
    Section: .text
    Size:    6
  - Name:    nvd_rt_helper
    Type:    STT_FUNC
    Binding: STB_GLOBAL
)");
  const llvm::StringRef RequiredBlocks[] = {"translated_block"};
  const llvm::StringRef Allowed[] = {"nvd_rt_helper"};
  const TranslationArtifactPolicyV1 Policy{RequiredBlocks, Allowed};
  EXPECT_FALSE(static_cast<bool>(
      neverd::translate::verifyTranslationArtifact(Bytes, HostTriple, Policy)));
}

TEST(TranslationArtifactVerifier,
     AcceptsOnlySealedLLVMPLT32RuntimeCallsInV1Policy) {
  const std::vector<uint8_t> Hidden = yamlArtifact(R"(
--- !ELF
FileHeader:
  Class:   ELFCLASS64
  Data:    ELFDATA2LSB
  Type:    ET_REL
  Machine: EM_X86_64
Sections:
  - Name:    .text
    Type:    SHT_PROGBITS
    Flags:   [ SHF_ALLOC, SHF_EXECINSTR ]
    Content: "E800000000C3"
  - Name: .rela.text
    Type: SHT_RELA
    Info: .text
    Link: .symtab
    Relocations:
      - Offset: 1
        Type:   R_X86_64_PLT32
        Symbol: nvd_rt_helper
Symbols:
  - Name:    translated_block
    Type:    STT_FUNC
    Section: .text
    Size:    6
  - Name:    nvd_rt_helper
    Type:    STT_FUNC
    Binding: STB_GLOBAL
    Other:   [ STV_HIDDEN ]
)");
  const llvm::StringRef RequiredBlocks[] = {"translated_block"};
  const llvm::StringRef Allowed[] = {"nvd_rt_helper"};
  const TranslationArtifactPolicyV1 Policy{RequiredBlocks, Allowed};
  EXPECT_FALSE(static_cast<bool>(neverd::translate::verifyTranslationArtifact(
      Hidden, HostTriple, Policy)));
  expectViolation(
      neverd::translate::verifyTranslationArtifact(Hidden, HostTriple, Allowed),
      TranslationArtifactViolation::RelocationTypeNotAllowed);

  const std::vector<uint8_t> Preemptible = yamlArtifact(R"(
--- !ELF
FileHeader:
  Class:   ELFCLASS64
  Data:    ELFDATA2LSB
  Type:    ET_REL
  Machine: EM_X86_64
Sections:
  - Name:    .text
    Type:    SHT_PROGBITS
    Flags:   [ SHF_ALLOC, SHF_EXECINSTR ]
    Content: "E800000000C3"
  - Name: .rela.text
    Type: SHT_RELA
    Info: .text
    Link: .symtab
    Relocations:
      - Offset: 1
        Type:   R_X86_64_PLT32
        Symbol: nvd_rt_helper
Symbols:
  - Name:    translated_block
    Type:    STT_FUNC
    Section: .text
    Size:    6
  - Name:    nvd_rt_helper
    Type:    STT_FUNC
    Binding: STB_GLOBAL
)");
  expectViolation(neverd::translate::verifyTranslationArtifact(
                      Preemptible, HostTriple, Policy),
                  TranslationArtifactViolation::RelocationTypeNotAllowed);
}

TEST(TranslationArtifactVerifier,
     RejectsELFPCRelativeRuntimeHelperMaterializationInV1Policy) {
  const std::vector<uint8_t> Bytes = yamlArtifact(R"(
--- !ELF
FileHeader:
  Class:   ELFCLASS64
  Data:    ELFDATA2LSB
  Type:    ET_REL
  Machine: EM_X86_64
Sections:
  - Name:    .text
    Type:    SHT_PROGBITS
    Flags:   [ SHF_ALLOC, SHF_EXECINSTR ]
    Content: "0000000000000000"
  - Name: .rela.text
    Type: SHT_RELA
    Info: .text
    Link: .symtab
    Relocations:
      - Offset: 0
        Type:   R_X86_64_PC32
        Symbol: nvd_rt_helper
Symbols:
  - Name:    translated_block
    Type:    STT_FUNC
    Section: .text
    Size:    8
  - Name:    nvd_rt_helper
    Type:    STT_FUNC
    Binding: STB_GLOBAL
)");
  const llvm::StringRef RequiredBlocks[] = {"translated_block"};
  const llvm::StringRef Allowed[] = {"nvd_rt_helper"};
  const TranslationArtifactPolicyV1 Policy{RequiredBlocks, Allowed};
  expectViolation(
      neverd::translate::verifyTranslationArtifact(Bytes, HostTriple, Policy),
      TranslationArtifactViolation::RelocationTypeNotAllowed);
}

TEST(TranslationArtifactVerifier,
     AcceptsDirectCOFFRuntimeHelperCallInV1Policy) {
  const std::vector<uint8_t> Bytes = yamlArtifact(R"(
--- !COFF
header:
  Machine:         IMAGE_FILE_MACHINE_AMD64
  Characteristics: [ ]
sections:
  - Name:            .text
    Characteristics: [ IMAGE_SCN_CNT_CODE, IMAGE_SCN_MEM_EXECUTE, IMAGE_SCN_MEM_READ ]
    Alignment:       4
    SectionData:     "E800000000C3"
    Relocations:
      - VirtualAddress: 1
        SymbolName:     nvd_rt_helper
        Type:           IMAGE_REL_AMD64_REL32
symbols:
  - Name:            translated_block
    Value:           0
    SectionNumber:   1
    SimpleType:      IMAGE_SYM_TYPE_NULL
    ComplexType:     IMAGE_SYM_DTYPE_FUNCTION
    StorageClass:    IMAGE_SYM_CLASS_STATIC
  - Name:            nvd_rt_helper
    Value:           0
    SectionNumber:   0
    SimpleType:      IMAGE_SYM_TYPE_NULL
    ComplexType:     IMAGE_SYM_DTYPE_FUNCTION
    StorageClass:    IMAGE_SYM_CLASS_EXTERNAL
)");
  const llvm::StringRef RequiredBlocks[] = {"translated_block"};
  const llvm::StringRef Allowed[] = {"nvd_rt_helper"};
  const TranslationArtifactPolicyV1 Policy{RequiredBlocks, Allowed};
  EXPECT_FALSE(static_cast<bool>(neverd::translate::verifyTranslationArtifact(
      Bytes, llvm::Triple("x86_64-pc-windows-msvc"), Policy)));
}

TEST(TranslationArtifactVerifier,
     RejectsAbsoluteCOFFRuntimeHelperRelocationInV1Policy) {
  const std::vector<uint8_t> Bytes = yamlArtifact(R"(
--- !COFF
header:
  Machine:         IMAGE_FILE_MACHINE_AMD64
  Characteristics: [ ]
sections:
  - Name:            .text
    Characteristics: [ IMAGE_SCN_CNT_CODE, IMAGE_SCN_MEM_EXECUTE, IMAGE_SCN_MEM_READ ]
    Alignment:       4
    SectionData:     "0000000000000000"
    Relocations:
      - VirtualAddress: 0
        SymbolName:     nvd_rt_helper
        Type:           IMAGE_REL_AMD64_ADDR64
symbols:
  - Name:            translated_block
    Value:           0
    SectionNumber:   1
    SimpleType:      IMAGE_SYM_TYPE_NULL
    ComplexType:     IMAGE_SYM_DTYPE_FUNCTION
    StorageClass:    IMAGE_SYM_CLASS_STATIC
  - Name:            nvd_rt_helper
    Value:           0
    SectionNumber:   0
    SimpleType:      IMAGE_SYM_TYPE_NULL
    ComplexType:     IMAGE_SYM_DTYPE_FUNCTION
    StorageClass:    IMAGE_SYM_CLASS_EXTERNAL
)");
  const llvm::StringRef RequiredBlocks[] = {"translated_block"};
  const llvm::StringRef Allowed[] = {"nvd_rt_helper"};
  const TranslationArtifactPolicyV1 Policy{RequiredBlocks, Allowed};
  expectViolation(neverd::translate::verifyTranslationArtifact(
                      Bytes, llvm::Triple("x86_64-pc-windows-msvc"), Policy),
                  TranslationArtifactViolation::RelocationTypeNotAllowed);
}

TEST(TranslationArtifactVerifier,
     RejectsCOFFPCRelativeRuntimeHelperMaterializationInV1Policy) {
  const std::vector<uint8_t> Bytes = yamlArtifact(R"(
--- !COFF
header:
  Machine:         IMAGE_FILE_MACHINE_AMD64
  Characteristics: [ ]
sections:
  - Name:            .text
    Characteristics: [ IMAGE_SCN_CNT_CODE, IMAGE_SCN_MEM_EXECUTE, IMAGE_SCN_MEM_READ ]
    Alignment:       4
    SectionData:     "0000000000000000"
    Relocations:
      - VirtualAddress: 0
        SymbolName:     nvd_rt_helper
        Type:           IMAGE_REL_AMD64_REL32
symbols:
  - Name:            translated_block
    Value:           0
    SectionNumber:   1
    SimpleType:      IMAGE_SYM_TYPE_NULL
    ComplexType:     IMAGE_SYM_DTYPE_FUNCTION
    StorageClass:    IMAGE_SYM_CLASS_STATIC
  - Name:            nvd_rt_helper
    Value:           0
    SectionNumber:   0
    SimpleType:      IMAGE_SYM_TYPE_NULL
    ComplexType:     IMAGE_SYM_DTYPE_FUNCTION
    StorageClass:    IMAGE_SYM_CLASS_EXTERNAL
)");
  const llvm::StringRef RequiredBlocks[] = {"translated_block"};
  const llvm::StringRef Allowed[] = {"nvd_rt_helper"};
  const TranslationArtifactPolicyV1 Policy{RequiredBlocks, Allowed};
  expectViolation(neverd::translate::verifyTranslationArtifact(
                      Bytes, llvm::Triple("x86_64-pc-windows-msvc"), Policy),
                  TranslationArtifactViolation::RelocationTypeNotAllowed);
}

TEST(TranslationArtifactVerifier,
     AcceptsDirectMachORuntimeHelperCallInV1Policy) {
  const std::vector<uint8_t> Bytes = yamlArtifact(R"(
--- !mach-o
FileHeader:
  magic:      0xFEEDFACF
  cputype:    0x01000007
  cpusubtype: 0x00000003
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
    vmsize:   6
    fileoff:  208
    filesize: 6
    maxprot:  5
    initprot: 5
    nsects:   1
    flags:    0
    Sections:
      - sectname:  __text
        segname:   __TEXT
        addr:      0
        size:      6
        offset:    208
        align:     2
        reloff:    214
        nreloc:    1
        flags:     0x80000400
        reserved1: 0
        reserved2: 0
        reserved3: 0
        content:   E800000000C3
        relocations:
          - address:   1
            symbolnum: 1
            pcrel:     true
            length:    2
            extern:    true
            type:      2
            scattered: false
            value:     0
  - cmd:      LC_SYMTAB
    cmdsize:  24
    symoff:   222
    nsyms:    2
    stroff:   254
    strsize:  34
LinkEditData:
  NameList:
    - n_strx:  1
      n_type:  0x0E
      n_sect:  1
      n_desc:  0
      n_value: 0
    - n_strx:  19
      n_type:  0x01
      n_sect:  0
      n_desc:  0
      n_value: 0
  StringTable:
    - ''
    - _translated_block
    - _nvd_rt_helper
)");
  const llvm::StringRef RequiredBlocks[] = {"_translated_block"};
  const llvm::StringRef Allowed[] = {"_nvd_rt_helper"};
  const TranslationArtifactPolicyV1 Policy{RequiredBlocks, Allowed};
  EXPECT_FALSE(static_cast<bool>(neverd::translate::verifyTranslationArtifact(
      Bytes, llvm::Triple("x86_64-apple-macosx"), Policy)));
}

TEST(TranslationArtifactVerifier,
     RejectsAbsoluteMachORuntimeHelperRelocationInV1Policy) {
  const std::vector<uint8_t> Bytes = yamlArtifact(R"(
--- !mach-o
FileHeader:
  magic:      0xFEEDFACF
  cputype:    0x01000007
  cpusubtype: 0x00000003
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
        content:   0000000000000000
        relocations:
          - address:   0
            symbolnum: 1
            pcrel:     false
            length:    3
            extern:    true
            type:      0
            scattered: false
            value:     0
  - cmd:      LC_SYMTAB
    cmdsize:  24
    symoff:   224
    nsyms:    2
    stroff:   256
    strsize:  34
LinkEditData:
  NameList:
    - n_strx:  1
      n_type:  0x0E
      n_sect:  1
      n_desc:  0
      n_value: 0
    - n_strx:  19
      n_type:  0x01
      n_sect:  0
      n_desc:  0
      n_value: 0
  StringTable:
    - ''
    - _translated_block
    - _nvd_rt_helper
)");
  const llvm::StringRef RequiredBlocks[] = {"_translated_block"};
  const llvm::StringRef Allowed[] = {"_nvd_rt_helper"};
  const TranslationArtifactPolicyV1 Policy{RequiredBlocks, Allowed};
  expectViolation(neverd::translate::verifyTranslationArtifact(
                      Bytes, llvm::Triple("x86_64-apple-macosx"), Policy),
                  TranslationArtifactViolation::RelocationTypeNotAllowed);
}

TEST(TranslationArtifactVerifier,
     RejectsMachOPCRelativeRuntimeHelperMaterializationInV1Policy) {
  const std::vector<uint8_t> Bytes = yamlArtifact(R"(
--- !mach-o
FileHeader:
  magic:      0xFEEDFACF
  cputype:    0x01000007
  cpusubtype: 0x00000003
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
        content:   0000000000000000
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
    strsize:  34
LinkEditData:
  NameList:
    - n_strx:  1
      n_type:  0x0E
      n_sect:  1
      n_desc:  0
      n_value: 0
    - n_strx:  19
      n_type:  0x01
      n_sect:  0
      n_desc:  0
      n_value: 0
  StringTable:
    - ''
    - _translated_block
    - _nvd_rt_helper
)");
  const llvm::StringRef RequiredBlocks[] = {"_translated_block"};
  const llvm::StringRef Allowed[] = {"_nvd_rt_helper"};
  const TranslationArtifactPolicyV1 Policy{RequiredBlocks, Allowed};
  expectViolation(neverd::translate::verifyTranslationArtifact(
                      Bytes, llvm::Triple("x86_64-apple-macosx"), Policy),
                  TranslationArtifactViolation::RelocationTypeNotAllowed);
}

TEST(TranslationArtifactVerifier, RejectsUnallowlistedUndefinedSymbol) {
  const std::vector<uint8_t> Bytes = yamlArtifact(R"(
--- !ELF
FileHeader:
  Class:   ELFCLASS64
  Data:    ELFDATA2LSB
  Type:    ET_REL
  Machine: EM_X86_64
Symbols:
  - Name:    nvd_rt_helper_suffix
    Type:    STT_FUNC
    Binding: STB_GLOBAL
)");
  const llvm::StringRef Allowed[] = {"nvd_rt_helper"};
  expectViolation(
      neverd::translate::verifyTranslationArtifact(Bytes, HostTriple, Allowed),
      TranslationArtifactViolation::ExternalSymbolNotAllowed);
}

TEST(TranslationArtifactVerifier,
     RejectsDynamicSymbolEvenWhenRuntimeAllowlisted) {
  const std::vector<uint8_t> Bytes = yamlArtifact(R"(
--- !ELF
FileHeader:
  Class:   ELFCLASS64
  Data:    ELFDATA2LSB
  Type:    ET_REL
  Machine: EM_X86_64
Sections:
  - Name: .text
    Type: SHT_PROGBITS
  - Name: .dynsym
    Type: SHT_DYNSYM
Symbols:
  - Name:    local_block
    Type:    STT_FUNC
    Section: .text
DynamicSymbols:
  - Name:    ambient_dynamic_symbol
    Type:    STT_FUNC
    Binding: STB_GLOBAL
)");
  const llvm::StringRef Allowed[] = {"ambient_dynamic_symbol"};
  expectViolation(
      neverd::translate::verifyTranslationArtifact(Bytes, HostTriple, Allowed),
      TranslationArtifactViolation::DynamicSymbolNotAllowed);
}

TEST(TranslationArtifactVerifier, RejectsExecutableWritableSection) {
  const std::vector<uint8_t> Bytes = yamlArtifact(R"(
--- !ELF
FileHeader:
  Class:   ELFCLASS64
  Data:    ELFDATA2LSB
  Type:    ET_REL
  Machine: EM_X86_64
Sections:
  - Name:  .text
    Type:  SHT_PROGBITS
    Flags: [ SHF_ALLOC, SHF_EXECINSTR, SHF_WRITE ]
)");
  expectViolation(
      neverd::translate::verifyTranslationArtifact(Bytes, HostTriple),
      TranslationArtifactViolation::ExecutableWritableSection);
}

TEST(TranslationArtifactVerifier, RejectsUnwindConstructorsAndTLS) {
  const std::vector<uint8_t> Unwind = yamlArtifact(R"(
--- !ELF
FileHeader:
  Class:   ELFCLASS64
  Data:    ELFDATA2LSB
  Type:    ET_REL
  Machine: EM_X86_64
Sections:
  - Name: .eh_frame
    Type: SHT_PROGBITS
)");
  expectViolation(
      neverd::translate::verifyTranslationArtifact(Unwind, HostTriple),
      TranslationArtifactViolation::ExceptionUnwindMetadata);

  const std::vector<uint8_t> Initializer = yamlArtifact(R"(
--- !ELF
FileHeader:
  Class:   ELFCLASS64
  Data:    ELFDATA2LSB
  Type:    ET_REL
  Machine: EM_X86_64
Sections:
  - Name: .init_array
    Type: SHT_INIT_ARRAY
)");
  expectViolation(
      neverd::translate::verifyTranslationArtifact(Initializer, HostTriple),
      TranslationArtifactViolation::StaticInitializer);

  const std::vector<uint8_t> TLS = yamlArtifact(R"(
--- !ELF
FileHeader:
  Class:   ELFCLASS64
  Data:    ELFDATA2LSB
  Type:    ET_REL
  Machine: EM_X86_64
Sections:
  - Name:  .tdata
    Type:  SHT_PROGBITS
    Flags: [ SHF_ALLOC, SHF_WRITE, SHF_TLS ]
)");
  expectViolation(neverd::translate::verifyTranslationArtifact(TLS, HostTriple),
                  TranslationArtifactViolation::ThreadLocalStorage);
}

TEST(TranslationArtifactVerifier, RejectsIFunc) {
  const std::vector<uint8_t> Bytes = yamlArtifact(R"(
--- !ELF
FileHeader:
  Class:   ELFCLASS64
  Data:    ELFDATA2LSB
  Type:    ET_REL
  Machine: EM_X86_64
Sections:
  - Name: .text
    Type: SHT_PROGBITS
Symbols:
  - Name:    indirect_resolver
    Type:    STT_GNU_IFUNC
    Binding: STB_GLOBAL
    Section: .text
)");
  expectViolation(
      neverd::translate::verifyTranslationArtifact(Bytes, HostTriple),
      TranslationArtifactViolation::IndirectSymbol);
}

TEST(TranslationArtifactVerifier, RejectsNonObjectRelocationTarget) {
  const std::vector<uint8_t> Bytes = yamlArtifact(R"(
--- !ELF
FileHeader:
  Class:   ELFCLASS64
  Data:    ELFDATA2LSB
  Type:    ET_REL
  Machine: EM_X86_64
Sections:
  - Name:    .text
    Type:    SHT_PROGBITS
    Content: "00000000"
  - Name: .rela.text
    Type: SHT_RELA
    Info: .text
    Link: .symtab
    Relocations:
      - Offset: 0
        Type:   R_X86_64_PC32
        Symbol: absolute_target
Symbols:
  - Name:    absolute_target
    Index:   SHN_ABS
    Binding: STB_GLOBAL
    Other:   [ STV_HIDDEN ]
)");
  expectViolation(
      neverd::translate::verifyTranslationArtifact(Bytes, HostTriple),
      TranslationArtifactViolation::RelocationTargetNotAllowed);
}

TEST(TranslationArtifactVerifier, RejectsUnknownRelocationType) {
  const std::vector<uint8_t> Bytes = yamlArtifact(R"(
--- !ELF
FileHeader:
  Class:   ELFCLASS64
  Data:    ELFDATA2LSB
  Type:    ET_REL
  Machine: EM_X86_64
Sections:
  - Name:    .text
    Type:    SHT_PROGBITS
    Flags:   [ SHF_ALLOC, SHF_EXECINSTR ]
    Content: "00000000"
  - Name: .rela.text
    Type: SHT_RELA
    Info: .text
    Link: .symtab
    Relocations:
      - Offset: 0
        Type:   0x7f
        Symbol: local_block
Symbols:
  - Name:    local_block
    Type:    STT_FUNC
    Section: .text
)");
  expectViolation(
      neverd::translate::verifyTranslationArtifact(Bytes, HostTriple),
      TranslationArtifactViolation::RelocationTypeNotAllowed);
}

TEST(TranslationArtifactVerifier, RejectsGOTAndPLTRelocations) {
  const std::vector<uint8_t> GOT = yamlArtifact(R"(
--- !ELF
FileHeader:
  Class:   ELFCLASS64
  Data:    ELFDATA2LSB
  Type:    ET_REL
  Machine: EM_X86_64
Sections:
  - Name:    .text
    Type:    SHT_PROGBITS
    Flags:   [ SHF_ALLOC, SHF_EXECINSTR ]
    Content: "00000000"
  - Name:    .data
    Type:    SHT_PROGBITS
    Flags:   [ SHF_ALLOC, SHF_WRITE ]
    Content: "00000000"
  - Name: .rela.text
    Type: SHT_RELA
    Info: .text
    Link: .symtab
    Relocations:
      - Offset: 0
        Type:   R_X86_64_GOTPCREL
        Symbol: local_data
Symbols:
  - Name:    local_data
    Type:    STT_OBJECT
    Section: .data
)");
  expectViolation(neverd::translate::verifyTranslationArtifact(GOT, HostTriple),
                  TranslationArtifactViolation::RelocationTypeNotAllowed);

  const std::vector<uint8_t> PLT = yamlArtifact(R"(
--- !ELF
FileHeader:
  Class:   ELFCLASS64
  Data:    ELFDATA2LSB
  Type:    ET_REL
  Machine: EM_X86_64
Sections:
  - Name:    .text
    Type:    SHT_PROGBITS
    Flags:   [ SHF_ALLOC, SHF_EXECINSTR ]
    Content: "00000000"
  - Name: .rela.text
    Type: SHT_RELA
    Info: .text
    Link: .symtab
    Relocations:
      - Offset: 0
        Type:   R_X86_64_PLT32
        Symbol: nvd_rt_helper
Symbols:
  - Name:    nvd_rt_helper
    Type:    STT_FUNC
    Binding: STB_GLOBAL
)");
  const llvm::StringRef Allowed[] = {"nvd_rt_helper"};
  expectViolation(
      neverd::translate::verifyTranslationArtifact(PLT, HostTriple, Allowed),
      TranslationArtifactViolation::RelocationTypeNotAllowed);
}

TEST(TranslationArtifactVerifier, RejectsIndirectSections) {
  const std::vector<uint8_t> ELF = yamlArtifact(R"(
--- !ELF
FileHeader:
  Class:   ELFCLASS64
  Data:    ELFDATA2LSB
  Type:    ET_REL
  Machine: EM_X86_64
Sections:
  - Name:  .got
    Type:  SHT_PROGBITS
    Flags: [ SHF_ALLOC, SHF_WRITE ]
)");
  expectViolation(neverd::translate::verifyTranslationArtifact(ELF, HostTriple),
                  TranslationArtifactViolation::IndirectSymbol);

  const std::vector<uint8_t> MachO = yamlArtifact(R"(
--- !mach-o
FileHeader:
  magic:      0xFEEDFACF
  cputype:    0x01000007
  cpusubtype: 0x00000003
  filetype:   0x00000001
  ncmds:      1
  sizeofcmds: 232
  flags:      0x00002000
  reserved:   0x00000000
LoadCommands:
  - cmd:      LC_SEGMENT_64
    cmdsize:  232
    segname:  ''
    vmaddr:   0
    vmsize:   8
    fileoff:  392
    filesize: 8
    maxprot:  3
    initprot: 3
    nsects:   1
    flags:    0
    Sections:
      - sectname:  __got
        segname:   __DATA
        addr:      0
        size:      8
        offset:    392
        align:     3
        reloff:    0
        nreloc:    0
        flags:     6
        reserved1: 0
        reserved2: 0
        reserved3: 0
        content:   0000000000000000
)");
  expectViolation(neverd::translate::verifyTranslationArtifact(
                      MachO, llvm::Triple("x86_64-apple-macosx")),
                  TranslationArtifactViolation::IndirectSymbol);
}

TEST(TranslationArtifactVerifier, RejectsPreemptibleDefinitions) {
  const std::vector<uint8_t> Global = yamlArtifact(R"(
--- !ELF
FileHeader:
  Class:   ELFCLASS64
  Data:    ELFDATA2LSB
  Type:    ET_REL
  Machine: EM_X86_64
Sections:
  - Name:  .text
    Type:  SHT_PROGBITS
    Flags: [ SHF_ALLOC, SHF_EXECINSTR ]
Symbols:
  - Name:    exported_block
    Type:    STT_FUNC
    Binding: STB_GLOBAL
    Section: .text
)");
  expectViolation(
      neverd::translate::verifyTranslationArtifact(Global, HostTriple),
      TranslationArtifactViolation::PreemptibleDefinition);

  const std::vector<uint8_t> Weak = yamlArtifact(R"(
--- !ELF
FileHeader:
  Class:   ELFCLASS64
  Data:    ELFDATA2LSB
  Type:    ET_REL
  Machine: EM_X86_64
Sections:
  - Name:  .text
    Type:  SHT_PROGBITS
    Flags: [ SHF_ALLOC, SHF_EXECINSTR ]
Symbols:
  - Name:    weak_block
    Type:    STT_FUNC
    Binding: STB_WEAK
    Other:   [ STV_HIDDEN ]
    Section: .text
)");
  expectViolation(
      neverd::translate::verifyTranslationArtifact(Weak, HostTriple),
      TranslationArtifactViolation::PreemptibleDefinition);
}

TEST(TranslationArtifactVerifier, AcceptsHiddenObjectDefinition) {
  const std::vector<uint8_t> Bytes = yamlArtifact(R"(
--- !ELF
FileHeader:
  Class:   ELFCLASS64
  Data:    ELFDATA2LSB
  Type:    ET_REL
  Machine: EM_X86_64
Sections:
  - Name:  .text
    Type:  SHT_PROGBITS
    Flags: [ SHF_ALLOC, SHF_EXECINSTR ]
Symbols:
  - Name:    hidden_block
    Type:    STT_FUNC
    Binding: STB_GLOBAL
    Other:   [ STV_HIDDEN ]
    Section: .text
)");
  EXPECT_FALSE(static_cast<bool>(
      neverd::translate::verifyTranslationArtifact(Bytes, HostTriple)));
}

TEST(TranslationArtifactVerifier, RejectsUnknownAllocatedSectionType) {
  const std::vector<uint8_t> Bytes = yamlArtifact(R"(
--- !ELF
FileHeader:
  Class:   ELFCLASS64
  Data:    ELFDATA2LSB
  Type:    ET_REL
  Machine: EM_X86_64
Sections:
  - Name:  .note.translation
    Type:  SHT_NOTE
    Flags: [ SHF_ALLOC ]
)");
  expectViolation(
      neverd::translate::verifyTranslationArtifact(Bytes, HostTriple),
      TranslationArtifactViolation::UnsupportedSection);

  const std::vector<uint8_t> LinkerDirective = yamlArtifact(R"(
--- !COFF
header:
  Machine:         IMAGE_FILE_MACHINE_AMD64
  Characteristics: [ ]
sections:
  - Name:            .drectve
    Characteristics: [ IMAGE_SCN_LNK_INFO, IMAGE_SCN_LNK_REMOVE ]
    Alignment:       1
    SectionData:     ""
symbols: []
)");
  expectViolation(neverd::translate::verifyTranslationArtifact(
                      LinkerDirective, llvm::Triple("x86_64-pc-windows-msvc")),
                  TranslationArtifactViolation::UnsupportedSection);
}

TEST(TranslationArtifactVerifier, RejectsNonLoadableRelocationTarget) {
  const std::vector<uint8_t> Bytes = yamlArtifact(R"(
--- !ELF
FileHeader:
  Class:   ELFCLASS64
  Data:    ELFDATA2LSB
  Type:    ET_REL
  Machine: EM_X86_64
Sections:
  - Name:    .text
    Type:    SHT_PROGBITS
    Flags:   [ SHF_ALLOC, SHF_EXECINSTR ]
    Content: "00000000"
  - Name:    .debug_payload
    Type:    SHT_PROGBITS
    Content: "00000000"
  - Name: .rela.text
    Type: SHT_RELA
    Info: .text
    Link: .symtab
    Relocations:
      - Offset: 0
        Type:   R_X86_64_PC32
        Symbol: debug_target
Symbols:
  - Name:    debug_target
    Type:    STT_OBJECT
    Section: .debug_payload
)");
  expectViolation(
      neverd::translate::verifyTranslationArtifact(Bytes, HostTriple),
      TranslationArtifactViolation::RelocationTargetNotAllowed);
}

TEST(TranslationArtifactVerifier, RejectsNonCanonicalAndMisalignedEncoding) {
  const std::vector<uint8_t> ImplicitAddend = yamlArtifact(R"(
--- !ELF
FileHeader:
  Class:   ELFCLASS64
  Data:    ELFDATA2LSB
  Type:    ET_REL
  Machine: EM_X86_64
Sections:
  - Name:    .text
    Type:    SHT_PROGBITS
    Flags:   [ SHF_ALLOC, SHF_EXECINSTR ]
    Content: "00000000"
  - Name: .rel.text
    Type: SHT_REL
    Info: .text
    Link: .symtab
    Relocations:
      - Offset: 0
        Type:   R_X86_64_PC32
        Symbol: local_block
Symbols:
  - Name:    local_block
    Type:    STT_FUNC
    Section: .text
)");
  expectViolation(
      neverd::translate::verifyTranslationArtifact(ImplicitAddend, HostTriple),
      TranslationArtifactViolation::RelocationTypeNotAllowed);

  const std::vector<uint8_t> Misaligned = yamlArtifact(R"(
--- !ELF
FileHeader:
  Class:   ELFCLASS64
  Data:    ELFDATA2LSB
  Type:    ET_REL
  Machine: EM_AARCH64
Sections:
  - Name:    .text
    Type:    SHT_PROGBITS
    Flags:   [ SHF_ALLOC, SHF_EXECINSTR ]
    Content: "0000000000000000"
  - Name: .rela.text
    Type: SHT_RELA
    Info: .text
    Link: .symtab
    Relocations:
      - Offset: 2
        Type:   R_AARCH64_CALL26
        Symbol: local_block
Symbols:
  - Name:    local_block
    Type:    STT_FUNC
    Section: .text
)");
  expectViolation(neverd::translate::verifyTranslationArtifact(
                      Misaligned, llvm::Triple("aarch64-unknown-linux-gnu")),
                  TranslationArtifactViolation::MalformedObject);
}

TEST(TranslationArtifactVerifier,
     RejectsDuplicateAArch64ELFFixupRangesBeforeTargetAdmission) {
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
        Symbol: nvd_rt_helper
      - Offset: 0
        Type:   R_AARCH64_CALL26
        Symbol: translated_block
Symbols:
  - Name:    translated_block
    Type:    STT_FUNC
    Section: .text
    Binding: STB_GLOBAL
    Other:   [ STV_HIDDEN ]
    Size:    4
  - Name:    nvd_rt_helper
    Type:    STT_FUNC
    Binding: STB_GLOBAL
)");
  const llvm::StringRef RequiredBlocks[] = {"translated_block"};
  const llvm::StringRef Allowed[] = {"nvd_rt_helper"};
  const TranslationArtifactPolicyV1 Policy{RequiredBlocks, Allowed};
  expectViolation(neverd::translate::verifyTranslationArtifact(
                      Bytes, llvm::Triple("aarch64-unknown-linux-gnu"), Policy),
                  TranslationArtifactViolation::MalformedObject);
}

TEST(TranslationArtifactVerifier,
     RejectsNonzeroEncodedAArch64ELFBranch26Immediate) {
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
        Symbol: nvd_rt_helper
        Addend: 0
Symbols:
  - Name:    translated_block
    Type:    STT_FUNC
    Section: .text
    Binding: STB_GLOBAL
    Other:   [ STV_HIDDEN ]
    Size:    4
  - Name:    nvd_rt_helper
    Type:    STT_FUNC
    Binding: STB_GLOBAL
)");
  const llvm::StringRef RequiredBlocks[] = {"translated_block"};
  const llvm::StringRef Allowed[] = {"nvd_rt_helper"};
  const TranslationArtifactPolicyV1 Policy{RequiredBlocks, Allowed};
  expectViolation(neverd::translate::verifyTranslationArtifact(
                      Bytes, llvm::Triple("aarch64-unknown-linux-gnu"), Policy),
                  TranslationArtifactViolation::RelocationTypeNotAllowed);
}

TEST(TranslationArtifactVerifier, RejectsPartiallyOverlappingRelocationFields) {
  const std::vector<uint8_t> Bytes = yamlArtifact(R"(
--- !ELF
FileHeader:
  Class:   ELFCLASS64
  Data:    ELFDATA2LSB
  Type:    ET_REL
  Machine: EM_X86_64
Sections:
  - Name:    .text
    Type:    SHT_PROGBITS
    Flags:   [ SHF_ALLOC, SHF_EXECINSTR ]
    Content: "0000000000000000"
  - Name: .rela.text
    Type: SHT_RELA
    Info: .text
    Link: .symtab
    Relocations:
      - Offset: 0
        Type:   R_X86_64_64
        Symbol: local_block
      - Offset: 4
        Type:   R_X86_64_PC32
        Symbol: local_block
Symbols:
  - Name:    local_block
    Type:    STT_FUNC
    Section: .text
    Size:    8
)");
  expectViolation(
      neverd::translate::verifyTranslationArtifact(Bytes, HostTriple),
      TranslationArtifactViolation::MalformedObject);
}

TEST(TranslationArtifactVerifier,
     RejectsAArch64ELFBranch26ToDefinedBlockInV1Policy) {
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
        Symbol: target_block
Symbols:
  - Name:    translated_block
    Type:    STT_FUNC
    Section: .text
    Binding: STB_GLOBAL
    Other:   [ STV_HIDDEN ]
    Size:    4
  - Name:    target_block
    Type:    STT_FUNC
    Section: .text
    Binding: STB_GLOBAL
    Other:   [ STV_HIDDEN ]
    Value:   4
    Size:    4
)");
  const llvm::StringRef RequiredBlocks[] = {"translated_block", "target_block"};
  const TranslationArtifactPolicyV1 Policy{RequiredBlocks, {}};
  expectViolation(neverd::translate::verifyTranslationArtifact(
                      Bytes, llvm::Triple("aarch64-unknown-linux-gnu"), Policy),
                  TranslationArtifactViolation::RelocationTargetNotAllowed);
}

TEST(TranslationArtifactVerifier,
     RejectsDuplicateAArch64MachOFixupRangesBeforeTargetAdmission) {
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
    strsize:  34
LinkEditData:
  NameList:
    - n_strx:  1
      n_type:  0x1F
      n_sect:  1
      n_desc:  0
      n_value: 0
    - n_strx:  19
      n_type:  0x01
      n_sect:  0
      n_desc:  0
      n_value: 0
  StringTable:
    - ''
    - _translated_block
    - _nvd_rt_helper
)");
  const llvm::StringRef RequiredBlocks[] = {"_translated_block"};
  const llvm::StringRef Allowed[] = {"_nvd_rt_helper"};
  const TranslationArtifactPolicyV1 Policy{RequiredBlocks, Allowed};
  expectViolation(neverd::translate::verifyTranslationArtifact(
                      Bytes, llvm::Triple("aarch64-apple-macosx"), Policy),
                  TranslationArtifactViolation::MalformedObject);
}

TEST(TranslationArtifactVerifier,
     RejectsAArch64MachOBranch26ToDefinedBlockInV1Policy) {
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
        content:   01000014C0035FD6
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
    strsize:  33
LinkEditData:
  NameList:
    - n_strx:  1
      n_type:  0x1F
      n_sect:  1
      n_desc:  0
      n_value: 0
    - n_strx:  19
      n_type:  0x1F
      n_sect:  1
      n_desc:  0
      n_value: 4
  StringTable:
    - ''
    - _translated_block
    - _target_block
)");
  const llvm::StringRef RequiredBlocks[] = {"_translated_block",
                                            "_target_block"};
  const TranslationArtifactPolicyV1 Policy{RequiredBlocks, {}};
  expectViolation(neverd::translate::verifyTranslationArtifact(
                      Bytes, llvm::Triple("aarch64-apple-macosx"), Policy),
                  TranslationArtifactViolation::RelocationTargetNotAllowed);
}

TEST(TranslationArtifactVerifier,
     RejectsMisalignedAArch64V1TextAndBlockRanges) {
  const std::vector<uint8_t> ELFSection = yamlArtifact(R"(
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
  - Name:    translated_block
    Type:    STT_FUNC
    Section: .text
    Binding: STB_GLOBAL
    Other:   [ STV_HIDDEN ]
    Size:    4
)");
  const llvm::StringRef ELFRequired[] = {"translated_block"};
  const TranslationArtifactPolicyV1 ELFPolicy{ELFRequired, {}};
  expectViolation(
      neverd::translate::verifyTranslationArtifact(
          ELFSection, llvm::Triple("aarch64-unknown-linux-gnu"), ELFPolicy),
      TranslationArtifactViolation::InvalidBlockDefinition);

  const std::vector<uint8_t> ELFBlock = yamlArtifact(R"(
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
    Content:      "0000C0035FD60000"
Symbols:
  - Name:    translated_block
    Type:    STT_FUNC
    Section: .text
    Binding: STB_GLOBAL
    Other:   [ STV_HIDDEN ]
    Value:   2
    Size:    4
)");
  expectViolation(
      neverd::translate::verifyTranslationArtifact(
          ELFBlock, llvm::Triple("aarch64-unknown-linux-gnu"), ELFPolicy),
      TranslationArtifactViolation::InvalidBlockDefinition);

  const std::vector<uint8_t> MachO = yamlArtifact(R"(
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
    strsize:  19
LinkEditData:
  NameList:
    - n_strx:  1
      n_type:  0x1F
      n_sect:  1
      n_desc:  0
      n_value: 0
  StringTable:
    - ''
    - _translated_block
)");
  const llvm::StringRef MachORequired[] = {"_translated_block"};
  const TranslationArtifactPolicyV1 MachOPolicy{MachORequired, {}};
  expectViolation(neverd::translate::verifyTranslationArtifact(
                      MachO, llvm::Triple("aarch64-apple-macosx"), MachOPolicy),
                  TranslationArtifactViolation::InvalidBlockDefinition);
}

TEST(TranslationArtifactVerifier, RejectsRelocationFieldOutsideDestination) {
  const std::vector<uint8_t> ELF = yamlArtifact(R"(
--- !ELF
FileHeader:
  Class:   ELFCLASS64
  Data:    ELFDATA2LSB
  Type:    ET_REL
  Machine: EM_X86_64
Sections:
  - Name:    .text
    Type:    SHT_PROGBITS
    Flags:   [ SHF_ALLOC, SHF_EXECINSTR ]
    Content: "00000000"
  - Name: .rela.text
    Type: SHT_RELA
    Info: .text
    Link: .symtab
    Relocations:
      - Offset: 2
        Type:   R_X86_64_PC32
        Symbol: local_block
Symbols:
  - Name:    local_block
    Type:    STT_FUNC
    Section: .text
)");
  expectViolation(neverd::translate::verifyTranslationArtifact(ELF, HostTriple),
                  TranslationArtifactViolation::MalformedObject);

  const std::vector<uint8_t> COFF = yamlArtifact(R"(
--- !COFF
header:
  Machine:         IMAGE_FILE_MACHINE_AMD64
  Characteristics: [ ]
sections:
  - Name:            .data
    Characteristics: [ IMAGE_SCN_CNT_INITIALIZED_DATA, IMAGE_SCN_MEM_READ, IMAGE_SCN_MEM_WRITE ]
    Alignment:       4
    SectionData:     "00000000"
    Relocations:
      - VirtualAddress: 0
        SymbolName:     local_data
        Type:           IMAGE_REL_AMD64_ADDR64
symbols:
  - Name:            local_data
    Value:           0
    SectionNumber:   1
    SimpleType:      IMAGE_SYM_TYPE_NULL
    ComplexType:     IMAGE_SYM_DTYPE_NULL
    StorageClass:    IMAGE_SYM_CLASS_STATIC
)");
  expectViolation(neverd::translate::verifyTranslationArtifact(
                      COFF, llvm::Triple("x86_64-pc-windows-msvc")),
                  TranslationArtifactViolation::MalformedObject);

  const std::vector<uint8_t> MachO = yamlArtifact(R"(
--- !mach-o
FileHeader:
  magic:      0xFEEDFACF
  cputype:    0x01000007
  cpusubtype: 0x00000003
  filetype:   0x00000001
  ncmds:      1
  sizeofcmds: 232
  flags:      0x00002000
  reserved:   0x00000000
LoadCommands:
  - cmd:      LC_SEGMENT_64
    cmdsize:  232
    segname:  ''
    vmaddr:   0
    vmsize:   4
    fileoff:  392
    filesize: 4
    maxprot:  3
    initprot: 3
    nsects:   1
    flags:    0
    Sections:
      - sectname:  __data
        segname:   __DATA
        addr:      0
        size:      4
        offset:    392
        align:     2
        reloff:    396
        nreloc:    1
        flags:     0
        reserved1: 0
        reserved2: 0
        reserved3: 0
        content:   00000000
        relocations:
          - address:   0
            symbolnum: 1
            pcrel:     false
            length:    3
            extern:    false
            type:      0
            scattered: false
            value:     0
)");
  expectViolation(neverd::translate::verifyTranslationArtifact(
                      MachO, llvm::Triple("x86_64-apple-macosx")),
                  TranslationArtifactViolation::MalformedObject);
}

TEST(TranslationArtifactVerifier, RejectsMachOGOTRelocationEncoding) {
  const std::vector<uint8_t> Bytes = yamlArtifact(R"(
--- !mach-o
FileHeader:
  magic:      0xFEEDFACF
  cputype:    0x01000007
  cpusubtype: 0x00000003
  filetype:   0x00000001
  ncmds:      1
  sizeofcmds: 232
  flags:      0x00002000
  reserved:   0x00000000
LoadCommands:
  - cmd:      LC_SEGMENT_64
    cmdsize:  232
    segname:  ''
    vmaddr:   0
    vmsize:   4
    fileoff:  392
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
        offset:    392
        align:     2
        reloff:    396
        nreloc:    1
        flags:     0x80000400
        reserved1: 0
        reserved2: 0
        reserved3: 0
        content:   00000000
        relocations:
          - address:   0
            symbolnum: 1
            pcrel:     true
            length:    2
            extern:    false
            type:      3
            scattered: false
            value:     0
)");
  expectViolation(neverd::translate::verifyTranslationArtifact(
                      Bytes, llvm::Triple("x86_64-apple-macosx")),
                  TranslationArtifactViolation::RelocationTypeNotAllowed);
}

TEST(TranslationArtifactVerifier, RejectsCOFFNoOpAndExternalSectionRelative) {
  const std::vector<uint8_t> NoOp = yamlArtifact(R"(
--- !COFF
header:
  Machine:         IMAGE_FILE_MACHINE_AMD64
  Characteristics: [ ]
sections:
  - Name:            .text
    Characteristics: [ IMAGE_SCN_CNT_CODE, IMAGE_SCN_MEM_EXECUTE, IMAGE_SCN_MEM_READ ]
    Alignment:       4
    SectionData:     "00000000"
    Relocations:
      - VirtualAddress: 0
        SymbolName:     local_block
        Type:           IMAGE_REL_AMD64_ABSOLUTE
symbols:
  - Name:            local_block
    Value:           0
    SectionNumber:   1
    SimpleType:      IMAGE_SYM_TYPE_NULL
    ComplexType:     IMAGE_SYM_DTYPE_NULL
    StorageClass:    IMAGE_SYM_CLASS_STATIC
)");
  expectViolation(neverd::translate::verifyTranslationArtifact(
                      NoOp, llvm::Triple("x86_64-pc-windows-msvc")),
                  TranslationArtifactViolation::RelocationTypeNotAllowed);

  const std::vector<uint8_t> SectionRelative = yamlArtifact(R"(
--- !COFF
header:
  Machine:         IMAGE_FILE_MACHINE_AMD64
  Characteristics: [ ]
sections:
  - Name:            .text
    Characteristics: [ IMAGE_SCN_CNT_CODE, IMAGE_SCN_MEM_EXECUTE, IMAGE_SCN_MEM_READ ]
    Alignment:       4
    SectionData:     "00000000"
    Relocations:
      - VirtualAddress: 0
        SymbolName:     nvd_rt_helper
        Type:           IMAGE_REL_AMD64_SECREL
symbols:
  - Name:            nvd_rt_helper
    Value:           0
    SectionNumber:   0
    SimpleType:      IMAGE_SYM_TYPE_NULL
    ComplexType:     IMAGE_SYM_DTYPE_NULL
    StorageClass:    IMAGE_SYM_CLASS_EXTERNAL
)");
  const llvm::StringRef Allowed[] = {"nvd_rt_helper"};
  expectViolation(
      neverd::translate::verifyTranslationArtifact(
          SectionRelative, llvm::Triple("x86_64-pc-windows-msvc"), Allowed),
      TranslationArtifactViolation::RelocationTargetNotAllowed);
}

} // namespace
