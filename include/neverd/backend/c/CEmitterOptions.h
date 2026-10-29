//===- CEmitterOptions.h - C emitter configuration -----------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Options controlling C source emission from decompiled IR.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_BACKEND_C_CEMITTEROPTIONS_H
#define NEVERD_BACKEND_C_CEMITTEROPTIONS_H

#include "neverd/Common.h"
#include "neverd/ir/SourceVariableEdits.h"
#include "neverd/libc/LibCVariant.h"

#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace neverd {

struct BinaryImage;
struct CSourceMap;
struct CSourceName;
struct SourceVariableTypes;
struct SourceCallTypeHint;
class DataAccessWidths;
struct NdType;

/// What a unit holds of the program's own definitions of the records it
/// names, from its local types.
struct CRecordDefinitions {
  /// C that defines the records and what they hold by value, which goes
  /// after the unit's includes.
  std::string Text;
  /// The records Text defines, by the names the unit spells them by, and
  /// whether each is a union.
  std::map<std::string, bool> Records;
  /// The records the unit has from the system's headers it includes, which
  /// lay each out as the program does, with its members' names; likewise.
  std::map<std::string, bool> HeaderRecords;
  /// Every record Text names or the unit asked for, defined or not, and
  /// whether each is a union, which the unit's own declarations of it say.
  std::map<std::string, bool> Tags;
  /// The ordinary identifiers Text declares, which the unit's own names
  /// step around.
  std::vector<std::string> Identifiers;
  /// The enumerations Text defines, by their tags' names.
  std::vector<std::string> Enumerations;
  /// The local types Text defines, and the records it only declares, by
  /// their names in the library, which another language's spelling of the
  /// unit defines as its own.
  std::vector<std::string> Types;
  std::vector<std::string> DeclaredTypes;
  /// Types whose matching target header declarations Text includes instead.
  std::vector<std::string> HeaderTypes;
  /// The records the unit's headers name by a typedef alone, as bionic's
  /// <pthread.h> does pthread_mutex_t: the unit spells each by that name
  /// and declares no typedef of its own, which would contradict the
  /// header's.
  std::vector<std::string> HeaderTypedefs;
};

