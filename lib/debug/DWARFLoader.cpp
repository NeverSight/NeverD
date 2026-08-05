//===- DWARFLoader.cpp - DWARF debug info loader -------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// DWARF debug information loading implementation.
///
//===----------------------------------------------------------------------===//

#include "neverd/debug/DWARFLoader.h"

#define DEBUG_TYPE "neverd-dwarf-loader"
#include "llvm/DebugInfo/DWARF/DWARFContext.h"
#include "llvm/DebugInfo/DWARF/DWARFDie.h"
#include "llvm/DebugInfo/DWARF/DWARFFormValue.h"
#include "llvm/DebugInfo/DWARF/DWARFUnit.h"
#include "llvm/Object/ELFObjectFile.h"
#include "llvm/Object/MachO.h"
#include "llvm/Object/MachOUniversal.h"
#include "llvm/Object/ObjectFile.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/LEB128.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/WithColor.h"
#include "llvm/Support/raw_ostream.h"

#include <algorithm>

namespace neverd {

struct DWARFDebugContext::Impl {
  std::unique_ptr<llvm::MemoryBuffer> BinaryBuf;
  std::unique_ptr<llvm::MemoryBuffer> DSYMBuf;
  std::unique_ptr<llvm::DWARFContext> DwarfCtx;

  std::map<va_t, FunctionSym> FuncMap;
  std::vector<FunctionSym> FuncList;

  struct LocalInfo {
    std::map<int64_t, VariableSym> ByOffset;
  };
  std::map<va_t, LocalInfo> LocalsMap;

  bool Loaded = false;

