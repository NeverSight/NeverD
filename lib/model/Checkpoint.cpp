//===- Checkpoint.cpp - Static checkpoint instruction evidence -----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Inspect bounded pickle instruction streams and seekable ZIP members.
//===----------------------------------------------------------------------===//

#include "ModelInternal.h"

#include "neverd/web/Error.h"
#include "neverd/web/SeekableZip.h"

#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/Endian.h"

namespace neverd::model {
namespace {
std::string_view operationName(unsigned Op) {
  switch (Op) {
  case 'c':
    return "GLOBAL";
  case 0x93:
    return "STACK_GLOBAL";
  case 'R':
    return "REDUCE";
  case 'b':
    return "BUILD";
  case 'i':
    return "INST";
  case 'o':
    return "OBJ";
  case 0x81:
    return "NEWOBJ";
  case 0x92:
    return "NEWOBJ_EX";
  case 0x82:
  case 0x83:
  case 0x84:
    return "EXT";
  case 'P':
  case 'Q':
    return "PERSISTENT_ID";
  case 0x97:
  case 0x98:
    return "EXTERNAL_BUFFER";
  default:
    return {};
  }
}
bool identifier(std::string_view Value) {
  return !Value.empty() && Value.size() <= 128 &&
         llvm::all_of(Value, [](unsigned char C) {
           return llvm::isAlnum(C) || C == '_' || C == '.';
         });
}

Json inspectPickle(Package &P, const std::string &Source, std::string_view Data,
                   std::string_view Member) {
  size_t Pos = 0, FrameEnd = 0, Count = 0, Offset = 0;
  std::optional<unsigned> Protocol;
  auto take = [&](uint64_t N) {
    const size_t End = FrameEnd ? FrameEnd : Data.size();
    if (N > End - Pos)
      throw Error("truncated pickle operand or frame");
    auto Result = Data.substr(Pos, N);
    Pos += N;
    return Result;
  };
  auto integer = [&](unsigned Width) {
    auto Bytes = take(Width);
    uint64_t V = 0;
    for (unsigned I = 0; I < Width; ++I)
      V |= uint64_t(uint8_t(Bytes[I])) << (I * 8);
    return V;
  };
  auto line = [&] {
    auto End = FrameEnd ? FrameEnd : Data.size();
    auto N = Data.substr(Pos, End - Pos).find('\n');
    if (N == Data.npos)
      throw Error("unterminated pickle operand");
    return take(N + 1).substr(0, N);
  };
  bool Complete = false;
  Json Location = {
      {"member", Member},
      {"coordinate_space", Member.empty() ? "file" : "decoded-member"}};
  try {
    while (Pos < Data.size()) {
      P.B.tick();
      if (Pos == FrameEnd)
        FrameEnd = 0;
      Offset = Pos;
      auto Op = integer(1);
      ++Count;
      std::string_view ModuleName, SymbolName;
      switch (Op) {
      case 0x80:
        Protocol = integer(1);
        if (Offset || *Protocol > 5)
          throw Error("unsupported pickle protocol");
        break;
      case 0x95: {
        if (FrameEnd)
          throw Error("nested pickle frame");
        const auto Size = integer(8);
        if (Size > Data.size() - Pos)
          throw Error("truncated pickle frame");
        FrameEnd = Pos + Size;
        break;
      }
      case '.':
        if (Pos != Data.size())
          throw Error("bytes after pickle STOP are uninspected");
        Complete = true;
        break;
      case 'c':
      case 'i': {
        auto Module = line(), Name = line();
        if (identifier(Module) && identifier(Name)) {
          ModuleName = Module;
          SymbolName = Name;
        }
        break;
      }
      case 'I':
      case 'L':
      case 'S':
      case 'V':
      case 'F':
      case 'g':
      case 'p':
      case 'P':
        line();
        break;
      case 'K':
      case 'h':
      case 'q':
      case 0x82:
        take(1);
        break;
      case 'M':
      case 0x83:
        take(2);
        break;
      case 'J':
      case 'j':
      case 'r':
      case 0x84:
        take(4);
        break;
      case 'G':
        take(8);
        break;
      case 0x8a:
      case 'U':
      case 'C':
      case 0x8c:
        take(integer(1));
        break;
      case 0x8b:
      case 'T':
      case 'B':
      case 'X':
        take(integer(4));
        break;
      case 0x8e:
      case 0x96:
      case 0x8d:
        take(integer(8));
        break;
      case 0x97:
      case 0x98:
      case 'N':
      case 0x88:
      case 0x89:
      case ']':
      case 'a':
      case 'e':
      case 'l':
      case ')':
      case 't':
      case 0x85:
      case 0x86:
      case 0x87:
      case '}':
      case 'd':
      case 's':
      case 'u':
      case 0x8f:
      case 0x90:
      case 0x91:
      case '0':
      case '2':
      case '(':
      case '1':
      case 0x94:
      case 0x93:
      case 'R':
      case 'b':
      case 'o':
      case 0x81:
      case 0x92:
      case 'Q':
        break;
      default:
        throw Error("unsupported pickle instruction");
      }
      auto Operation = operationName(Op);
      if (!Operation.empty()) {
        Json Evidence = Location;
        if (!ModuleName.empty()) {
          Evidence["module"] = ModuleName;
          Evidence["symbol"] = SymbolName;
        }
        Evidence["offset"] = Offset;
        Evidence["length"] = Pos - Offset;
        Evidence["opcode"] = Operation;
        P.finding("package.pickle-operation", Source, "review", "serialization",
                  "observed",
                  "Review this serialization operation and its resolution "
                  "before using a runtime loader.",
                  std::move(Evidence));
      }
    }
    if (!Complete)
      throw Error("pickle stream has no terminal STOP");
  } catch (const Stop &) {
    throw;
  } catch (const Error &E) {
    Location["offset"] = Offset;
    P.gap(Source, E.what(), Location);
  }
  return {{"instruction_count", Count},
          {"inspected_bytes", Pos},
          {"complete_instruction_stream", Complete},
          {"declared_protocol", Protocol ? Json(*Protocol) : Json(nullptr)},
          {"sha256", digest(Data)}};
}
} // namespace

void inspectCheckpoint(Package &P, const std::string &Name) {
  File &F = P.file(Name);
  auto Prefix = F.header(0, std::min<uint64_t>(F.size(), 4), P.B);
  Json Item = {{"id", Name},
               {"size", F.size()},
               {"format", "checkpoint"},
               {"inspection", "static instruction framing"}};
  if (Prefix != "PK\x03\x04") {
    if (F.size() > limits::kJsonBytes)
      throw Error("unframed checkpoint exceeds static instruction byte limit");
    auto Data = F.header(0, F.size(), P.B);
    Item["pickle"] = inspectPickle(P, Name, Data, {});
    P.Artifacts.push_back(std::move(Item));
    return;
  }
  Item["format"] = "checkpoint-zip";
  Json Members = Json::array();
  uint64_t Steps = 0, Index = 0;
  bool Found = false;
  try {
    const auto &Stop = P.B.Opts.Cancel;
    auto Read = [&](uint64_t At, uint64_t N) { return F.header(At, N, P.B); };
    auto Cancel = [&] { return Stop && Stop(); };
    auto Directory = web::readCheckpointZipDirectory(F.size(), Read, Cancel);
    for (const auto &[M, CRC] : Directory) {
      P.B.tick();
      Json Row = {{"name", M.Path},
                  {"header_offset", M.LocalHeaderOffset},
                  {"stored_offset", M.StoredOffset},
                  {"stored_length", M.StoredSize},
                  {"expanded_length", M.Size},
                  {"method", M.Compression},
                  {"coverage", "inventoried"}};
      if (!M.UnavailableReason.empty()) {
        Row["coverage"] = M.UnavailableReason;
        P.gap(Name, M.UnavailableReason,
              {{"member", M.Path}, {"offset", M.StoredOffset}});
      } else if (M.Kind == "file" &&
                 (M.Path.ends_with(".pkl") || M.Path.ends_with(".pickle"))) {
        Found = true;
        if (M.Size > limits::kJsonBytes || M.Size > P.B.ExpandedLeft ||
            M.StoredSize > limits::kJsonBytes ||
            M.Size > ::neverd::limits::web::kMaxZipExpansionRatio *
                         std::max<uint64_t>(M.StoredSize, 1))
          throw Error("checkpoint expansion budget exceeded");
        P.B.ExpandedLeft -= M.Size;
        std::string Data;
        Data.reserve(M.Size);
        web::decodeZipMemberRange(
            M.StoredSize,
            [&](uint64_t At, uint64_t N) {
              return Read(M.StoredOffset + At, N);
            },
            M.Compression, M.Size, CRC,
            [&](std::string_view Part) { Data += Part; }, Steps, Cancel);
        Row["pickle"] = inspectPickle(P, Name, Data, M.Path);
        Row["coverage"] =
            Row["pickle"].at("complete_instruction_stream") == true
                ? "instruction-framing"
                : "partial";
      } else if (M.Kind == "file")
        Row["coverage"] = "payload-skipped";
      if (Index++ >= P.B.Opts.Offset && Members.size() < P.B.Opts.Limit)
        Members.push_back(std::move(Row));
    }
    if (!Found)
      P.gap(Name, "archive contains no readable pickle metadata");
    Item["member_count"] = Directory.size();
  } catch (const web::Error &E) {
    if (std::string_view(E.what()) == "cancelled")
      throw Stop("cancelled");
    P.gap(Name, E.what());
  }
  Item["members"] = std::move(Members);
  P.Artifacts.push_back(std::move(Item));
}
} // namespace neverd::model