struct CEmitterOptions {
  bool EmitIncludes = true;
  /// The unit is a whole program, which a C compiler takes as one: its main
  /// holds to the result and parameters C gives main.  A single function's
  /// view keeps the name its listing gives it, as LLVM-C's does.
  bool WholeProgram = false;
  bool EmitComments = true;
  /// Wrap record declarations in stable preprocessor guards so independently
  /// emitted translation units can be concatenated. A consumer that parses one
  /// complete unit and performs its own cross-unit deduplication can disable
  /// the guards without changing the declarations themselves.  The local
  /// types' definitions (RecordDefinitions) keep their guards regardless.
  bool EmitRecordGuards = true;
  bool UseDebugNames = true;
  /// Structured pseudocode may use C++ exception syntax. A plain C view
  /// keeps runtime calls and renders unwind-table edges and handler entries
  /// explicitly, as the Itanium C projection already does. Platform C
  /// extensions such as MSVC SEH remain C syntax.
  bool StructuredExceptionSyntax = true;
  /// LLVMC clients with a complete source contract can retain the module's
  /// function types and request prototypes for definitions as well as imports.
  /// Disables inferred-void and debug-signature projections in that route.
  /// A settled no-value contract still discards the machine return register;
  /// a retained non-void signature uses a zero-initialized carrier instead.
  bool PreserveLLVMFunctionTypes = false;
  /// Spell ordinary scalar byte accesses through pointers, as
  /// ScalarPointers says. Ordered and unusual-width accesses retain their
  /// existing exact semantics. This is the default; clear it for portable
  /// byte copies, which any C compiler accepts.
  bool UseUnalignedPointers = true;
  /// How UseUnalignedPointers spells an access.
  enum class ScalarPointerSpelling : uint8_t {
    /// The standard scalar types, `*(uint64_t *)p`, as decompiled code reads
    /// best. The C assumes a target that accesses scalars at any alignment
    /// (x86, AArch64) and -fno-strict-aliasing; 16-byte accesses, which
    /// compilers move with aligned vector instructions, stay byte copies.
    StandardTypes,
    /// Clang/GCC aligned(1), may_alias alias types, `*(_QWORD *)p`, which
    /// keep unaligned and overlapping storage exact under any GCC/Clang
    /// build without asserting an effective type. Generated sources that
    /// promise byte semantics to their compilers use these.
    AliasTypes,
  };
  ScalarPointerSpelling ScalarPointers = ScalarPointerSpelling::StandardTypes;
  Arch TheArch = Arch::X64;
  BinaryFormat Format = BinaryFormat::Unknown;
  /// When set, HighC can fold rdata integer loads, print printable
  /// rdata C/wchar literals, and name image-backed data objects
  /// instead of emitting raw virtual addresses.
  const BinaryImage *Image = nullptr;
  /// The C library whose declarations the program's calls follow: the
  /// image's (libc::libcVariantOf), else its format's.
  libc::LibCVariant libcVariant() const {
    return Image ? libc::libcVariantOf(*Image) : libc::libcVariantOf(Format);
  }
  /// The names the user gave addresses, which name image data ahead of debug
  /// information and symbols.
  const std::map<va_t, std::string> *UserNames = nullptr;
  /// The widths of the reads and writes the code of the whole image states
  /// at each address, which name unnamed image data as the listing names it
  /// (neverd/DataNames.def): `qword_` for data every such access reads or
  /// writes eight bytes at, `unk_` where they differ or none states one.
  /// Without it, data is named by the accesses of the functions emitted.
  const DataAccessWidths *AccessWidths = nullptr;
  /// Optional source-map sink. Recording never changes the emitted C text.
  CSourceMap *SourceMap = nullptr;
  /// Receives the names the text spells (CSourceMap::Names) without the
  /// marked render a source map needs.
  std::vector<CSourceName> *SourceNames = nullptr;
  /// A function definition the writer could not form C for.
  struct RefusedDefinition {
    va_t Entry = 0;
    std::string Name;
    std::string Reason;
  };
  /// Where a unit lists the definitions it leaves out rather than failing:
  /// one whose source requirements fail, then one whose C throws, each in the
  /// unit's order, is omitted with what it threw, and the others are written.
  /// Null fails the unit at the first, as one function's view does.  A
  /// rendering that records positions, a source map or source names still
  /// fails, as its text follows one pass, and so does one whose requirements
  /// fail under structured exception syntax, which writes funclets inside
  /// their parents, or in a function whose body fills another's `__finally`.
  std::vector<RefusedDefinition> *RefusedDefinitions = nullptr;
  /// Given the named records a unit's types reach, through pointers and
  /// arrays, the definitions the program's local types give them.  A record
  /// left out, as one the local types do not define or define otherwise
  /// than the debug information the unit reads its members by, stays
  /// declared only. A pointer to a named record requests only its tag and
  /// declaration, even if the library has its complete layout. A separate
  /// by-value request for the same record still requires that layout.
  std::function<CRecordDefinitions(
      const std::vector<std::shared_ptr<NdType>> &)>
      RecordDefinitions;
  /// Analyst declarations for source interpretation only. They supply no
  /// native ABI, memory extent, code ownership or rewriting authority.
  const SourceVariableTypes *VariableTypes = nullptr;
  const std::map<std::string, SourceCallTypeHint> *CallTypes = nullptr;
  struct FrameVariable {
    std::string Name;
    std::shared_ptr<NdType> Type;
  };
  const std::map<int64_t, FrameVariable> *FrameVariables = nullptr;
  const std::vector<SourceVariableEdit> *VariableEdits = nullptr;
  std::string VariableSnapshot;
};

} // namespace neverd

#endif // NEVERD_BACKEND_C_CEMITTEROPTIONS_H