  TypeRef convertDwarfType(llvm::DWARFDie Die, int Depth = 0);
  void parseCompileUnits();
  void parseFunction(llvm::DWARFDie Die, llvm::DWARFUnit *Unit);
  void parseVariable(llvm::DWARFDie Die, va_t FuncAddr, bool IsParam);
};

DWARFDebugContext::~DWARFDebugContext() = default;

static std::filesystem::path findDSYM(const std::filesystem::path &BinPath) {
  auto DSYMBundle = BinPath;
  DSYMBundle += ".dSYM";
  auto DwarfDir = DSYMBundle / "Contents" / "Resources" / "DWARF";
  if (std::filesystem::exists(DwarfDir)) {
    for (auto &Entry : std::filesystem::directory_iterator(DwarfDir)) {
      if (Entry.is_regular_file())
        return Entry.path();
    }
  }

  auto Parent = BinPath.parent_path();
  if (Parent.empty())
    Parent = ".";
  auto Stem = BinPath.stem().string();
  if (!std::filesystem::exists(Parent))
    return {};
  for (auto &Entry : std::filesystem::directory_iterator(Parent)) {
    if (Entry.path().extension() == ".dSYM" && Entry.is_directory()) {
      auto DSYMStem = Entry.path().stem().string();
      if (DSYMStem != Stem)
        continue;
      auto Inner = Entry.path() / "Contents" / "Resources" / "DWARF";
      if (std::filesystem::exists(Inner)) {
        for (auto &F : std::filesystem::directory_iterator(Inner))
          if (F.is_regular_file())
            return F.path();
      }
    }
  }
  return {};
}

static std::unique_ptr<llvm::object::ObjectFile>
getObjectFromBuffer(llvm::MemoryBuffer &Buf) {
  auto BinOr =
      llvm::object::ObjectFile::createObjectFile(Buf.getMemBufferRef());
  if (!BinOr) {
    llvm::consumeError(BinOr.takeError());

    auto UniOr =
        llvm::object::MachOUniversalBinary::create(Buf.getMemBufferRef());
    if (!UniOr) {
      llvm::consumeError(UniOr.takeError());
      return nullptr;
    }
    auto &Uni = *UniOr;
    for (auto &ObjForArch : Uni->objects()) {
      auto ObjOr = ObjForArch.getAsObjectFile();
      if (ObjOr)
        return std::move(*ObjOr);
      llvm::consumeError(ObjOr.takeError());
    }
    return nullptr;
  }
  return std::move(*BinOr);
}

std::unique_ptr<DWARFDebugContext>
DWARFDebugContext::load(const std::filesystem::path &BinaryPath,
                        BinaryFormat Format) {
  auto Ctx = std::unique_ptr<DWARFDebugContext>(new DWARFDebugContext());
  Ctx->PImpl = std::make_unique<Impl>();

  auto BufOr = llvm::MemoryBuffer::getFile(BinaryPath.string());
  if (!BufOr) {
    llvm::WithColor::warning()
        << "debug: cannot open " << BinaryPath.string() << "\n";
    return Ctx;
  }
  Ctx->PImpl->BinaryBuf = std::move(*BufOr);

  auto Obj = getObjectFromBuffer(*Ctx->PImpl->BinaryBuf);
  if (!Obj) {
    llvm::WithColor::warning()
        << "debug: cannot parse object file " << BinaryPath.string() << "\n";
    return Ctx;
  }

  Ctx->PImpl->DwarfCtx = llvm::DWARFContext::create(*Obj);
  if (!Ctx->PImpl->DwarfCtx) {
    llvm::WithColor::warning() << "debug: cannot create DWARFContext for "
                               << BinaryPath.string() << "\n";
    return Ctx;
  }

  bool HasDwarf = Ctx->PImpl->DwarfCtx->getNumCompileUnits() > 0;

  if (!HasDwarf && Format == BinaryFormat::MachO) {
    auto DSYMPath = findDSYM(BinaryPath);
    if (!DSYMPath.empty()) {
      LLVM_DEBUG(llvm::dbgs()
                 << "debug: loading dSYM from " << DSYMPath.string() << "\n");
      auto DSYMBufOr = llvm::MemoryBuffer::getFile(DSYMPath.string());
      if (DSYMBufOr) {
        Ctx->PImpl->DSYMBuf = std::move(*DSYMBufOr);
        auto DSYMObj = getObjectFromBuffer(*Ctx->PImpl->DSYMBuf);
        if (DSYMObj) {
          Ctx->PImpl->DwarfCtx = llvm::DWARFContext::create(*DSYMObj);
          HasDwarf = Ctx->PImpl->DwarfCtx &&
                     Ctx->PImpl->DwarfCtx->getNumCompileUnits() > 0;
        }
      }
    }
  }

  if (!HasDwarf) {
    LLVM_DEBUG(llvm::dbgs() << "debug: no DWARF info found for "
                            << BinaryPath.string() << "\n");
    return Ctx;
  }

  LLVM_DEBUG(llvm::dbgs() << "debug: "
                          << Ctx->PImpl->DwarfCtx->getNumCompileUnits()
                          << " compile units found" << "\n");
  Ctx->PImpl->parseCompileUnits();
  Ctx->PImpl->Loaded = true;

  LLVM_DEBUG(llvm::dbgs() << "debug: loaded " << Ctx->PImpl->FuncList.size()
                          << " functions with debug info" << "\n");
  return Ctx;
}

TypeRef DWARFDebugContext::Impl::convertDwarfType(llvm::DWARFDie Die,
                                                  int Depth) {
  if (!Die.isValid() || Depth > 16)
    return NdType::makeInt(4);

  auto Tag = Die.getTag();
  switch (Tag) {
  case llvm::dwarf::DW_TAG_base_type: {
    auto SizeAttr = Die.find(llvm::dwarf::DW_AT_byte_size);
    auto Encoding = Die.find(llvm::dwarf::DW_AT_encoding);
    uint16_t Sz = SizeAttr ? static_cast<uint16_t>(
                                 SizeAttr->getAsUnsignedConstant().value_or(4))
                           : 4;
    if (Encoding) {
      auto Enc = Encoding->getAsUnsignedConstant().value_or(0);
      if (Enc == llvm::dwarf::DW_ATE_float)
        return NdType::makeFloat(Sz);
      bool IsSigned = (Enc == llvm::dwarf::DW_ATE_signed ||
                       Enc == llvm::dwarf::DW_ATE_signed_char);
      return NdType::makeInt(Sz, IsSigned);
    }
    return NdType::makeInt(Sz);
  }
  case llvm::dwarf::DW_TAG_pointer_type: {
    auto PointeeDie =
        Die.getAttributeValueAsReferencedDie(llvm::dwarf::DW_AT_type);
    auto PT = PointeeDie.isValid() ? convertDwarfType(PointeeDie, Depth + 1)
                                   : NdType::makeVoid();
    return NdType::makePtr(PT);
  }
  case llvm::dwarf::DW_TAG_typedef:
  case llvm::dwarf::DW_TAG_const_type:
  case llvm::dwarf::DW_TAG_volatile_type:
  case llvm::dwarf::DW_TAG_restrict_type: {
    auto Ref = Die.getAttributeValueAsReferencedDie(llvm::dwarf::DW_AT_type);
    return Ref.isValid() ? convertDwarfType(Ref, Depth + 1)
                         : NdType::makeVoid();
  }
  case llvm::dwarf::DW_TAG_array_type: {
    auto ElemDie =
        Die.getAttributeValueAsReferencedDie(llvm::dwarf::DW_AT_type);
    auto Elem = ElemDie.isValid() ? convertDwarfType(ElemDie, Depth + 1)
                                  : NdType::makeInt(1, false);
    uint32_t Count = 0;
    for (auto Child : Die.children()) {
      if (Child.getTag() == llvm::dwarf::DW_TAG_subrange_type) {
        auto UB = Child.find(llvm::dwarf::DW_AT_upper_bound);
        auto Cnt = Child.find(llvm::dwarf::DW_AT_count);
        if (Cnt)
          Count =
              static_cast<uint32_t>(Cnt->getAsUnsignedConstant().value_or(0));
        else if (UB)
          Count =
              static_cast<uint32_t>(UB->getAsUnsignedConstant().value_or(0)) +
              1;
      }
    }
    auto T = std::make_shared<NdType>();
    T->Kind = NdTypeKind::Array;
    T->ArrayCount = Count;
    T->ElemType = Elem;
    T->Size = static_cast<uint16_t>(Count * Elem->Size);
    return T;
  }
  case llvm::dwarf::DW_TAG_structure_type:
  case llvm::dwarf::DW_TAG_class_type:
  case llvm::dwarf::DW_TAG_union_type: {
    auto T = std::make_shared<NdType>();
    T->Kind = NdTypeKind::Struct;
    auto Sz = Die.find(llvm::dwarf::DW_AT_byte_size);
    T->Size =
        Sz ? static_cast<uint16_t>(Sz->getAsUnsignedConstant().value_or(0)) : 0;
    return T;
  }
  case llvm::dwarf::DW_TAG_subroutine_type: {
    auto T = std::make_shared<NdType>();
    T->Kind = NdTypeKind::Func;
    T->Size = 8;
    auto RetDie = Die.getAttributeValueAsReferencedDie(llvm::dwarf::DW_AT_type);
    T->RetType = RetDie.isValid() ? convertDwarfType(RetDie, Depth + 1)
                                  : NdType::makeVoid();
    for (auto Child : Die.children()) {
      if (Child.getTag() == llvm::dwarf::DW_TAG_formal_parameter) {
        auto ParamDie =
            Child.getAttributeValueAsReferencedDie(llvm::dwarf::DW_AT_type);
        T->ParamTypes.push_back(ParamDie.isValid()
                                    ? convertDwarfType(ParamDie, Depth + 1)
                                    : NdType::makeInt(4));
      }
    }
    return T;
  }
  case llvm::dwarf::DW_TAG_enumeration_type: {
    auto Sz = Die.find(llvm::dwarf::DW_AT_byte_size);
    uint16_t S =
        Sz ? static_cast<uint16_t>(Sz->getAsUnsignedConstant().value_or(4)) : 4;
    return NdType::makeInt(S);
  }
  default:
    return NdType::makeInt(4);
  }
}

void DWARFDebugContext::Impl::parseFunction(llvm::DWARFDie Die,
                                            llvm::DWARFUnit *Unit) {
  auto LowPC = Die.find(llvm::dwarf::DW_AT_low_pc);
  if (!LowPC)
    return;

  auto AddrVal = LowPC->getAsAddress();
  if (!AddrVal)
    return;
  va_t Addr = *AddrVal;
  if (Addr == 0)
    return;

  FunctionSym Sym;
  Sym.Addr = Addr;

  auto Hi = Die.find(llvm::dwarf::DW_AT_high_pc);
  if (Hi) {
    if (auto HiAddr = Hi->getAsAddress())
      Sym.Size = *HiAddr - Addr;
    else if (auto HiOff = Hi->getAsUnsignedConstant())
      Sym.Size = *HiOff;
  }

  if (auto Name = Die.getName(llvm::DINameKind::ShortName))
    Sym.Name = Name;
  else if (auto Link = Die.getName(llvm::DINameKind::LinkageName))
    Sym.Name = Link;
  else
    return;

  if (auto DeclFile = Die.find(llvm::dwarf::DW_AT_decl_file)) {
    if (auto Line = Die.find(llvm::dwarf::DW_AT_decl_line)) {
      Sym.DeclLoc.Line =
          static_cast<uint32_t>(Line->getAsUnsignedConstant().value_or(0));
      if (auto *LT = Unit->getContext().getLineTableForUnit(Unit)) {
        auto FileIdx = DeclFile->getAsUnsignedConstant().value_or(0);
        std::string FileName;
        if (LT->getFileNameByIndex(
                FileIdx, Unit->getCompilationDir(),
                llvm::DILineInfoSpecifier::FileLineInfoKind::AbsoluteFilePath,
                FileName))
          Sym.DeclLoc.File = FileName;
      }
    }
  }

  auto RetDie = Die.getAttributeValueAsReferencedDie(llvm::dwarf::DW_AT_type);
  Sym.ReturnType =
      RetDie.isValid() ? convertDwarfType(RetDie) : NdType::makeVoid();

  for (auto Child : Die.children()) {
    if (Child.getTag() == llvm::dwarf::DW_TAG_formal_parameter) {
      std::string PName;
      if (auto N = Child.getName(llvm::DINameKind::ShortName))
        PName = N;
      auto PtyDie =
          Child.getAttributeValueAsReferencedDie(llvm::dwarf::DW_AT_type);
      auto PTy =
          PtyDie.isValid() ? convertDwarfType(PtyDie) : NdType::makeInt(4);
      Sym.Params.emplace_back(PName, PTy);
      parseVariable(Child, Addr, true);
    } else if (Child.getTag() == llvm::dwarf::DW_TAG_variable) {
      parseVariable(Child, Addr, false);
    }
  }

  FuncMap[Addr] = Sym;
  FuncList.push_back(Sym);
}

void DWARFDebugContext::Impl::parseVariable(llvm::DWARFDie Die, va_t FuncAddr,
                                            bool IsParam) {
  std::string VName;
  if (auto N = Die.getName(llvm::DINameKind::ShortName))
    VName = N;
  if (VName.empty())
    return;

  auto TypeDie = Die.getAttributeValueAsReferencedDie(llvm::dwarf::DW_AT_type);
  auto VType =
      TypeDie.isValid() ? convertDwarfType(TypeDie) : NdType::makeInt(4);

  int64_t StackOff = 0;
  auto Loc = Die.find(llvm::dwarf::DW_AT_location);
  if (Loc && Loc->isFormClass(llvm::DWARFFormValue::FC_Exprloc)) {
    auto Block = Loc->getAsBlock();
    if (Block && Block->size() >= 2) {
      auto Op = (*Block)[0];
      if (Op == llvm::dwarf::DW_OP_fbreg) {
        const uint8_t *Begin = Block->data() + 1;
        const uint8_t *End = Block->data() + Block->size();
        unsigned BytesRead = 0;
        const char *Error = nullptr;
        int64_t Val =
            llvm::decodeSLEB128(Begin, &BytesRead, End, &Error);
        if (!Error && BytesRead > 0)
          StackOff = Val;
      }
    }
  }

  VariableSym VSym;
  VSym.Name = VName;
  VSym.Type = VType;
  VSym.StackOffset = StackOff;
  VSym.IsParam = IsParam;
  LocalsMap[FuncAddr].ByOffset[StackOff] = VSym;
}

void DWARFDebugContext::Impl::parseCompileUnits() {
  for (auto &CU : DwarfCtx->compile_units()) {
    auto Root = CU->getUnitDIE(false);
    if (!Root.isValid())
      continue;

    std::function<void(llvm::DWARFDie)> Walk = [&](llvm::DWARFDie Die) {
      if (!Die.isValid())
        return;
      auto Tag = Die.getTag();
      if (Tag == llvm::dwarf::DW_TAG_subprogram)
        parseFunction(Die, CU.get());
      for (auto Child : Die.children())
        Walk(Child);
    };
    Walk(Root);
  }

  std::sort(FuncList.begin(), FuncList.end(),
            [](const FunctionSym &A, const FunctionSym &B) {
              return A.Addr < B.Addr;
            });
}

// DebugContext interface

std::optional<FunctionSym> DWARFDebugContext::resolveFunction(va_t Addr) const {
  if (!PImpl || !PImpl->Loaded)
    return std::nullopt;
  auto It = PImpl->FuncMap.find(Addr);
  if (It != PImpl->FuncMap.end())
    return It->second;

  for (const auto &[_, FS] : PImpl->FuncMap) {
    if (FS.contains(Addr))
      return FS;
  }
  return std::nullopt;
}

std::optional<VariableSym>
DWARFDebugContext::resolveVariable(va_t FuncAddr, int64_t Offset) const {
  if (!PImpl || !PImpl->Loaded)
    return std::nullopt;
  auto It = PImpl->LocalsMap.find(FuncAddr);
  if (It == PImpl->LocalsMap.end())
    return std::nullopt;
  auto VIt = It->second.ByOffset.find(Offset);
  if (VIt == It->second.ByOffset.end())
    return std::nullopt;
  return VIt->second;
}

std::optional<TypeSym> DWARFDebugContext::resolveType(uint64_t) const {
  return std::nullopt;
}

std::optional<SourceLoc> DWARFDebugContext::sourceLocation(va_t Addr) const {
  if (!PImpl || !PImpl->Loaded || !PImpl->DwarfCtx)
    return std::nullopt;

  auto Info = PImpl->DwarfCtx->getLineInfoForAddress(
      {Addr, llvm::object::SectionedAddress::UndefSection});
  if (!Info || (Info->FileName.empty() && Info->Line == 0))
    return std::nullopt;

  SourceLoc Loc;
  Loc.File = Info->FileName;
  Loc.Line = Info->Line;
  Loc.Col = Info->Column;
  return Loc;
}

std::vector<FunctionSym> DWARFDebugContext::allFunctions() const {
  if (!PImpl || !PImpl->Loaded)
    return {};
  return PImpl->FuncList;
}

bool DWARFDebugContext::hasInfo() const { return PImpl && PImpl->Loaded; }

} // namespace neverd
