//===- LLVMCEmitter.cpp - LLVM IR to C emitter ---------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Top-level orchestration for the LLVM-IR C emitter: include generation,
/// struct/global/forward-declaration emission, and per-function dispatch.
/// Function and instruction rendering live in LLVMCFuncWriter.cpp and
/// LLVMCStmtWriter.cpp; value/expression rendering lives in
/// LLVMCExprWriter.cpp.
///
//===----------------------------------------------------------------------===//

#include "neverd/backend/c/LLVMC/LLVMCEmitter.h"

#include "../FloatConversion.h"
#include "../LibraryPrototype.h"
#include "../StdcallImportPops.h"
#include "../UnalignedMemory.h"
#include "../VariadicImportStub.h"
#include "../pass/LLVMC/LLVMCBitTests.h"
#include "../pass/LLVMC/LLVMCCommonBranches.h"
#include "../pass/LLVMC/LLVMCLoopPhases.h"
#include "../pass/LLVMC/LLVMCScalarLoopRecovery.h"
#include "../render/X86FPStateHelpers.h"
#include "LLVMCFrameLayout.h"
#include "LLVMCIntegerMinMax.h"
#include "LLVMCIntegerSaturation.h"
#include "LLVMCScalarUnary.h"
#include "LLVMCSourceStrings.h"
#include "LLVMCStackAllocation.h"
#include "LLVMCStorage.h"
#include "LLVMCWriter.h"

#include "neverd/Common.h"
#include "neverd/DataAccessWidths.h"
#include "neverd/backend/DataSymbolMetadata.h"
#include "neverd/backend/RewriteSourceIdentity.h"
#include "neverd/backend/c/FloatingPointContract.h"
#include "neverd/backend/c/render/CTypeFormat.h"
#include "neverd/backend/c/render/SyscallHelpers.h"
#include "neverd/backend/llvm/LLVMName.h"
#include "neverd/backend/llvm/LLVMSourceMap.h"
#include "neverd/backend/llvm/LLVMSourceTypeHints.h"
#include "neverd/backend/llvm/LLVMX86AddressSpaces.h"
#include "neverd/backend/llvm/LLVMX86FPStateAsm.h"
#include "neverd/backend/llvm/LLVMX86ShadowStackAsm.h"
#include "neverd/backend/llvm/PEImportShadow.h"
#include "neverd/libc/LibCResult.h"
#include "neverd/libc/MinGWHeaderFunctions.h"

#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/IR/Dominators.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/IR/IntrinsicsAArch64.h"
#include "llvm/IR/Verifier.h"
#include "llvm/Passes/PassBuilder.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/MD5.h"
#include "llvm/Support/WithColor.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/TargetParser/Triple.h"
#include "llvm/Transforms/Scalar/DCE.h"
#include "llvm/Transforms/Scalar/Scalarizer.h"
#include "llvm/Transforms/Utils/Cloning.h"
#include "llvm/Transforms/Utils/LoopUtils.h"
#include "llvm/Transforms/Utils/PromoteMemToReg.h"

#define DEBUG_TYPE "neverd-llvmc-emitter"

#include <cctype>
#include <stdexcept>

namespace neverd {

namespace {
/// Whether \p Global mirrors the image's pointer storage: one global for a
/// whole run of sections, every pointer in it relocated.  Its bytes are the
/// image's, which a single function's output declares rather than defines.
bool mirrorsImageStorage(const llvm::GlobalValue &Global) {
  return parseNdCodePtrSymbol(Global.getName()).has_value();
}

// A selected body also owns the constants it addresses, including globals
// synthesized by LLVM and providers named only by their initializers. Do not
// follow a referenced function into its body: that body is not being emitted,
// nor into the image's storage, which is declared.
std::set<const llvm::GlobalValue *>
referencedGlobals(const llvm::Function &Function) {
  std::set<const llvm::GlobalValue *> Globals;
  llvm::SmallVector<const llvm::Value *, 32> Work;
  llvm::SmallPtrSet<const llvm::Value *, 32> Seen;
  for (const auto &Block : Function)
    for (const auto &Instruction : Block)
      for (const auto &Operand : Instruction.operands())
        if (llvm::isa<llvm::Constant>(Operand))
          Work.push_back(Operand);
  while (!Work.empty()) {
    const auto *Value = Work.pop_back_val();
    if (!Seen.insert(Value).second)
      continue;
    if (const auto *Global = llvm::dyn_cast<llvm::GlobalValue>(Value)) {
      Globals.insert(Global);
      if (const auto *Object = llvm::dyn_cast<llvm::GlobalVariable>(Global);
          Object && Object->hasInitializer() && !mirrorsImageStorage(*Object))
        Work.push_back(Object->getInitializer());
      continue;
    }
    if (const auto *Constant = llvm::dyn_cast<llvm::Constant>(Value))
      for (const auto &Operand : Constant->operands())
        Work.push_back(Operand);
  }
  return Globals;
}
/// An initializer longer than this prints over several lines of about
/// InitializerLineChars each, so a mirrored image section reads, and pages,
/// as the code around it does.
constexpr size_t WrapInitializersOver = 4096;
constexpr size_t InitializerLineChars = 120;
constexpr llvm::StringLiteral InitializerIndent = "    ";

/// The length of the escape sequence at \p At in \p Text: an octal escape
/// takes three digits at most, a hex escape every digit after it.
size_t escapeLength(llvm::StringRef Text, size_t At) {
  size_t End = At + 1;
  if (End == Text.size())
    return 1;
  const char Kind = Text[End++];
  if (Kind >= '0' && Kind <= '7') {
    while (End < Text.size() && End < At + 4 && Text[End] >= '0' &&
           Text[End] <= '7')
      ++End;
  } else if (Kind == 'x') {
    while (End < Text.size() && llvm::isHexDigit(Text[End]))
      ++End;
  } else if (Kind == 'u' || Kind == 'U') {
    End = std::min(Text.size(), End + (Kind == 'u' ? 4 : 8));
  }
  return End - At;
}

/// \p Text, an initializer, broken after its commas and inside its string
/// literals when it is too long for one line.  No break falls in a comment,
/// a character constant or an escape sequence, and a string literal goes on
/// as an adjacent one, which C joins back.
std::string wrapInitializer(std::string Text) {
  if (Text.size() <= WrapInitializersOver)
    return Text;
  std::string Out;
  Out.reserve(Text.size() + Text.size() / InitializerLineChars *
                                (InitializerIndent.size() + 3));
  size_t LineStart = 0;
  const auto LineFull = [&] {
    return Out.size() - LineStart >= InitializerLineChars;
  };
  const auto Break = [&](llvm::StringRef Before, llvm::StringRef After) {
    Out += Before;
    Out += '\n';
    LineStart = Out.size();
    Out += InitializerIndent;
    Out += After;
  };
  const auto CopyEscape = [&](size_t &I) {
    const size_t Length = escapeLength(Text, I);
    Out.append(Text, I, Length);
    I += Length - 1;
  };
  enum class Within { Code, String, Character, Comment };
  Within State = Within::Code;
  for (size_t I = 0; I < Text.size(); ++I) {
    const char C = Text[I];
    const char Next = I + 1 < Text.size() ? Text[I + 1] : '\0';
    switch (State) {
    case Within::String:
      if (C == '"') {
        State = Within::Code;
        Out += C;
        break;
      }
      if (LineFull())
        Break("\"", "\"");
      if (C == '\\')
        CopyEscape(I);
      else
        Out += C;
      break;
    case Within::Character:
      if (C == '\\') {
        CopyEscape(I);
        break;
      }
      if (C == '\'')
        State = Within::Code;
      Out += C;
      break;
    case Within::Comment:
      if (C == '*' && Next == '/') {
        State = Within::Code;
        Out += "*/";
        ++I;
        break;
      }
      Out += C;
      break;
    case Within::Code:
      if (C == ',' && Next == ' ' && LineFull()) {
        Break(",", "");
        ++I;
        break;
      }
      if (C == '"')
        State = Within::String;
      else if (C == '\'')
        State = Within::Character;
      else if (C == '/' && Next == '*') {
        State = Within::Comment;
        Out += "/*";
        ++I;
        break;
      }
      Out += C;
      break;
    }
  }
  return Out;
}

} // namespace

bool LLVMCWriter::isNativeVectorIntrinsic(const llvm::CallBase &Call,
                                          Arch TheArch) {
  const auto *Callee = Call.getCalledFunction();
  if (TheArch != Arch::AArch64 || !Callee ||
      Callee->getIntrinsicID() != llvm::Intrinsic::aarch64_neon_bfmmla ||
      Call.arg_size() != 3)
    return false;
  auto &Context = Call.getContext();
  auto *Accumulator =
      llvm::FixedVectorType::get(llvm::Type::getFloatTy(Context), 4);
  auto *Input = llvm::FixedVectorType::get(llvm::Type::getBFloatTy(Context), 8);
  return Call.getType() == Accumulator &&
         Call.getArgOperand(0)->getType() == Accumulator &&
         Call.getArgOperand(1)->getType() == Input &&
         Call.getArgOperand(2)->getType() == Input &&
         Call.getFunctionType() == Callee->getFunctionType();
}

void LLVMCWriter::prepareFunctionIdentifiers(llvm::Module &Mod) {
  GlobalIdentifierAllocator = CProjectionIdentifierAllocator{};
  FunctionIdentifiers.clear();
  FunctionIdentifierNames.clear();
  FunctionSymbolNames.clear();
  FunctionSources.clear();
  ExternalDataIdentifiers.clear();
  const std::set<std::string> ImportCNames =
      Img ? peImportCNames(*Img) : std::set<std::string>{};
  for (llvm::Function &Fn : Mod) {
    llvm::StringRef Name = Fn.getName();
    std::string IntrinsicHelper;
    if (Fn.getIntrinsicID() == llvm::Intrinsic::fshl ||
        Fn.getIntrinsicID() == llvm::Intrinsic::fshr) {
      IntrinsicHelper = Fn.getIntrinsicID() == llvm::Intrinsic::fshl
                            ? "neverd_llvm_fshl"
                            : "neverd_llvm_fshr";
      if (const auto *Integer =
              llvm::dyn_cast<llvm::IntegerType>(Fn.getReturnType()))
        IntrinsicHelper += "_i" + std::to_string(Integer->getBitWidth());
      Name = IntrinsicHelper;
    }
    if (const char *Kind = integerMinMaxSpelling(Fn.getIntrinsicID())) {
      IntrinsicHelper = std::string("neverd_llvm_") + Kind;
      if (const auto *Integer =
              llvm::dyn_cast<llvm::IntegerType>(Fn.getReturnType()))
        IntrinsicHelper += "_i" + std::to_string(Integer->getBitWidth());
      Name = IntrinsicHelper;
    }
    if (const char *Kind = integerSaturationSpelling(Fn.getIntrinsicID())) {
      IntrinsicHelper = std::string("neverd_llvm_") + Kind;
      if (const auto *Integer =
              llvm::dyn_cast<llvm::IntegerType>(Fn.getReturnType()))
        IntrinsicHelper += "_i" + std::to_string(Integer->getBitWidth());
      Name = IntrinsicHelper;
    }
    if (const char *Kind = scalarUnarySpelling(Fn.getIntrinsicID())) {
      IntrinsicHelper = std::string("neverd_llvm_") + Kind;
      if (const auto *Integer =
              llvm::dyn_cast<llvm::IntegerType>(Fn.getReturnType()))
        IntrinsicHelper += "_i" + std::to_string(Integer->getBitWidth());
      if (Fn.arg_size() == 1 && Fn.getArg(0)->getType()->isFloatingPointTy())
        IntrinsicHelper +=
            "_f" +
            std::to_string(Fn.getArg(0)->getType()->getPrimitiveSizeInBits());
      Name = IntrinsicHelper;
    }
    std::string DebugName;
    if (isSynthesizedFuncName(Name)) {
      llvm::StringRef Hex = Name;
      if (!Hex.consume_front(kAutoFuncPrefix))
        Hex.consume_front(kAutoFuncPrefixEVM);
      uint64_t Addr = 0;
      if (!Hex.empty() && !Hex.getAsInteger(16, Addr) && Addr != 0) {
        if (Dbg) {
          if (auto FS = Dbg->resolveFunction(static_cast<va_t>(Addr));
              FS && !FS->Name.empty())
            DebugName = FS->Name;
        }
        if (DebugName.empty() && Img) {
          std::string FromImage =
              Img->getFunctionNameAt(static_cast<va_t>(Addr));
          if (!FromImage.empty() && !isSynthesizedFuncName(FromImage))
            DebugName = std::move(FromImage);
          if (!DebugName.empty())
            if (auto Shadow =
                    peImportShadowName(ImportCNames, *Img, Addr, DebugName))
              DebugName = std::move(*Shadow);
          if (DebugName.empty())
            if (const Import *Imp = Img->findImportStubAt(Addr);
                Imp && !Imp->Name.empty())
              DebugName =
                  symbolOfImportName(Imp->Name, Opts.Format, Opts.TheArch);
        }
      }
    }
    // An image symbol carries the format's decoration; an LLVM name may have
    // lost it already.  A definition steps aside from the C runtime's names.
    llvm::StringRef CName =
        DebugName.empty() ? cNameOfGlobal(Name, Fn.isDeclaration())
                          : cNameOfSymbol(DebugName, Opts.Format, Opts.TheArch);
    FunctionSymbolNames.emplace(&Fn, CName.str());
    FunctionSources.emplace(&Fn, DebugName.empty() ? Name.str() : DebugName);
    std::string Stem;
    if (!Fn.isDeclaration()) {
      CName = cDefinitionName(CName, Opts.Format);
      // The image's own copy of a C library routine, as a statically linked
      // `atexit`, is not the routine a header or the runtime's tables
      // declare the name for, and takes its address as a body lifted under
      // an import's name does (`fprintf_1400014B0`), one of mingw-w64's
      // own routines a Windows program links too (`__mingw_printf`).  So
      // does a whole module's `main`, which C holds to the parameters and
      // result it gives main, not those the code has.
      if (libc::isKnownFunction(CName) ||
          libc::libcPrototype(CName, Opts.Format) ||
          (Opts.Format == BinaryFormat::COFF &&
           libc::mingwHeadersDeclare(CName)) ||
          (!OnlyFunction && CName == "main")) {
        auto VA = rewrite_source::getOriginalVA(Fn);
        if (!VA)
          llvm::consumeError(VA.takeError());
        else if (*VA)
          Stem = (CName + "_" + llvm::utohexstr(**VA)).str();
      }
    }
    if (!Stem.empty())
      CName = Stem;
    const auto Entry =
        FunctionIdentifiers
            .emplace(&Fn,
                     GlobalIdentifierAllocator.allocate(CName, "nd_function"))
            .first;
    FunctionIdentifierNames.insert(Entry->second);
  }
  std::set<const llvm::GlobalVariable *> CallableGlobals;
  for (const auto &Function : Mod)
    for (const auto &Block : Function)
      for (const auto &Instruction : Block)
        if (const auto *Call = llvm::dyn_cast<llvm::CallBase>(&Instruction))
          if (const auto *Global = llvm::dyn_cast<llvm::GlobalVariable>(
                  Call->getCalledOperand()->stripPointerCasts()))
            CallableGlobals.insert(Global);
  for (const auto &Global : Mod.globals())
    if (Global.isDeclaration() && !Global.getName().empty() &&
        data_symbol::isMarked(Global) && !CallableGlobals.count(&Global) &&
        !parseNdDataSymbol(Global.getName()) &&
        !parseNdCodePtrSymbol(Global.getName()) &&
        !Global.getName().starts_with("__imp_") &&
        !Global.getName().starts_with("_imp_"))
      ExternalDataIdentifiers.emplace(
          &Global, GlobalIdentifierAllocator.allocate(
                       "neverd_data_" + Global.getName().str(), "neverd_data"));
}

std::string LLVMCWriter::functionIdentifier(const llvm::Function &Fn) const {
  if (auto It = FunctionIdentifiers.find(&Fn); It != FunctionIdentifiers.end())
    return It->second;
  llvm::StringRef CName = cNameOfGlobal(Fn.getName(), Fn.isDeclaration());
  if (!Fn.isDeclaration())
    CName = cDefinitionName(CName, Opts.Format);
  return canonicalizeCProjectionIdentifier(CName, "nd_function");
}

llvm::StringRef LLVMCWriter::cNameOfGlobal(llvm::StringRef Name,
                                           bool Declaration) const {
  if (Declaration && ImportEntryCNames.contains(Name))
    return Name;
  return llvm_name::cNameOfLLVMName(Name, Opts.Format, Opts.TheArch);
}

namespace {
struct RestoreOutput {
  LLVMCOut &Out;
  llvm::raw_ostream *Original;
  ~RestoreOutput() { Out.retarget(Original); }
};
} // namespace

void LLVMCWriter::writeModule(llvm::Module &Mod, const llvm::Function *Only) {
  OnlyFunction = Only;
  SelectedGlobals =
      Only ? referencedGlobals(*Only) : std::set<const llvm::GlobalValue *>{};
  EmittedGlobalNames.clear();
  RecordPointerTags.clear();
  CurMod = &Mod;
  prepareFunctionIdentifiers(Mod);
  collectImageDataUses(Mod);
  writeIncludes(Mod);
  OS << "\n";
  // What follows names the program's records, whose definitions go before
  // it once it is written.
  std::string Rest;
  llvm::raw_string_ostream RestOS(Rest);
  {
    RestoreOutput Restore{OS, &OS.stream()};
    OS.retarget(&RestOS);
    writeModuleBody(Mod);
  }
  writeRecordDefinitions(Rest);
  OS << Rest;
}

std::string LLVMCWriter::recordPointerSpelling(const NdType &Record) {
  std::string Tag = cNamedTypeSpelling(Record.SourceName);
  if (Tag.empty())
    return {};
  RecordPointerTags.insert(Tag);
  return Tag + "*";
}

/// \p Text with each use of \p Tag as a type (a declaration's, a cast's)
/// spelled with its keyword: `struct stat buf`, where a function of the
/// same name hides the bare one.
static void spellTagUses(std::string &Text, llvm::StringRef Tag,
                         llvm::StringRef Keyword) {
  const auto Identifier = [](char C) { return llvm::isAlnum(C) || C == '_'; };
  for (size_t At = Text.find(Tag.data(), 0, Tag.size());
       At != std::string::npos; At = Text.find(Tag.data(), At, Tag.size())) {
    const size_t End = At + Tag.size();
    size_t Before = At;
    while (Before > 0 && Text[Before - 1] == ' ')
      --Before;
    size_t After = End;
    while (After < Text.size() && Text[After] == ' ')
      ++After;
    const bool Word = (At == 0 || !Identifier(Text[At - 1])) &&
                      (End == Text.size() || !Identifier(Text[End]));
    const bool Member =
        Before > 0 && (Text[Before - 1] == '.' || Text[Before - 1] == '>');
    const bool Tagged =
        llvm::StringRef(Text).take_front(Before).ends_with(Keyword.str());
    const bool Type =
        After < Text.size() && (Text[After] == '*' || Identifier(Text[After]));
    if (Word && !Member && !Tagged && Type) {
      Text.insert(At, Keyword.str() + " ");
      At += Keyword.size() + 1;
    }
    At += Tag.size();
  }
}

void LLVMCWriter::writeRecordDefinitions(std::string &Text) {
  if (!Opts.EmitIncludes)
    return;
  // A record only pointers name, which no definition below completes, is
  // declared incomplete, as HighC declares it.  A name the unit gives a
  // function or an object of its own is that, not a type.
  auto DeclareIncomplete = [&](const std::set<std::string> &Defined) {
    bool Any = false;
    for (const std::string &Tag : RecordPointerTags)
      if (!Defined.count(Tag) && !GlobalIdentifierAllocator.holds(Tag) &&
          !EmittedGlobalNames.count(Tag)) {
        OS << "typedef struct " << Tag << " " << Tag << ";\n";
        Any = true;
      }
    if (Any)
      OS << "\n";
  };
  if (!Opts.RecordDefinitions) {
    DeclareIncomplete({});
    return;
  }
  // Every identifier the text spells: a record's or an enumerator's name
  // among them.
  std::set<std::string> Words;
  for (size_t I = 0; I < Text.size();) {
    if (!llvm::isAlpha(Text[I]) && Text[I] != '_') {
      ++I;
      continue;
    }
    size_t End = I + 1;
    while (End < Text.size() && (llvm::isAlnum(Text[End]) || Text[End] == '_'))
      ++End;
    if (I == 0 || (!llvm::isAlnum(Text[I - 1]) && Text[I - 1] != '_'))
      Words.insert(Text.substr(I, End - I));
    I = End;
  }
  std::vector<TypeRef> Named;
  Named.reserve(Words.size());
  for (const std::string &Word : Words)
    Named.push_back(NdType::makeNamedRecord(Word, 0));
  // And the records whose members it reads, which it need not spell.
  for (const std::string &Name : MemberRecords)
    if (!Words.count(Name))
      Named.push_back(NdType::makeNamedRecord(Name, 0));
  CRecordDefinitions Definitions = Opts.RecordDefinitions(Named);
  // The records and enumerations the text spells by their tags' names,
  // which typedefs of those names declare.
  std::vector<std::string> Spelled;
  for (const auto &[Name, Union] : Definitions.Tags)
    if (Words.count(Name))
      Spelled.push_back(Name);
  std::vector<std::string> Enumerations;
  for (const std::string &Name : Definitions.Enumerations)
    if (Words.count(Name))
      Enumerations.push_back(Name);
  // A name the definitions declare that the unit gives a function or an
  // object of its own leaves them out: one name would mean both.  A record
  // the text spells by such a name is spelled by its tag, as a header the
  // unit includes may define it (glibc's `struct stat` beside `stat`).
  std::vector<std::string> Declared = Definitions.Identifiers;
  llvm::append_range(Declared, Spelled);
  llvm::append_range(Declared, Enumerations);
  const auto Held = [&](const std::string &Name) {
    return GlobalIdentifierAllocator.holds(Name) ||
           EmittedGlobalNames.count(Name);
  };
  for (const std::string &Name : Declared)
    if (Held(Name)) {
      for (const std::string &Tag : Spelled)
        if (Held(Tag))
          spellTagUses(Text, Tag, Definitions.Tags[Tag] ? "union" : "struct");
      OS << "/* The local types' definitions are left out: " << Name
         << " names one of the unit's functions or objects too. */\n\n";
      DeclareIncomplete({});
      return;
    }
  DeclareIncomplete(std::set<std::string>(Spelled.begin(), Spelled.end()));
  for (const std::string &Name : Spelled)
    OS << "typedef " << (Definitions.Tags[Name] ? "union " : "struct ") << Name
       << " " << Name << ";\n";
  if (!Definitions.Text.empty())
    OS << "\n" << Definitions.Text << "\n";
  // An enumeration's type is complete only where it is defined.
  for (const std::string &Name : Enumerations)
    OS << "typedef enum " << Name << " " << Name << ";\n";
  if (!Enumerations.empty())
    OS << "\n";
}

void LLVMCWriter::writeModuleBody(llvm::Module &Mod) {
  writeStructDefs(Mod);
  for (const auto &Global : Mod.globals()) {
    if (OnlyFunction && !SelectedGlobals.count(&Global))
      continue;
    const auto It = ExternalDataIdentifiers.find(&Global);
    if (It == ExternalDataIdentifiers.end())
      continue;
    const auto &Identifier = It->second;
    OS << "extern unsigned char " << Identifier << "[] __asm__(\""
       << escapeCString(cNameOfGlobal(Global.getName(), true)) << "\")";
    if (Global.hasExternalWeakLinkage())
      OS << " __attribute__((weak))";
    OS << ";\n";
  }
  auto WriteFunctions = [&] {
    for (auto &Fn : Mod) {
      if (Fn.isDeclaration())
        continue;
      if (OnlyFunction && &Fn != OnlyFunction)
        continue;
      auto Event = SourceRecorder ? SourceRecorder->function(Fn) : std::nullopt;
      if (SourceRecorder)
        OS << SourceRecorder->definition(Fn);
      if (Event)
        OS << SourceRecorder->begin(*Event);
      writeFunction(Fn);
      if (Event)
        OS << SourceRecorder->end(*Event);
      OS << "\n";
    }
  };
  if (OnlyFunction) {
    writeForwardDecls(Mod);
    writeGlobals(Mod);
    // The objects the function only takes the address of are declared where
    // its text names them, so that text comes first.
    std::string Tail;
    llvm::raw_string_ostream TailOS(Tail);
    {
      RestoreOutput Restore{OS, &OS.stream()};
      OS.retarget(&TailOS);
      writeImportCalleeDecls(Mod);
      WriteFunctions();
    }
    writeReferencedImageObjects({OnlyFunction}, Tail);
    OS << Tail;
    recordSourceNames(Mod);
    return;
  }

  // A global initializer can reference any definition, including a later
  // one. Render each body once and retain its actual C prototype instead of
  // guessing that the LLVM type matches an inferred-void or debug projection.
  std::string Functions;
  llvm::raw_string_ostream FunctionsOS(Functions);
  {
    RestoreOutput Restore{OS, &OS.stream()};
    OS.retarget(&FunctionsOS);
    WriteFunctions();
  }
  writeForwardDecls(Mod);
  // A global's initializer can hold an import's address, as an import
  // table's mirror does.
  writeImportCalleeDecls(Mod);
  writeGlobals(Mod);
  // Code that reaches image data through a mirror the environment holds
  // names the data itself, as one function's output does.
  std::vector<const llvm::Function *> Defined;
  for (const auto &Fn : Mod)
    if (!Fn.isDeclaration())
      Defined.push_back(&Fn);
  writeReferencedImageObjects(Defined, Functions);
  OS << "\n" << Functions;
  recordSourceNames(Mod);
}

void LLVMCWriter::recordSourceNames(const llvm::Module &Mod) {
  // The marked renders spell the same names.
  if ((!Opts.SourceMap && !Opts.SourceNames) || SourceRecorder)
    return;
  std::map<std::string, CSourceName> ByIdentifier;
  for (const llvm::Function &Fn : Mod) {
    const auto Identifier = FunctionIdentifiers.find(&Fn);
    const auto Source = FunctionSources.find(&Fn);
    if (Fn.isIntrinsic() || Identifier == FunctionIdentifiers.end() ||
        Source == FunctionSources.end())
      continue;
    std::optional<va_t> Entry;
    if (auto VA = rewrite_source::getOriginalVA(Fn))
      Entry = *VA;
    else
      llvm::consumeError(VA.takeError());
    auto [It, Inserted] = ByIdentifier.try_emplace(Identifier->second);
    CSourceName &Name = It->second;
    if (Inserted) {
      Name.Identifier = Identifier->second;
      Name.Symbol = Source->second;
      Name.Address = Entry;
      continue;
    }
    // One identifier for several symbols names none of them.
    if (Name.Symbol != Source->second)
      Name.Symbol.clear();
    if (Name.Address != Entry)
      Name.Address.reset();
  }
  std::vector<CSourceName> Names;
  Names.reserve(ByIdentifier.size());
  for (auto &[Identifier, Name] : ByIdentifier)
    Names.push_back(std::move(Name));
  if (Opts.SourceNames)
    *Opts.SourceNames = Names;
  if (Opts.SourceMap)
    Opts.SourceMap->Names = std::move(Names);
}

void LLVMCWriter::writeIncludes(llvm::Module &Mod) {
  bool HasFloatingArithmetic = false;
  std::set<std::string> Headers;
  Headers.insert("stdint.h");
  NeedsUnalignedTypes = false;
  std::map<std::string, std::pair<unsigned, bool>> FunnelShifts;
  std::map<std::string, ScalarIntegerMinMax> IntegerMinMax;
  std::map<std::string, ScalarIntegerSaturation> IntegerSaturations;
  std::map<std::string, ScalarUnary> ScalarUnaries;
  FPStateHelperNames.clear();
  SyscallHelperNames.clear();

  for (auto &Fn : Mod) {
    if (OnlyFunction && &Fn != OnlyFunction)
      continue;
    // Windows exception handling prints as `__try` with the filter
    // dispositions and intrinsics <excpt.h> defines.
    if (Opts.Format == BinaryFormat::COFF && !Fn.isDeclaration() &&
        (functionHasWindowsEHPads(Fn) ||
         (GuardAnalysisOnlyFunctions && isAnalysisOnlyFunction(Fn) &&
          functionNeedsAnalysisOnlyEHWrap(Fn))))
      Headers.insert("excpt.h");
    for (auto &BB : Fn) {
      for (auto &Inst : BB) {
        HasFloatingArithmetic |= Inst.getOpcode() == llvm::Instruction::FAdd ||
                                 Inst.getOpcode() == llvm::Instruction::FSub ||
                                 Inst.getOpcode() == llvm::Instruction::FMul ||
                                 Inst.getOpcode() == llvm::Instruction::FDiv;
        if (const auto *Call = llvm::dyn_cast<llvm::CallBase>(&Inst)) {
          if (auto Shape = scalarIntegerSaturation(*Call))
            IntegerSaturations.emplace(
                functionIdentifier(*Call->getCalledFunction()), *Shape);
          if (auto Shape = scalarIntegerMinMax(*Call))
            IntegerMinMax.emplace(
                functionIdentifier(*Call->getCalledFunction()), *Shape);
          if (auto Shape = scalarUnary(*Call))
            ScalarUnaries.emplace(
                functionIdentifier(*Call->getCalledFunction()), *Shape);
        }
        if (llvm::isa<llvm::FenceInst>(&Inst))
          HasCIntrinsics = true;
        // A library routine called through its import takes the header that
        // declares it, as a direct call does, and one the runtime's tables
        // type the header its prototype's types need (`FILE`).
        if (const auto *Call = llvm::dyn_cast<llvm::CallBase>(&Inst);
            Call && !Call->getCalledFunction())
          if (const LibraryCallee Library = libraryCallee(*Call)) {
            if (!Library.Prototype)
              Headers.insert(libc::headerFor(Library.Name));
            else if (!Library.Prototype->Header.empty())
              Headers.insert(std::string(Library.Prototype->Header));
          }
        if (auto *LI = llvm::dyn_cast<llvm::LoadInst>(&Inst)) {
          if (isLLVMX86SegmentedAddressSpace(LI->getPointerAddressSpace())) {
            HasCIntrinsics = true;
            // Windows reads the segment as MSVC's <intrin.h> spells it
            // (__readgsqword).
            if (Opts.Format == BinaryFormat::COFF)
              Headers.insert("intrin.h");
          }
          NeedsUnalignedTypes |=
              LI->isSimple() && LI->getPointerAddressSpace() == 0 &&
              !c_memory::alias(typeToCLLVM(LI->getType()), Opts.ScalarPointers)
                   .empty();
        }
        if (const auto *SI = llvm::dyn_cast<llvm::StoreInst>(&Inst))
          NeedsUnalignedTypes |=
              SI->isSimple() && SI->getPointerAddressSpace() == 0 &&
              !c_memory::alias(typeToCLLVM(SI->getValueOperand()->getType()),
                               Opts.ScalarPointers)
                   .empty();
        if (auto *CI = llvm::dyn_cast<llvm::CallInst>(&Inst)) {
          if (classifyX86ShadowStackReadAsm(*CI)) {
            const unsigned Word = CI->getType()->getIntegerBitWidth() / 8;
            if ((Opts.TheArch != Arch::X86 && Opts.TheArch != Arch::X64) ||
                Word != (Opts.TheArch == Arch::X64 ? 8U : 4U))
              llvm::report_fatal_error(
                  "shadow stack read target/type mismatch");
          }
          if (const SyscallConvention *Syscall =
                  modeledSyscallInlineAsm(Opts.TheArch, *CI)) {
            auto [It, Inserted] =
                SyscallHelperNames.try_emplace(Syscall->Modeled);
            if (Inserted)
              It->second = GlobalIdentifierAllocator.allocate(
                  intrinsicCName(Syscall->Modeled), "nd_syscall");
          }
          if (auto Shape = classifyX86FPStateAsm(*CI);
              Shape && isX86FPNumericalStateIntrinsic(Shape->first)) {
            if (Opts.TheArch == Arch::X86 &&
                isX86FPConversionStateIntrinsic(Shape->first) &&
                x86FPStateDestinationBytes(Shape->first, Shape->second) == 8)
              llvm::report_fatal_error(
                  "x86-32 FP conversion requires a 32-bit integer result");
            auto [It, Inserted] = FPStateHelperNames.try_emplace(*Shape);
            if (Inserted)
              It->second = GlobalIdentifierAllocator.allocate(
                  x86FPScalarValueCHelper(Shape->first, Shape->second),
                  "nd_fp_value");
          }
          if (const auto *Asm =
                  llvm::dyn_cast<llvm::InlineAsm>(CI->getCalledOperand())) {
            if (Asm->getAsmString().starts_with("pushf") ||
                Asm->getAsmString() == "pushq $0\n\tpopfq" ||
                Asm->getAsmString() == "pushl $0\n\tpopfl")
              Headers.insert(Opts.Format == BinaryFormat::COFF ? "intrin.h"
                                                               : "x86intrin.h");
            // A control or debug register move prints as MSVC's __readcr8 or
            // __writedr (renderX86InlineAsm).
            if (const llvm::StringRef Text = Asm->getAsmString();
                Opts.Format == BinaryFormat::COFF && Text.starts_with("mov ") &&
                (Text.contains("%cr") || Text.contains("%dr")))
              Headers.insert("intrin.h");
            if ((Opts.TheArch == Arch::X86 || Opts.TheArch == Arch::X64) &&
                (Asm->getAsmString() == "ldmxcsr ($0)" ||
                 Asm->getAsmString() == "stmxcsr ($0)"))
              HasCIntrinsics = true;
            continue;
          }
          auto *Callee = CI->getCalledFunction();
          if (!Callee)
            continue;
          const auto IID = Callee->getIntrinsicID();
          if (IID == llvm::Intrinsic::memcpy ||
              IID == llvm::Intrinsic::memmove || IID == llvm::Intrinsic::memset)
            Headers.insert("string.h");
          if (IID == llvm::Intrinsic::fshl || IID == llvm::Intrinsic::fshr) {
            const auto *Ty = llvm::dyn_cast<llvm::IntegerType>(CI->getType());
            const unsigned Width = Ty ? Ty->getBitWidth() : 0;
            if (CI->arg_size() != 3 ||
                (Width != 8 && Width != 16 && Width != 32 && Width != 64 &&
                 Width != 128))
              throw std::runtime_error("unsupported LLVM funnel shift width");
            FunnelShifts.emplace(
                functionIdentifier(*Callee),
                std::pair{Width, IID == llvm::Intrinsic::fshl});
          }
          auto Name = Callee->getName().str();

          if (const char *Mapped = llvmIntrinsicToCName(Name.c_str())) {
            // Generic arithmetic and control builtins use host C facilities.
            // Only mappings that actually name ISA intrinsics need the
            // recovered image's architecture headers.
            const llvm::StringRef RawName(Name);
            if (RawName.starts_with("llvm.x86.") ||
                RawName.starts_with("llvm.aarch64.") ||
                RawName.starts_with("llvm.arm.") ||
                IID == llvm::Intrinsic::readcyclecounter ||
                IID == llvm::Intrinsic::debugtrap)
              HasCIntrinsics = true;
            IntrinsicMappedNames.insert(Name);
            if (const char *Header = libc::headerFor(Mapped))
              Headers.insert(Header);
          }
          if (isNativeVectorIntrinsic(*CI, Opts.TheArch))
            HasCIntrinsics = true;
          if (isX86FastFailName(Name))
            HasCIntrinsics = true;

          if (const char *Hdr = libc::headerFor(stripLeadingUnderscores(Name)))
            Headers.insert(Hdr);
          // A callee the image names as a C library routine, as the
          // Windows kernel's own memset is, is declared by that header too
          // (writeForwardDecls leaves it to the header).
          else if (const char *Named =
                       libc::headerFor(functionIdentifier(*Callee)))
            Headers.insert(Named);
        }
      }
    }
  }

  // A stub of a variadic import passes its arguments on through stdarg.h.
  if (Img)
    for (llvm::Function &Fn : Mod) {
      if (Fn.isDeclaration())
        continue;
      auto VA = rewrite_source::getOriginalVA(Fn);
      if (!VA) {
        llvm::consumeError(VA.takeError());
        continue;
      }
      if (!*VA)
        continue;
      if (const std::string Import = c_stub::variadicImportOfStub(*Img, **VA);
          !Import.empty())
        if (const auto *Forward = libc::libcVariadicForward(Import))
          c_stub::addVariadicStubHeaders(Headers, *Forward);
    }

  if (HasCIntrinsics)
    for (const char *Hdr : getArchIntrinsicHeaders(Opts.TheArch))
      Headers.insert(Hdr);

  IncludedHeaders.clear();
  if (Opts.EmitIncludes) {
    for (auto &H : Headers)
      OS << "#include <" << H << ">\n";
    OS << "\n";
    IncludedHeaders.insert(Headers.begin(), Headers.end());
  }
  if (HasFloatingArithmetic)
    c_float::writeContractionPolicy(OS.stream());
  // The accesses' types or assumptions are written only when an access can
  // name a type; otherwise every access keeps its portable byte copy.
  UnalignedTypesWritten = Opts.UseUnalignedPointers && NeedsUnalignedTypes;
  if (UnalignedTypesWritten)
    c_memory::writeTypes(OS, Opts.ScalarPointers);
  for (const auto &[Name, Shape] : ScalarUnaries) {
    const unsigned CarrierBits = Shape.carrierBits();
    const std::string Type = CarrierBits == 128
                                 ? "unsigned __int128"
                                 : "uint" + std::to_string(CarrierBits) + "_t";
    if (Shape.Kind == llvm::Intrinsic::bitreverse) {
      OS << "static inline " << Type << " " << Name << "(" << Type
         << " value) {\n"
         << "    " << Type << " result = 0;\n"
         << "    for (unsigned int bit = 0; bit < " << Shape.Bits
         << "; ++bit) {\n"
         << "        result = (" << Type << ")((result << 1) | (value & 1));\n"
         << "        value >>= 1;\n"
         << "    }\n    return result;\n}\n\n";
      continue;
    }
    c_float::writeConversion(OS, Name,
                             {Shape.Bits, Shape.FloatBits,
                              Shape.Kind == llvm::Intrinsic::fptosi_sat,
                              FPToIntegerPolicy::Saturate});
  }
  writeX86FPScalarValueCHelpers(OS, FPStateHelperNames);
  for (const auto &[Id, Name] : SyscallHelperNames)
    if (const SyscallConvention *Syscall = modeledSyscallConvention(Id))
      OS << linuxSyscallHelperDefinition(*Syscall, Name);
  for (const auto &[Name, Shape] : IntegerSaturations) {
    const unsigned CarrierBits = Shape.Bits == 1 ? 8 : Shape.Bits;
    const std::string Type = CarrierBits == 128
                                 ? "unsigned __int128"
                                 : "uint" + std::to_string(CarrierBits) + "_t";
    OS << "static inline " << Type << " " << Name << "(" << Type << " a, "
       << Type << " b) {\n";
    if (Shape.Bits == 1)
      OS << "    a &= 1;\n    b &= 1;\n";
    if (Shape.Add) {
      OS << "    const " << Type << " maximum = (" << Type << ")"
         << (Shape.Bits == 1 ? "1" : "-1") << ";\n"
         << "    return a > maximum - b ? maximum : (" << Type << ")(a + b);\n";
    } else {
      OS << "    return a < b ? 0 : (" << Type << ")(a - b);\n";
    }
    OS << "}\n\n";
  }
  for (const auto &[Name, Shape] : IntegerMinMax) {
    const unsigned CarrierBits = Shape.Bits == 1 ? 8 : Shape.Bits;
    const std::string Type = CarrierBits == 128
                                 ? "unsigned __int128"
                                 : "uint" + std::to_string(CarrierBits) + "_t";
    OS << "static inline " << Type << " " << Name << "(" << Type << " a, "
       << Type << " b) {\n";
    if (Shape.Bits == 1)
      OS << "    a &= 1;\n    b &= 1;\n";
    if (Shape.Signed) {
      // Flipping the sign bit maps two's-complement signed order to unsigned
      // order without an out-of-range signed cast or a 128-bit truncation.
      OS << "    const " << Type << " sign = (" << Type << ")1 << "
         << Shape.Bits - 1 << ";\n"
         << "    return (a ^ sign) " << (Shape.Minimum ? '<' : '>')
         << " (b ^ sign) ? a : b;\n";
    } else {
      OS << "    return a " << (Shape.Minimum ? '<' : '>') << " b ? a : b;\n";
    }
    OS << "}\n\n";
  }
  for (const auto &[Name, Shape] : FunnelShifts) {
    const auto [Width, Left] = Shape;
    const std::string Type = Width == 128
                                 ? "unsigned __int128"
                                 : "uint" + std::to_string(Width) + "_t";
    OS << "static inline " << Type << " " << Name << "(" << Type << " a, "
       << Type << " b, " << Type << " amount) {\n"
       << "    unsigned int shift = (unsigned int)(amount % " << Width
       << ");\n";
    if (Left)
      OS << "    return shift == 0 ? a : (" << Type
         << ")((a << shift) | (b >> (" << Width << " - shift)));\n";
    else
      OS << "    return shift == 0 ? b : (" << Type << ")((a << (" << Width
         << " - shift)) | (b >> shift));\n";
    OS << "}\n\n";
  }
}

void LLVMCWriter::writeStructDefs(llvm::Module &Mod) {
  std::set<llvm::Type *> Seen;
  std::function<void(llvm::Type *)> Visit = [&](llvm::Type *Ty) {
    if (!Seen.insert(Ty).second)
      return;
    if (auto *FT = llvm::dyn_cast<llvm::FunctionType>(Ty)) {
      Visit(FT->getReturnType());
      for (auto *Parameter : FT->params())
        Visit(Parameter);
      return;
    }
    if (!Ty->isAggregateType())
      return;
    if (auto *ST = llvm::dyn_cast<llvm::StructType>(Ty); ST && ST->isOpaque()) {
      OS << typeToCLLVM(Ty) << ";\n";
      return;
    }
    for (auto *Child : Ty->subtypes())
      Visit(Child);
    // Selected-function fragments may be combined in one C translation unit.
    // Include the complete LLVM shape, so equal records share a definition
    // while incompatible named records still produce a C redefinition error.
    std::string Shape;
    llvm::raw_string_ostream ShapeOut(Shape);
    Ty->print(ShapeOut);
    llvm::MD5 Hash;
    Hash.update(Shape);
    const std::string Guard =
        "NEVERD_C_AGGREGATE_" + Hash.final().digest().str().str();
    OS << "#ifndef " << Guard << "\n#define " << Guard << "\n";
    OS << typeToCLLVM(Ty) << " {\n";
    if (auto *Array = llvm::dyn_cast<llvm::ArrayType>(Ty)) {
      OS << "    " << typeToCLLVM(Array->getElementType()) << " elements["
         << Array->getNumElements() << "];\n";
    } else {
      auto *ST = llvm::cast<llvm::StructType>(Ty);
      for (unsigned I = 0; I < ST->getNumElements(); ++I)
        OS << "    " << typeToCLLVM(ST->getElementType(I)) << " field_" << I
           << ";\n";
    }
    OS << "}";
    if (auto *ST = llvm::dyn_cast<llvm::StructType>(Ty); ST && ST->isPacked())
      OS << " __attribute__((packed))";
    OS << ";\n#endif\n";
    const auto &Layout = Mod.getDataLayout();
    OS << "_Static_assert(sizeof(" << typeToCLLVM(Ty)
       << ") == " << Layout.getTypeAllocSize(Ty)
       << ", \"LLVM aggregate size\");\n";
    if (auto *ST = llvm::dyn_cast<llvm::StructType>(Ty)) {
      const auto *RecordLayout = Layout.getStructLayout(ST);
      for (unsigned I = 0; I < ST->getNumElements(); ++I)
        OS << "_Static_assert(__builtin_offsetof(" << typeToCLLVM(Ty)
           << ", field_" << I << ") == " << RecordLayout->getElementOffset(I)
           << ", \"LLVM aggregate field offset\");\n";
    }
    OS << "\n";
  };
  auto VisitStorage = [&](llvm::Type *Ty) {
    if (auto *Array = llvm::dyn_cast<llvm::ArrayType>(Ty))
      Visit(Array->getElementType());
    else
      Visit(Ty);
  };
  // One function's output declares the image's storage as bytes, whose
  // record of every pointer in a section it does not define.
  for (auto &Global : Mod.globals())
    if (!OnlyFunction || !mirrorsImageStorage(Global))
      VisitStorage(Global.getValueType());
  for (auto &Fn : Mod) {
    Visit(Fn.getFunctionType());
    if (OnlyFunction && &Fn != OnlyFunction)
      continue;
    for (auto &BB : Fn)
      for (auto &Inst : BB) {
        Visit(Inst.getType());
        if (auto *Allocation = llvm::dyn_cast<llvm::AllocaInst>(&Inst))
          VisitStorage(Allocation->getAllocatedType());
        for (const auto &Operand : Inst.operands())
          Visit(Operand->getType());
      }
  }
}

void LLVMCWriter::writeImageByteArray(const llvm::GlobalVariable &Global,
                                      llvm::StringRef Name) {
  const auto *Array = llvm::cast<llvm::ArrayType>(Global.getValueType());
  OS << llvmc::storageAlignment(Global.getValueType(),
                                Global.getParent()->getDataLayout(),
                                Global.getAlign().valueOrOne());
  if (Global.hasLocalLinkage())
    OS << "static ";
  if (Global.isConstant())
    OS << "const ";
  OS << "uint8_t " << Name << "[" << Array->getNumElements() << "] = {";
  std::string Elements;
  if (!Global.getInitializer()->isNullValue())
    for (uint64_t I = 0; I < Array->getNumElements(); ++I) {
      const auto *Element = llvm::dyn_cast_or_null<llvm::ConstantInt>(
          Global.getInitializer()->getAggregateElement(
              static_cast<unsigned>(I)));
      if (!Element)
        throw std::invalid_argument(
            "non-byte initializer in image byte storage");
      if (!Element->isZero())
        Elements += (Elements.empty() ? "[" : ", [") + std::to_string(I) +
                    "] = " + std::to_string(Element->getZExtValue());
    }
  OS << (Elements.empty() ? "0" : wrapInitializer(std::move(Elements)))
     << "};\n";
}

void LLVMCWriter::writeGlobals(llvm::Module &Mod) {
  // Initializers can refer forward or form cycles. The declarations use the
  // same storage shape and linkage as the definitions below.
  auto declare = [&](const llvm::GlobalVariable &Global) {
    OS << llvmc::storageAlignment(Global.getValueType(), Mod.getDataLayout(),
                                  Global.getAlign().valueOrOne());
    OS << (Global.hasLocalLinkage() ? "static " : "extern ");
    if (Global.isConstant())
      OS << "const ";
    auto *Array = llvm::dyn_cast<llvm::ArrayType>(Global.getValueType());
    const auto *Data =
        llvm::dyn_cast<llvm::ConstantDataArray>(Global.getInitializer());
    const bool String = Global.isConstant() && Data && Data->isString() &&
                        !parseNdDataSymbol(Global.getName());
    OS << (String ? "char"
                  : typeToCLLVM(Array ? Array->getElementType()
                                      : Global.getValueType()))
       << " " << globalNameStr(&Global);
    if (Array)
      OS << "[" << Array->getNumElements() << "]";
    OS << ";\n";
  };
  if (OnlyFunction) {
    for (const auto &Global : Mod.globals())
      if (SelectedGlobals.count(&Global) && Global.hasInitializer() &&
          !mirrorsImageStorage(Global))
        declare(Global);
  } else {
    // The whole module defines the image's storage too, in module order: a
    // mirror built while an earlier mirror's initializer resolved follows
    // the initializer that names it.
    std::set<const llvm::GlobalVariable *> Defined, NamedEarlier;
    for (const auto &Global : Mod.globals()) {
      Defined.insert(&Global);
      if (!Global.hasInitializer())
        continue;
      llvm::SmallVector<const llvm::Constant *, 16> Work{
          Global.getInitializer()};
      llvm::SmallPtrSet<const llvm::Constant *, 16> Seen;
      while (!Work.empty()) {
        const llvm::Constant *C = Work.pop_back_val();
        if (!Seen.insert(C).second)
          continue;
        if (const auto *Var = llvm::dyn_cast<llvm::GlobalVariable>(C)) {
          if (!Defined.count(Var))
            NamedEarlier.insert(Var);
        } else if (!llvm::isa<llvm::GlobalValue>(C)) {
          for (const llvm::Use &Operand : C->operands())
            if (const auto *Next = llvm::dyn_cast<llvm::Constant>(Operand))
              Work.push_back(Next);
        }
      }
    }
    for (const auto &Global : Mod.globals())
      if (NamedEarlier.count(&Global) && Global.hasInitializer() &&
          !Global.getName().empty() &&
          !ExternalDataIdentifiers.count(&Global) &&
          !sourceStringOnlyCalledByValue(Global))
        declare(Global);
  }
  // Permission to read a scalar from the image is not permission to remove a
  // volatile/atomic access. Its named object must survive declaration pruning.
  std::set<va_t> ObservedImageObjects;
  for (const auto &Fn : Mod)
    for (const auto &BB : Fn)
      for (const auto &Inst : BB) {
        const llvm::Value *Ptr = nullptr;
        if (const auto *Load = llvm::dyn_cast<llvm::LoadInst>(&Inst);
            Load && !Load->isSimple())
          Ptr = Load->getPointerOperand();
        if (const auto *Store = llvm::dyn_cast<llvm::StoreInst>(&Inst))
          Ptr = Store->getPointerOperand();
        if (Ptr)
          if (auto VA = imageDataVA(Ptr))
            ObservedImageObjects.insert(*VA);
      }
  for (auto &GV : Mod.globals()) {
    if (OnlyFunction && !SelectedGlobals.count(&GV))
      continue;
    std::string RawName = GV.getName().str();
    if (RawName.empty())
      continue;

    if (ExternalDataIdentifiers.count(&GV))
      continue;
    // IAT/import slots are callable names at call sites, not C objects.
    // Printing `__imp_??1?$CStringT@...` as `extern uint64_t` is not C.
    if (!GV.hasInitializer()) {
      llvm::StringRef Raw = RawName;
      std::string ImportName;
      if (Raw.starts_with("__imp_") || Raw.starts_with("_imp_") ||
          Raw.starts_with("??") || Raw.starts_with("ord_"))
        ImportName = canonicalizeCProjectionIdentifier(Raw, "nd_import");
      else if (auto Slot = parseNdCodePtrSymbol(Raw)) {
        if (Img)
          if (const Import *Imp = Img->findImportAt(*Slot);
              Imp && !Imp->Name.empty())
            ImportName =
                canonicalizeCProjectionIdentifier(Imp->Name, "nd_import");
      } else if (auto Slot = parseNdDataSymbol(Raw)) {
        if (Img)
          if (const Import *Imp = Img->findImportAt(*Slot);
              Imp && !Imp->Name.empty())
            ImportName =
                canonicalizeCProjectionIdentifier(Imp->Name, "nd_import");
      }
      if (!ImportName.empty())
        continue;
    }

    std::string Name = resolveNdDataName(RawName);
    if (Name.empty()) {
      std::string Identifier = RawName;
      if (Identifier[0] == '_')
        Identifier.erase(0, 1);
      for (char Ch : Identifier) {
        if (std::isalnum(static_cast<unsigned char>(Ch)) || Ch == '_')
          Name += Ch;
        else
          Name += '_';
      }
      if (Name.empty())
        Name = "g_";
      else if (std::isdigit(static_cast<unsigned char>(Name[0])))
        Name = "g_" + Name;
      if (std::string Comment =
              symbolComment(cNameOfGlobal(RawName, GV.isDeclaration()), Name);
          !Comment.empty())
        ImageObjectSymbols.try_emplace(Name, std::move(Comment));
    } else if (!GV.hasInitializer()) {
      llvm::StringRef Resolved = Name;
      if (Resolved.starts_with("__imp_") || Resolved.starts_with("_imp_") ||
          Resolved.starts_with("??") || Resolved.starts_with("ord_") ||
          Resolved.contains("??"))
        continue;
      if (auto Slot = parseNdDataSymbol(RawName); Slot && Img) {
        if (Img->findImportAt(*Slot))
          continue;
        auto *VTy = GV.getValueType();
        const uint16_t Size =
            VTy && VTy->isIntegerTy()
                ? static_cast<uint16_t>(VTy->getIntegerBitWidth() / 8)
                : 0;
        if (!ObservedImageObjects.count(*Slot) &&
            foldReadonlyScalar(*Slot, Size))
          continue;
      }
      auto *VTy = GV.getValueType();
      const char *CTy = "uint8_t";
      if (VTy->isIntegerTy(16))
        CTy = "uint16_t";
      else if (VTy->isIntegerTy(32))
        CTy = "uint32_t";
      else if (VTy->isIntegerTy(64))
        CTy = "uint64_t";
      if (!headerDeclaresObject(Name))
        OS << imageObjectSymbolLine(Name)
           << llvmc::storageAlignment(GV.getValueType(), Mod.getDataLayout(),
                                      GV.getAlign().valueOrOne())
           << "extern " << CTy << " " << Name << "; /* 0x"
           << llvm::utohexstr(*parseNdDataSymbol(RawName)) << " */\n";
      EmittedGlobalNames.insert(Name);
      continue;
    }

    if (!GV.hasInitializer() || sourceStringOnlyCalledByValue(GV))
      continue;

    // The image's storage stays the image's in one function's output, which
    // reaches its objects by their own names.
    if (OnlyFunction && mirrorsImageStorage(GV)) {
      OS << imageObjectSymbolLine(Name) << "extern "
         << (GV.isConstant() ? "const " : "") << "unsigned char " << Name
         << "[];\n";
      EmittedGlobalNames.insert(Name);
      continue;
    }

    auto *Init = GV.getInitializer();
    EmittedGlobalNames.insert(Name);
    // Every declaration below defines it.
    OS << imageObjectSymbolLine(Name);

    // Lifted writable image data can be one byte array with overlapping
    // integer views.  Keep the backing range as an array; printing it as a
    // pointer, or printing each offset as a separate C object, loses those
    // overlaps (including the two ten-byte x87 operands in FPREM samples).
    if (const auto *Array = llvm::dyn_cast<llvm::ArrayType>(GV.getValueType());
        parseNdDataSymbol(RawName) && Array &&
        Array->getElementType()->isIntegerTy(8) &&
        (llvm::isa<llvm::ConstantAggregateZero>(Init) ||
         llvm::isa<llvm::ConstantDataArray>(Init))) {
      writeImageByteArray(GV, Name);
      continue;
    }

    OS << llvmc::storageAlignment(GV.getValueType(), Mod.getDataLayout(),
                                  GV.getAlign().valueOrOne());
    if (auto *CDA = llvm::dyn_cast<llvm::ConstantDataArray>(Init)) {
      if (GV.isConstant() && CDA->isString()) {
        llvm::StringRef Raw = CDA->getAsString();
        while (!Raw.empty() && Raw.back() == '\0')
          Raw = Raw.drop_back();
        if (GV.hasLocalLinkage())
          OS << "static ";
        OS << "const char " << Name << "[" << CDA->getNumElements()
           << "] = " << wrapInitializer("\"" + escapeCString(Raw) + "\"")
           << ";\n";
        continue;
      }
    }

    // Storage arrays retain C array declarations; only SSA values need the
    // record carrier. GEPs and image byte ranges still address this storage.
    if (auto *Array = llvm::dyn_cast<llvm::ArrayType>(GV.getValueType())) {
      if (GV.hasLocalLinkage())
        OS << "static ";
      if (GV.isConstant())
        OS << "const ";
      OS << typeToCLLVM(Array->getElementType()) << " " << Name << "["
         << Array->getNumElements() << "] = {";
      std::string Elements;
      if (llvm::isa<llvm::ConstantAggregateZero>(Init))
        Elements = "0";
      else
        for (uint64_t I = 0; I < Array->getNumElements(); ++I) {
          if (I)
            Elements += ", ";
          Elements += constStr(Init->getAggregateElement(unsigned(I)));
        }
      OS << wrapInitializer(std::move(Elements)) << "};\n";
      continue;
    }

    if (GV.hasLocalLinkage())
      OS << "static ";
    if (GV.isConstant())
      OS << "const ";
    OS << typeToCLLVM(GV.getValueType()) << " " << Name;
    if (!llvm::isa<llvm::ConstantAggregateZero>(Init))
      OS << " = " << wrapInitializer(constStr(Init));
    OS << ";\n";
  }
}

void LLVMCWriter::writeReferencedImageObjects(
    llvm::ArrayRef<const llvm::Function *> Fns, llvm::StringRef Text) {
  if (Fns.empty())
    return;
  std::map<std::string, llvm::Type *> Objs;
  std::map<va_t, const llvm::GlobalVariable *> ByteArrays;
  // An object the function names keeps the type its accesses give it; one
  // whose address alone it takes is as wide as the listing names it.
  auto Declare = [&](const std::string &Name, llvm::Type *Ty, bool Widen) {
    llvm::StringRef Raw = Name;
    if (Raw.empty() || EmittedGlobalNames.count(Name) ||
        Raw.starts_with("__imp_") || Raw.starts_with("_imp_") ||
        Raw.starts_with("??") || Raw.starts_with("ord_") ||
        Raw.contains("??") || headerDeclaresObject(Raw))
      return;
    auto It = Objs.find(Name);
    if (It == Objs.end())
      Objs[Name] = Ty;
    else if (Widen && Ty->isIntegerTy() && It->second->isIntegerTy() &&
             Ty->getIntegerBitWidth() > It->second->getIntegerBitWidth())
      It->second = Ty;
  };
  auto AddressType = [&](va_t VA) {
    unsigned Bytes = 1;
    if (Opts.AccessWidths)
      if (const auto Width = Opts.AccessWidths->at(VA);
          Width && (*Width == 2 || *Width == 4 || *Width == 8))
        Bytes = *Width;
    return llvm::Type::getIntNTy(Fns.front()->getContext(), Bytes * 8);
  };
  auto Note = [&](const llvm::Value *Ptr, llvm::Type *Ty, bool MayFold) {
    if (!Ty)
      return;
    const uint64_t Size = imageIntegerAccessSize(Ty);
    if (auto Backing = imageByteArrayBacking(Ptr, Size)) {
      if (EmittedGlobalNames.count(constStr(Backing->first)))
        return;
      ByteArrays.emplace(*parseNdDataSymbol(Backing->first->getName()),
                         Backing->first);
      return;
    }
    std::string Name = imageDataCName(Ptr);
    if (Name.empty())
      return;
    if (auto VA = imageDataVA(Ptr)) {
      if (MayFold && foldReadonlyScalar(*VA, static_cast<uint16_t>(Size)))
        return;
    }
    Declare(Name, Ty, /*Widen=*/true);
  };
  for (const llvm::Function *Fn : Fns)
    for (const auto &BB : *Fn)
      for (const auto &Inst : BB) {
        if (auto *LI = llvm::dyn_cast<llvm::LoadInst>(&Inst))
          Note(LI->getPointerOperand(), LI->getType(), LI->isSimple());
        else if (auto *SI = llvm::dyn_cast<llvm::StoreInst>(&Inst))
          Note(SI->getPointerOperand(), SI->getValueOperand()->getType(),
               false);
      }
  // The objects whose addresses the function's constants spell and its text
  // names: a literal shows its text, and a constant the text folds away
  // names nothing.
  // The identifiers the text spells, read once: a whole program's text runs
  // to megabytes, and every address constant asks.
  std::optional<llvm::DenseSet<llvm::StringRef>> Identifiers;
  const auto Spelled = [&](llvm::StringRef Name) {
    if (!Identifiers) {
      Identifiers.emplace();
      const auto Part = [](char C) { return llvm::isAlnum(C) || C == '_'; };
      for (size_t At = 0; At < Text.size();) {
        if (!Part(Text[At])) {
          ++At;
          continue;
        }
        const size_t Begin = At;
        while (At < Text.size() && Part(Text[At]))
          ++At;
        Identifiers->insert(Text.slice(Begin, At));
      }
    }
    return Identifiers->contains(Name);
  };
  const auto DeclareAddressed = [&](va_t Addr, const std::string &Name) {
    if (!Name.empty() &&
        !stringLiteralAt(Img, Dbg, Addr, /*AllowEmpty=*/true) && Spelled(Name))
      Declare(Name, AddressType(Addr), /*Widen=*/false);
  };
  llvm::SmallPtrSet<const llvm::Constant *, 32> Seen;
  const auto NoteAddresses = [&](auto &&Self, const llvm::Value *V) -> void {
    const auto *C = llvm::dyn_cast<llvm::Constant>(V);
    if (!C || llvm::isa<llvm::Function>(C) || !Seen.insert(C).second)
      return;
    if (const auto *CI = llvm::dyn_cast<llvm::ConstantInt>(C)) {
      if (const auto Addr = codeSectionAddress(*CI);
          Addr && isCodeSectionData(Img, *Addr))
        DeclareAddressed(*Addr, namedImageObject(*Addr));
      return;
    }
    if (C->getType()->isPointerTy())
      if (const auto VA = imageDataVA(C)) {
        DeclareAddressed(*VA, imageDataCName(C));
        return;
      }
    // A global's operand is its initializer, which the function does not
    // spell.
    if (llvm::isa<llvm::ConstantExpr>(C))
      for (const llvm::Use &Op : C->operands())
        Self(Self, Op.get());
  };
  for (const llvm::Function *Fn : Fns)
    for (const auto &BB : *Fn)
      for (const auto &Inst : BB)
        for (const llvm::Use &Op : Inst.operands())
          NoteAddresses(NoteAddresses, Op.get());
  // A pointer mirror the environment holds (runtime-filled slots) is no
  // module's to define, but code that reaches it by offset still names it.
  bool Declared = false;
  if (!OnlyFunction && CurMod)
    for (const auto &Global : CurMod->globals())
      if (!Global.hasInitializer() && mirrorsImageStorage(Global))
        if (const std::string Name = globalNameStr(&Global);
            !EmittedGlobalNames.count(Name) && Spelled(Name)) {
          OS << "extern " << typeToCLLVM(Global.getValueType()) << " " << Name
             << ";\n";
          EmittedGlobalNames.insert(Name);
          Declared = true;
        }
  for (const auto &[Name, Ty] : Objs) {
    OS << imageObjectSymbolLine(Name) << "extern " << typeToCLLVM(Ty) << " "
       << Name << ";\n";
  }
  for (const auto &[Base, GV] : ByteArrays) {
    const std::string Name = namedImageObject(Base);
    OS << imageObjectSymbolLine(Name);
    writeImageByteArray(*GV, Name);
  }
  if (Declared || !Objs.empty() || !ByteArrays.empty())
    OS << "\n";
}

std::string LLVMCWriter::boundImportCallee(const llvm::CallBase &Call) const {
  if (Call.getCalledFunction() ||
      llvm::isa<llvm::InlineAsm>(Call.getCalledOperand()))
    return {};
  const llvm::Value *Slot = Call.getCalledOperand()->stripPointerCasts();
  if (const auto *Cast = llvm::dyn_cast<llvm::IntToPtrInst>(Slot))
    Slot = Cast->getOperand(0);
  const auto *Symbol = llvm::dyn_cast<llvm::GlobalVariable>(Slot);
  const bool CalledSymbol =
      Symbol && Symbol->isDeclaration() && data_symbol::isMarked(*Symbol);
  const auto *Load = llvm::dyn_cast<llvm::LoadInst>(Slot);
  const auto SlotVA =
      Load ? imageDataVA(Load->getPointerOperand()) : std::nullopt;
  if (!CalledSymbol &&
      (!Img || !SlotVA || !Img->ImportStorageSlots.count(*SlotVA)))
    return {};
  return resolveImportCalleeName(Call.getCalledOperand());
}

LLVMCWriter::LibraryCallee
LLVMCWriter::libraryCallee(const llvm::CallBase &Call) const {
  if (Opts.PreserveLLVMFunctionTypes)
    return {};
  if (const llvm::Function *Callee = Call.getCalledFunction()) {
    // The module's own definition is its code, and another routine it
    // declares is declared as its calls type it (writeForwardDecls), unless
    // a header declares it, as one an LLVM intrinsic prints as may be.
    if (!Callee->isDeclaration())
      return {};
    std::string Name;
    if (!Callee->isIntrinsic())
      Name = functionIdentifier(*Callee);
    else if (const char *Mapped =
                 llvmIntrinsicToCName(Callee->getName().str().c_str()))
      Name = Mapped;
    if (Name.empty())
      return {};
    if (libc::headerFor(Name))
      return {std::move(Name), nullptr};
    // A routine the runtime's tables type is declared from them, as its
    // import is (writeForwardDecls, writeImportCalleeDecls).
    if (!Callee->isIntrinsic())
      if (const libc::LibCPrototype *Prototype =
              libc::libcPrototype(Name, Opts.Format))
        return {std::move(Name), Prototype};
    return {};
  }
  if (llvm::isa<llvm::InlineAsm>(Call.getCalledOperand()))
    return {};
  // A routine called through its import has the declaration a direct call
  // to it would: its header's, or the runtime tables' where the unit
  // declares it from them.
  if (std::string Name = resolveImportCalleeName(Call.getCalledOperand());
      !Name.empty() && libc::headerFor(Name))
    return {std::move(Name), nullptr};
  std::string Name = boundImportCallee(Call);
  if (Name.empty() || FunctionIdentifierNames.count(Name))
    return {};
  if (const libc::LibCPrototype *Prototype =
          libc::libcPrototype(Name, Opts.Format))
    return {std::move(Name), Prototype};
  return {};
}

std::string LLVMCWriter::callResult(const llvm::CallBase &Call,
                                    std::string Expr) const {
  llvm::Type *Type = Call.getType();
  if (!Type->isIntegerTy() && !Type->isPointerTy() &&
      !Type->isFloatingPointTy())
    return Expr;
  // A callee that returns nothing leaves its result register undefined, of
  // which zero is a stable choice, as an undefined value's is.
  const std::string Undefined = "(" + Expr + ", (" + typeToCLLVM(Type) + ")0)";
  if (calleeReturnsNothing(Call))
    return Undefined;
  const LibraryCallee Library = libraryCallee(Call);
  if (!Library)
    return Expr;
  const std::optional<libc::LibCResult> Result =
      libc::libcResult(Library.Name, Opts.Format);
  if (!Result)
    return Expr;
  // C does not convert the pointer a routine returns to the integer the
  // machine holds it in.
  if (*Result == libc::LibCResult::Pointer && Type->isIntegerTy())
    return "((" + typeToCLLVM(Type) + ")(uintptr_t)" + Expr + ")";
  if (*Result == libc::LibCResult::Void)
    return Undefined;
  return Expr;
}

void LLVMCWriter::writeImportCalleeDecls(llvm::Module &Mod) {
  const c_library::ImportPops StdcallPops =
      Img ? c_library::stdcallImportPops(*Img) : c_library::ImportPops();
  // Each import with the type its calls return, and for a stdcall one the
  // types they pass; one whose calls disagree is left to them.
  struct ImportCalls {
    std::optional<std::string> Return;
    std::optional<std::vector<std::string>> Params;
  };
  std::map<std::string, ImportCalls> Imports;
  const llvm::DataLayout &Layout = Mod.getDataLayout();
  // Each routine the runtime's tables type, with their prototype.
  std::map<std::string, const libc::LibCPrototype *> Typed;
  // The imports a weak reference names, whose null address the code tests
  // (`__gmon_start__`).
  std::set<std::string> Weak;
  for (const llvm::Function &Fn : Mod) {
    if (Fn.isDeclaration() || (OnlyFunction && &Fn != OnlyFunction))
      continue;
    for (const llvm::BasicBlock &Block : Fn)
      for (const llvm::Instruction &Inst : Block) {
        const auto *Call = llvm::dyn_cast<llvm::CallBase>(&Inst);
        if (!Call)
          continue;
        // A call through a slot the loader binds names another module's
        // routine, in the import table or outside it (an ELF GOT entry), as
        // does a call to the symbol such a slot holds, which the slot's load
        // folds to.  One a header the unit includes declares keeps that
        // prototype, and one the runtime's tables type (RaiseException) takes
        // theirs (libraryCallee); any other, such as most of the Windows API,
        // is declared as the machine calls it.
        std::string Name = boundImportCallee(*Call);
        if (Name.empty() || FunctionIdentifierNames.count(Name) ||
            libc::isKnownFunction(Name))
          continue;
        if (const auto *Symbol = llvm::dyn_cast<llvm::GlobalValue>(
                Call->getCalledOperand()->stripPointerCasts());
            Symbol && Symbol->hasExternalWeakLinkage())
          Weak.insert(Name);
        if (const LibraryCallee Library = libraryCallee(*Call);
            Library.Prototype) {
          Typed.emplace(Library.Name, Library.Prototype);
          continue;
        }
        const std::string Return = typeToCLLVM(Call->getType());
        // A stdcall routine pops the arguments its calls push, which a C
        // compiler leaves to it only by its declaration: the call's, where
        // they fill the slots its name spells.
        std::optional<std::vector<std::string>> Params;
        if (const auto Pops = StdcallPops.find(Name);
            Pops != StdcallPops.end() && Pops->second) {
          uint64_t Pushed = 0;
          Params.emplace();
          for (const llvm::Use &Argument : Call->args()) {
            Pushed += llvm::alignTo(
                Layout.getTypeStoreSize(Argument->getType()).getFixedValue(),
                4);
            Params->push_back(typeToCLLVM(Argument->getType()));
          }
          if (Pushed != *Pops->second)
            Params.reset();
        }
        const auto [Import, Added] =
            Imports.try_emplace(Name, ImportCalls{Return, Params});
        if (Added)
          continue;
        if (Import->second.Return && *Import->second.Return != Return)
          Import->second.Return.reset();
        if (Import->second.Params != Params)
          Import->second.Params.reset();
      }
  }
  auto Extern = [&](const std::string &Name) {
    return Weak.count(Name) ? "extern __attribute__((weak)) " : "extern ";
  };
  for (const auto &[Name, Prototype] : Typed)
    OS << Extern(Name) << c_library::declaration(*Prototype, Name, Opts.TheArch)
       << ";\n";
  for (const auto &[Name, Calls] : Imports) {
    if (!Calls.Return)
      continue;
    // Unprototyped, the declaration takes the arguments the machine passed.
    if (!Calls.Params) {
      OS << Extern(Name) << *Calls.Return << " " << Name << "();\n";
      continue;
    }
    OS << Extern(Name) << "__attribute__((stdcall)) " << *Calls.Return << " "
       << Name << "(";
    for (size_t I = 0; I < Calls.Params->size(); ++I)
      OS << (I ? ", " : "") << (*Calls.Params)[I];
    OS << (Calls.Params->empty() ? "void" : "") << ");\n";
  }
}

void LLVMCWriter::collectImageDataUses(llvm::Module &Mod) {
  // Image data is named by its address, from the image or from the module's
  // data globals.
  ImageDataUses.clear();
  const llvm::DataLayout &Layout = Mod.getDataLayout();
  for (const llvm::Function &Fn : Mod)
    for (const llvm::BasicBlock &Block : Fn)
      for (const llvm::Instruction &Inst : Block) {
        const llvm::Value *Pointer = nullptr;
        llvm::Type *Type = nullptr;
        if (const auto *Load = llvm::dyn_cast<llvm::LoadInst>(&Inst)) {
          Pointer = Load->getPointerOperand();
          Type = Load->getType();
        } else if (const auto *Store = llvm::dyn_cast<llvm::StoreInst>(&Inst)) {
          Pointer = Store->getPointerOperand();
          Type = Store->getValueOperand()->getType();
        } else if (const auto *Call = llvm::dyn_cast<llvm::CallBase>(&Inst)) {
          // A call through a pointer the image holds.
          if (Call->getCalledFunction())
            continue;
          const llvm::Value *Callee =
              Call->getCalledOperand()->stripPointerCasts();
          if (const auto *Cast = llvm::dyn_cast<llvm::IntToPtrInst>(Callee))
            Callee = Cast->getOperand(0);
          if (const auto *Load = llvm::dyn_cast<llvm::LoadInst>(Callee))
            if (auto VA = imageDataVA(Load->getPointerOperand()))
              ImageDataUses[*VA].CallSlot = true;
          continue;
        }
        if (!Pointer || !Type || !Type->isSized())
          continue;
        if (auto VA = imageDataVA(Pointer))
          ImageDataUses[*VA].AccessBytes.insert(
              Layout.getTypeStoreSize(Type).getFixedValue());
      }
}

std::string LLVMCWriter::sourceScalarType(llvm::Type *T, bool Signed) const {
  if (Signed && (T->isIntegerTy(8) || T->isIntegerTy(16) ||
                 T->isIntegerTy(32) || T->isIntegerTy(64)))
    return "int" + std::to_string(T->getIntegerBitWidth()) + "_t";
  return typeToCLLVM(T);
}

std::string
LLVMCWriter::sourceFunctionDeclaration(const llvm::Function &Fn,
                                       llvm::StringRef Declarator,
                                       std::optional<unsigned> Params) const {
  auto *FT = Fn.getFunctionType();
  const unsigned Count =
      Params ? std::min(*Params, FT->getNumParams()) : FT->getNumParams();
  std::string Result =
      sourceScalarType(FT->getReturnType(),
                       Fn.hasFnAttribute(LLVMSourceSignedResult)) +
      " " + Declarator.str() + "(";
  if (Count == 0 && !FT->isVarArg()) {
    Result += "void";
  } else if (Count) {
    for (unsigned I = 0; I < Count; ++I) {
      if (I)
        Result += ", ";
      Result += sourceScalarType(
          FT->getParamType(I),
          Fn.getAttributes().hasParamAttr(I, LLVMSourceSignedParameter));
    }
    if (FT->isVarArg())
      Result += ", ...";
  }
  // ISO C before C23 cannot spell only `...`: preserve the existing
  // unprototyped projection rather than inventing a fixed argument.
  return Result + ")";
}

/// The stack bytes the first \p Params parameters of \p Fn (all without)
/// take on i386, each a multiple of four.
static uint64_t argumentSlotBytes(const llvm::Function &Fn,
                                  std::optional<unsigned> Params) {
  const llvm::DataLayout &Layout = Fn.getParent()->getDataLayout();
  const unsigned Count = std::min(Params.value_or(Fn.arg_size()),
                                  static_cast<unsigned>(Fn.arg_size()));
  uint64_t Bytes = 0;
  for (unsigned I = 0; I < Count; ++I)
    Bytes += llvm::alignTo(
        Layout.getTypeStoreSize(Fn.getFunctionType()->getParamType(I))
            .getFixedValue(),
        4);
  return Bytes;
}

void LLVMCWriter::writeForwardDecls(llvm::Module &Mod) {
  const c_library::ImportPops StdcallPops =
      Img ? c_library::stdcallImportPops(*Img) : c_library::ImportPops();
  for (auto &Fn : Mod) {
    if (OnlyFunction) {
      // Selected fragments need declarations for their referenced providers,
      // including scalar calls and definitions whose bodies are not emitted.
      if (!SelectedGlobals.count(&Fn))
        continue;
    }
    if (Fn.isIntrinsic())
      continue;
    if (llvmc::isCStackAllocationFunction(Fn))
      continue;
    // Every definition prints as code, one with Windows exception semantics
    // too (writeAnalysisOnlyFunction), so each is declared before the bodies
    // that call it; another module's routine is declared where a body or a
    // global names it.
    if (!OnlyFunction && Fn.isDeclaration() && !isReferencedByProjection(Fn))
      continue;

    std::string RawName = Fn.getName().str();

    if (IntrinsicMappedNames.count(RawName))
      continue;

    if (llvmIntrinsicToCName(RawName.c_str()))
      continue;
    if (isX86FastFailName(RawName))
      continue;

    std::string Name = functionIdentifier(Fn);

    if (!OnlyFunction && !Fn.isDeclaration()) {
      if (auto It = DefinitionDeclarations.find(&Fn);
          It != DefinitionDeclarations.end()) {
        OS << It->second << ";\n";
        continue;
      }
      if (!Opts.PreserveLLVMFunctionTypes)
        continue;
    }

    if (libc::isKnownFunction(Name))
      continue;
    // A routine the runtime's tables type takes their declaration, as its
    // calls take their conversions (libraryCallee).
    if (Fn.isDeclaration())
      if (const libc::LibCPrototype *Prototype =
              libc::libcPrototype(Name, Opts.Format)) {
        OS << (Fn.hasExternalWeakLinkage() ? "extern __attribute__((weak)) "
                                           : "extern ")
           << c_library::declaration(*Prototype, Name, Opts.TheArch) << ";\n";
        continue;
      }
    if (const MsvcCallee *Msvc = msvcCallee(Name, Opts.Format)) {
      OS << msvcSyntheticPrototype(Name, *Msvc,
                                   Opts.TheArch == Arch::X64 && Msvc->FastCall)
         << ";\n";
      continue;
    }

    // Calls the callee's prototype decides pass as many arguments as it
    // takes (printedCallArgLimit), which the declaration takes too.
    const std::optional<unsigned> Params = prototypedParamCount(Fn);
    // An i386 stdcall routine pops the arguments its calls push, which a C
    // compiler leaves to it only by its declaration: where its parameters
    // fill the slots its names spell (c_library::stdcallImportPops).
    if (Fn.isDeclaration())
      if (const auto Pops = StdcallPops.find(Name);
          Pops != StdcallPops.end() && Pops->second &&
          *Pops->second == argumentSlotBytes(Fn, Params))
        OS << "__attribute__((stdcall)) ";
    OS << sourceFunctionDeclaration(Fn, Name, Params);
    // An import whose identifier is not its symbol links by the symbol, which
    // a comment spells as its language does.
    std::string Comment;
    if (Fn.isDeclaration()) {
      const llvm::StringRef CName = cNameOfGlobal(RawName, true);
      if (linksByLabel(CName, Name)) {
        std::string Label;
        llvm::raw_string_ostream(Label).write_escaped(
            symbolOfCName(CName, Opts.Format, Opts.TheArch));
        OS << " __asm__(\"" << Label << "\")";
        // A name that reads as the label spells it needs no comment.
        if (const std::string Readable = demangledComment(CName);
            Opts.EmitComments && !Readable.empty() && Readable != CName)
          Comment = " /* " + Readable + " */";
      }
    }
    OS << ";" << Comment << "\n";
  }
}

//===----------------------------------------------------------------------===//
// Public API
//===----------------------------------------------------------------------===//

static void lowerCIntegerReductions(llvm::Module &Mod) {
  llvm::SmallVector<llvm::IntrinsicInst *> Work;
  for (auto &F : Mod)
    for (auto &BB : F)
      for (auto &I : BB)
        if (auto *Call = llvm::dyn_cast<llvm::IntrinsicInst>(&I))
          switch (Call->getIntrinsicID()) {
          case llvm::Intrinsic::vector_reduce_add:
          case llvm::Intrinsic::vector_reduce_mul:
          case llvm::Intrinsic::vector_reduce_and:
          case llvm::Intrinsic::vector_reduce_or:
          case llvm::Intrinsic::vector_reduce_xor:
            if (llvm::isa<llvm::FixedVectorType>(
                    Call->getArgOperand(0)->getType()))
              Work.push_back(Call);
            break;
          default:
            break;
          }
  for (auto *Call : Work) {
    llvm::IRBuilder<> B(Call);
    auto Id = Call->getIntrinsicID();
    auto *Vector = Call->getArgOperand(0);
    auto *Identity = llvm::getReductionIdentity(
        Id, Vector->getType()->getScalarType(), llvm::FastMathFlags{});
    auto *Result = llvm::getOrderedReduction(
        B, Identity, Vector, llvm::getArithmeticReductionInstruction(Id));
    Call->replaceAllUsesWith(Result);
    Call->eraseFromParent();
  }
}

// Scalarizer handles vector-to-vector casts, but not packing a vector into a
// scalar integer. Express the pack explicitly before scalarization so the C
// writer never sees an unassigned gather temporary.
static llvm::Value *balancedOr(llvm::IRBuilder<> &B,
                               llvm::SmallVector<llvm::Value *> Values) {
  while (Values.size() > 1) {
    llvm::SmallVector<llvm::Value *> Next;
    for (size_t I = 0; I < Values.size(); I += 2)
      Next.push_back(I + 1 < Values.size()
                         ? B.CreateOr(Values[I], Values[I + 1])
                         : Values[I]);
    Values = std::move(Next);
  }
  return Values.front();
}

static void lowerPackedVectorBitcasts(llvm::Module &Mod) {
  llvm::SmallVector<llvm::BitCastInst *> Work;
  for (auto &F : Mod)
    for (auto &BB : F)
      for (auto &I : BB)
        if (auto *Cast = llvm::dyn_cast<llvm::BitCastInst>(&I))
          Work.push_back(Cast);
  for (auto *Cast : Work) {
    auto *Vector = llvm::dyn_cast<llvm::FixedVectorType>(Cast->getSrcTy());
    const bool Pack = Vector != nullptr;
    auto *Scalar = Pack ? Cast->getDestTy() : Cast->getSrcTy();
    if (!Pack)
      Vector = llvm::dyn_cast<llvm::FixedVectorType>(Cast->getDestTy());
    auto IsLane = [](llvm::Type *Type) {
      return Type->isIntegerTy() || Type->isHalfTy() || Type->isFloatTy() ||
             Type->isDoubleTy() || Type->isBFloatTy();
    };
    if (!Vector || !IsLane(Scalar) || !IsLane(Vector->getElementType()))
      continue;
    const unsigned ScalarBits = Scalar->getPrimitiveSizeInBits();
    const unsigned Count = Vector->getNumElements();
    const unsigned Bits = Vector->getElementType()->getPrimitiveSizeInBits();
    if (ScalarBits > 512 || uint64_t(Count) * Bits != ScalarBits)
      continue;
    auto *Integer = llvm::IntegerType::get(Mod.getContext(), ScalarBits);
    auto *LaneInteger = llvm::IntegerType::get(Mod.getContext(), Bits);
    llvm::IRBuilder<> B(Cast);
    auto *Input = Cast->getOperand(0);
    if (!Pack && !Scalar->isIntegerTy())
      Input = B.CreateBitCast(Input, Integer);
    llvm::SmallVector<llvm::Value *> Parts;
    llvm::Value *Value =
        Pack ? static_cast<llvm::Value *>(llvm::ConstantInt::get(Integer, 0))
             : llvm::PoisonValue::get(Vector);
    for (unsigned Lane = 0; Lane < Count; ++Lane) {
      unsigned Shift =
          (Mod.getDataLayout().isLittleEndian() ? Lane : Count - Lane - 1) *
          Bits;
      if (Pack) {
        auto *Part = B.CreateExtractElement(Input, Lane);
        if (!Part->getType()->isIntegerTy())
          Part = B.CreateBitCast(Part, LaneInteger);
        Part = B.CreateZExtOrTrunc(Part, Integer);
        if (Shift)
          Part = B.CreateShl(Part, llvm::ConstantInt::get(Integer, Shift));
        Parts.push_back(Part);
      } else {
        auto *Part = Input;
        if (Shift)
          Part = B.CreateLShr(Part, llvm::ConstantInt::get(Integer, Shift));
        Part = B.CreateZExtOrTrunc(Part, LaneInteger);
        if (!Vector->getElementType()->isIntegerTy())
          Part = B.CreateBitCast(Part, Vector->getElementType());
        Value = B.CreateInsertElement(Value, Part, Lane);
      }
    }
    if (Pack) {
      Value = balancedOr(B, std::move(Parts));
      if (!Scalar->isIntegerTy())
        Value = B.CreateBitCast(Value, Scalar);
    }
    Cast->replaceAllUsesWith(Value);
    Cast->eraseFromParent();
  }
}

static bool containsVectorType(llvm::Type *Type,
                               llvm::SmallPtrSetImpl<llvm::Type *> &Seen) {
  if (!Seen.insert(Type).second)
    return false;
  if (Type->isVectorTy())
    return true;
  if (auto *Array = llvm::dyn_cast<llvm::ArrayType>(Type))
    return containsVectorType(Array->getElementType(), Seen);
  if (auto *Struct = llvm::dyn_cast<llvm::StructType>(Type)) {
    if (Struct->isOpaque())
      return false;
    for (auto *Element : Struct->elements())
      if (containsVectorType(Element, Seen))
        return true;
  }
  if (auto *Function = llvm::dyn_cast<llvm::FunctionType>(Type)) {
    if (containsVectorType(Function->getReturnType(), Seen))
      return true;
    for (auto *Parameter : Function->params())
      if (containsVectorType(Parameter, Seen))
        return true;
  }
  return false;
}

static bool containsVectorType(llvm::Type *Type) {
  llvm::SmallPtrSet<llvm::Type *, 16> Seen;
  return containsVectorType(Type, Seen);
}

static bool referencesVectorGlobal(const llvm::Function &Function) {
  llvm::SmallVector<const llvm::Value *, 32> Work;
  llvm::SmallPtrSet<const llvm::Value *, 32> Seen;
  for (const auto &Block : Function)
    for (const auto &Instruction : Block)
      Work.push_back(&Instruction);
  while (!Work.empty()) {
    const auto *Value = Work.pop_back_val();
    if (!Seen.insert(Value).second || llvm::isa<llvm::Function>(Value))
      continue;
    if (const auto *Global = llvm::dyn_cast<llvm::GlobalVariable>(Value))
      if (containsVectorType(Global->getValueType()))
        return true;
    // Follow constant GEPs, casts, aliases and pointer-valued initializers as
    // well as direct uses; opaque pointers do not reveal the storage type.
    if (const auto *User = llvm::dyn_cast<llvm::User>(Value))
      for (const auto &Operand : User->operands())
        Work.push_back(Operand.get());
  }
  return false;
}

static bool isCVectorBoundaryType(llvm::Type *Type) {
  return isCIntegerVectorType(Type) || !containsVectorType(Type);
}

static bool isCVectorBoundarySignature(const llvm::FunctionType *Type) {
  return isCVectorBoundaryType(Type->getReturnType()) &&
         llvm::all_of(Type->params(), isCVectorBoundaryType);
}

static bool isCVectorBoundaryInstruction(const llvm::Instruction &Inst,
                                         Arch TheArch) {
  if (const auto *Call = llvm::dyn_cast<llvm::CallInst>(&Inst))
    if (const auto Shape = classifyX86FPStateAsm(*Call);
        Shape && isX86FPNumericalStateIntrinsic(Shape->first))
      return TheArch == Arch::X86 || TheArch == Arch::X64;
  auto IsLocalType = [](llvm::Type *Type) {
    return isCVectorType(Type) || !containsVectorType(Type);
  };
  if (!IsLocalType(Inst.getType()) ||
      !llvm::all_of(Inst.operands(), [&](const llvm::Use &Operand) {
        return IsLocalType(Operand->getType());
      }))
    return false;
  // The scalarizer retains gathers/scatters at function boundaries. C vector
  // types preserve those lanes and their calling convention; arithmetic and
  // memory operations still need the existing explicit scalar lowering.
  if (llvm::isa<llvm::ExtractElementInst, llvm::InsertElementInst,
                llvm::ReturnInst, llvm::PHINode>(Inst))
    return true;
  if (const auto *Select = llvm::dyn_cast<llvm::SelectInst>(&Inst))
    return Select->getCondition()->getType()->isIntegerTy(1);
  if (const auto *Call = llvm::dyn_cast<llvm::CallBase>(&Inst)) {
    const auto *Callee = Call->getCalledFunction();
    if (LLVMCWriter::isNativeVectorIntrinsic(*Call, TheArch))
      return true;
    return (!Callee || !Callee->isIntrinsic()) &&
           isCVectorBoundarySignature(Call->getFunctionType());
  }
  return false;
}

static bool containsArrayValue(llvm::Type *Ty) {
  if (Ty->isArrayTy())
    return true;
  if (auto *ST = llvm::dyn_cast<llvm::StructType>(Ty); ST && !ST->isOpaque())
    return llvm::any_of(ST->elements(), containsArrayValue);
  return false;
}

static void validateArrayValueBoundaries(const llvm::Module &Mod,
                                         const llvm::Function *Only) {
  const auto &Target = Mod.getTargetTriple();
  // A C record is a value carrier, but that alone does not authenticate its
  // ABI against a raw LLVM aggregate. Admit the word pairs for which both
  // lower to one/two integer argument/result registers. Larger, narrow-lane,
  // nested and Windows aggregates need a separate ABI lowering contract.
  const bool WordABI =
      (Target.isOSLinux() || Target.isOSDarwin()) &&
      (Target.isAArch64() || Target.getArch() == llvm::Triple::x86_64) &&
      Mod.getDataLayout().getPointerSizeInBits() == 64;
  auto CheckType = [&](llvm::Type *Ty) {
    if (!containsArrayValue(Ty))
      return;
    const auto *Array = llvm::dyn_cast<llvm::ArrayType>(Ty);
    if (!WordABI || !Array || !Array->getElementType()->isIntegerTy(64) ||
        Array->getNumElements() < 1 || Array->getNumElements() > 2)
      throw std::runtime_error(
          "C projection has an unsupported array value ABI");
  };
  auto CheckSignature = [&](llvm::FunctionType *Ty, unsigned CC,
                            const llvm::AttributeList &Attributes) {
    bool HasArray = containsArrayValue(Ty->getReturnType());
    bool HasArrayParameter = false;
    unsigned ArgumentWords = 0;
    bool WordParameters = true;
    CheckType(Ty->getReturnType());
    for (auto *Param : Ty->params()) {
      HasArray |= containsArrayValue(Param);
      HasArrayParameter |= containsArrayValue(Param);
      CheckType(Param);
      if (auto *Array = llvm::dyn_cast<llvm::ArrayType>(Param))
        ArgumentWords += Array->getNumElements();
      else if (Param->isIntegerTy(64) ||
               (Param->isPointerTy() && Param->getPointerAddressSpace() == 0))
        ++ArgumentWords;
      else
        WordParameters = false;
    }
    if (HasArray && CC != llvm::CallingConv::C)
      throw std::runtime_error(
          "C projection has an unsupported array calling convention");
    if (HasArray)
      for (auto Set : Attributes)
        for (auto Kind :
             {llvm::Attribute::ByVal, llvm::Attribute::ByRef,
              llvm::Attribute::InAlloca, llvm::Attribute::Preallocated,
              llvm::Attribute::StructRet, llvm::Attribute::InReg,
              llvm::Attribute::Nest, llvm::Attribute::SwiftSelf,
              llvm::Attribute::SwiftError})
          if (Set.hasAttribute(Kind))
            throw std::runtime_error(
                "C projection has unsupported array ABI attributes");
    // Once registers run out, C may spill a whole record while LLVM splits
    // the raw aggregate. Do not infer that stack-argument ABI from its type.
    const unsigned RegisterWords = Target.isAArch64() ? 8 : 6;
    if (HasArrayParameter &&
        (!WordParameters || ArgumentWords > RegisterWords || Ty->isVarArg()))
      throw std::runtime_error(
          "C projection has an unsupported array argument ABI");
  };
  for (const auto &Fn : Mod) {
    if (Only && &Fn != Only)
      continue;
    CheckSignature(Fn.getFunctionType(), Fn.getCallingConv(),
                   Fn.getAttributes());
    for (const auto &BB : Fn)
      for (const auto &Inst : BB)
        if (const auto *Call = llvm::dyn_cast<llvm::CallBase>(&Inst)) {
          CheckSignature(Call->getFunctionType(), Call->getCallingConv(),
                         Call->getAttributes());
          for (unsigned I = Call->getFunctionType()->getNumParams();
               I < Call->arg_size(); ++I)
            if (containsArrayValue(Call->getArgOperand(I)->getType()))
              throw std::runtime_error(
                  "C projection has an unsupported variadic array value");
        }
  }
}

// Keep call-result register homes in SSA on the private scalar projection.
// Repeating after promotion also exposes narrow views of the same result;
// LLVM owns the dominance/initialization proof for every promoted load.
static void promoteCallResultHomes(llvm::Function &Function) {
  llvm::DominatorTree Dominators(Function);
  for (unsigned Round = 0; Round != 8; ++Round) {
    llvm::SmallVector<llvm::AllocaInst *, 8> Homes;
    for (auto &Instruction : Function.getEntryBlock()) {
      auto *Home = llvm::dyn_cast<llvm::AllocaInst>(&Instruction);
      if (!Home || !llvm::isAllocaPromotable(Home))
        continue;
      const llvm::StoreInst *Definition = nullptr;
      bool Multiple = false;
      bool Read = false;
      for (auto *User : Home->users())
        if (const auto *Store = llvm::dyn_cast<llvm::StoreInst>(User)) {
          Multiple |= Definition != nullptr;
          Definition = Store;
        } else {
          Read |= llvm::isa<llvm::LoadInst>(User);
        }
      if (!Read || Multiple || !Definition)
        continue;
      const llvm::Value *Value = Definition->getValueOperand();
      unsigned Depth = 0;
      while (const auto *Cast = llvm::dyn_cast<llvm::CastInst>(Value)) {
        if (++Depth > 8)
          break;
        Value = Cast->getOperand(0);
      }
      if (llvm::isa<llvm::CallInst>(Value))
        Homes.push_back(Home);
    }
    if (Homes.empty())
      break;
    llvm::PromoteMemToReg(Homes, Dominators);
  }
}

bool LLVMCEmitter::emit(llvm::Module &Mod, llvm::raw_ostream &Out,
                        const CEmitterOptions &Opts, DebugContext *Dbg,
                        const BinaryImage *Img, const llvm::Function *Only) {
  if (Only && referencesVectorGlobal(*Only))
    throw std::runtime_error(
        "C projection references unsupported vector global storage");
  bool HasVectors = false;
  if (!Only)
    for (const auto &Global : Mod.globals())
      HasVectors |= containsVectorType(Global.getValueType());
  for (const auto &Function : Mod) {
    if (Only && &Function != Only)
      continue;
    HasVectors |= containsVectorType(Function.getFunctionType());
    for (const auto &Block : Function)
      for (const auto &Instruction : Block) {
        // Validate explicitly supported scalar shapes before normalization can
        // discard an unused malformed call, retaining the source error
        // contract.
        if (const auto *Call = llvm::dyn_cast<llvm::CallBase>(&Instruction)) {
          (void)scalarIntegerMinMax(*Call);
          (void)scalarIntegerSaturation(*Call);
          (void)scalarUnary(*Call);
        }
        if (const auto *Allocation =
                llvm::dyn_cast<llvm::AllocaInst>(&Instruction))
          HasVectors |= containsVectorType(Allocation->getAllocatedType());
        HasVectors |= Instruction.getType()->isVectorTy();
        for (const auto &Operand : Instruction.operands())
          HasVectors |= Operand->getType()->isVectorTy();
      }
  }
  if (llvm::verifyModule(Mod, &llvm::errs()))
    return false;
  validateArrayValueBoundaries(Mod, Only);
  llvmc::validateCStackAllocations(Mod, Only);
  // Source normalization must not mutate the caller's IR. Remove dead
  // computations left by recovered control flow even when semantic
  // optimization is disabled; they can otherwise reference unneeded image
  // tables. LLVM DCE retains volatile/atomic accesses and side-effecting calls.
  llvm::ValueToValueMapTy ValueMap;
  auto Projection = llvm::CloneModule(Mod, ValueMap);
  // Normalization can remove an otherwise unused frame_end/rsp_init marker.
  // Capture its display context on the private allocation before DCE so
  // debug stack offsets still name the same bytes after normalization.
  for (auto &Function : *Projection)
    for (auto &Block : Function)
      for (auto &Instruction : Block)
        if (auto *Frame = llvm::dyn_cast<llvm::AllocaInst>(&Instruction))
          if (auto Base = llvmc::syntheticFrameBaseOffset(
                  *Frame, Projection->getDataLayout()))
            Frame->setMetadata(
                llvmc::CapturedFrameBase,
                llvm::MDNode::get(
                    Mod.getContext(),
                    llvm::ConstantAsMetadata::get(llvm::ConstantInt::get(
                        llvm::Type::getInt64Ty(Mod.getContext()), *Base))));
  std::optional<LLVMSourceMap> Sources;
  if (Opts.SourceMap && Opts.SourceMap->LLVMSources) {
    Sources = *Opts.SourceMap->LLVMSources;
    Sources->remap([&](llvm::Value *Value) { return ValueMap.lookup(Value); });
  }
  const llvm::Function *ProjectionOnly = nullptr;
  if (Only) {
    ProjectionOnly =
        llvm::dyn_cast_or_null<llvm::Function>(ValueMap.lookup(Only));
    if (!ProjectionOnly)
      throw std::runtime_error(
          "C projection could not map the selected function into its clone");
  }
  if (HasVectors) {
    lowerCIntegerReductions(*Projection);
    lowerPackedVectorBitcasts(*Projection);
  }
  llvmc::lowerCStackAllocations(*Projection, ProjectionOnly);
  // Native image/debug projections keep their separate source contracts.
  // Pure scalar loop recovery proves its exact replacement before publishing
  // into this private clone and retains the existing path on refusal.
  if (!Dbg && !Img && !Opts.Image)
    llvmc::recoverScalarLoops(*Projection, ProjectionOnly);
  if (!Dbg && !Img && !Opts.Image)
    for (auto &Function : *Projection)
      if (!Function.isDeclaration() &&
          (!ProjectionOnly || &Function == ProjectionOnly))
        promoteCallResultHomes(Function);
  for (auto &Function : *Projection)
    if (!ProjectionOnly || &Function == ProjectionOnly) {
      llvmc::simplifyBitTests(Function);
      llvmc::factorCommonBranchTests(Function);
      llvmc::factorLoopPhiExpressions(Function);
      llvmc::distributeReturnEdges(Function);
    }
  llvm::LoopAnalysisManager Loops;
  llvm::FunctionAnalysisManager Functions;
  llvm::CGSCCAnalysisManager CallGraph;
  llvm::ModuleAnalysisManager Modules;
  llvm::PassBuilder Passes;
  Passes.registerModuleAnalyses(Modules);
  Passes.registerCGSCCAnalyses(CallGraph);
  Passes.registerFunctionAnalyses(Functions);
  Passes.registerLoopAnalyses(Loops);
  Passes.crossRegisterProxies(Loops, Functions, CallGraph, Modules);
  llvm::FunctionPassManager Normalize;
  if (HasVectors) {
    llvm::ScalarizerPassOptions ScalarOptions;
    // LLVM only splits simple accesses: volatile and atomic vectors still
    // reach the unsupported-instruction guard below.
    ScalarOptions.ScalarizeLoadStore = true;
    Normalize.addPass(llvm::ScalarizerPass(ScalarOptions));
  }
  Normalize.addPass(llvm::DCEPass());
  llvm::ModulePassManager Pipeline;
  Pipeline.addPass(
      llvm::createModuleToFunctionPassAdaptor(std::move(Normalize)));
  Pipeline.run(*Projection, Modules);
  if (HasVectors) {
    // Lane-width-changing vector casts create new scalar/vector bitcasts in
    // Scalarizer. Lower those with the source layout before scalarizing their
    // explicit lane gathers, too. The second pass introduces no new casts.
    lowerPackedVectorBitcasts(*Projection);
    Modules.invalidate(*Projection, llvm::PreservedAnalyses::none());
    Pipeline.run(*Projection, Modules);
  }
  if (llvm::verifyModule(*Projection, &llvm::errs()))
    return false;
  if (HasVectors) {
    if (!ProjectionOnly)
      for (const auto &Global : Projection->globals())
        if (containsVectorType(Global.getValueType()))
          throw std::runtime_error(
              "C projection retains an unsupported vector global");
    for (const auto &F : *Projection) {
      if (ProjectionOnly && &F != ProjectionOnly)
        continue;
      if (!ProjectionOnly && F.isDeclaration() && F.isIntrinsic())
        continue;
      if (!isCVectorBoundarySignature(F.getFunctionType()))
        throw std::runtime_error(
            "C projection retains an unsupported vector signature");
      for (const auto &BB : F)
        for (const auto &I : BB) {
          if (const auto *Allocation = llvm::dyn_cast<llvm::AllocaInst>(&I))
            if (containsVectorType(Allocation->getAllocatedType()))
              throw std::runtime_error(
                  "C projection retains unsupported vector local storage");
          if ((containsVectorType(I.getType()) ||
               llvm::any_of(I.operands(),
                            [](const llvm::Use &Operand) {
                              return containsVectorType(Operand->getType());
                            })) &&
              !isCVectorBoundaryInstruction(I, Opts.TheArch)) {
            std::string Detail;
            llvm::raw_string_ostream Stream(Detail);
            Stream
                << "C projection retains an unsupported vector instruction in "
                << F.getName() << ": " << I;
            throw std::runtime_error(Stream.str());
          }
        }
    }
  }
  if (!Opts.SourceMap) {
    LLVMCWriter W(Out, Opts, Dbg, Img);
    W.writeModule(*Projection, ProjectionOnly);
    return true;
  }
  std::string Ordinary, Annotated;
  llvm::raw_string_ostream OrdinaryOS(Ordinary), AnnotatedOS(Annotated);
  LLVMCWriter Plain(OrdinaryOS, Opts, Dbg, Img);
  Plain.writeModule(*Projection, ProjectionOnly);
  CSourceRecorder Recorder(*Opts.SourceMap, Ordinary);
  try {
    if (Sources)
      Recorder.prepareLLVMSources(*Sources);
    LLVMCWriter Marked(AnnotatedOS, Opts, Dbg, Img, true, &Recorder);
    Marked.writeModule(*Projection, ProjectionOnly);
    Recorder.finish(Annotated, Ordinary);
  } catch (const std::exception &) {
    // A mapping failure leaves the successful ordinary source intact, with
    // unknown spans. Errors in the ordinary emission still propagate.
  }
  CSourceRecorder Instructions(*Opts.SourceMap, Ordinary,
                               CSourceRecorder::Recording::Instructions);
  try {
    if (Sources)
      Instructions.prepareLLVMSources(*Sources);
    std::string Annotated;
    llvm::raw_string_ostream OS(Annotated);
    LLVMCWriter Marked(OS, Opts, Dbg, Img, true, &Instructions);
    Marked.writeModule(*Projection, ProjectionOnly);
    Instructions.finish(Annotated, Ordinary);
  } catch (const std::exception &) {
    // Navigation evidence does not authorize changes to source/library mapping.
  }
  Out << Ordinary;
  return true;
}

bool LLVMCEmitter::emitToFile(llvm::Module &Mod, const std::string &Path,
                              const CEmitterOptions &Opts, DebugContext *Dbg,
                              const BinaryImage *Img) {
  std::error_code EC;
  llvm::raw_fd_ostream OS(Path, EC);
  if (EC) {
    llvm::WithColor::error() << "llvm_c_emitter: cannot open " << Path << ": "
                             << EC.message() << "\n";
    return false;
  }
  bool Ok = emit(Mod, OS, Opts, Dbg, Img);
  LLVM_DEBUG(llvm::dbgs() << "llvm_c_emitter: written to " << Path << "\n");
  return Ok;
}

} // namespace neverd
