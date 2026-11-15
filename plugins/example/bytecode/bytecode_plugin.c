//===- bytecode_plugin.c - External bytecode rules in a C plugin -*- C -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "neverd/sdk/NeverDPlugin.h"

#include <stdio.h>
#include <string.h>

static void decode(void *UserData, const unsigned char *Bytes, size_t Size,
                   uint64_t Address, ND_BytecodeInstructionSinkV1 Reply,
                   void *ReplyContext) {
  const unsigned Key = *(const unsigned *)UserData;
  const char *Instruction =
      Size && (Bytes[0] ^ (unsigned char)(Address + Key)) == 0x6d
          ? "{\"size\":1,\"operations\":[{\"op\":\"RETURN\",\"inputs\":[]}]}"
          : "{\"error\":\"unrecognized example instruction\"}";
  Reply(ReplyContext, Instruction, strlen(Instruction));
}

static int recover(neverd_session_t Session, int Arg) {
  (void)Session;
  // Independently constructed example. A real plugin reads its own container
  // and supplies its encoding rules, function boundaries and call bindings.
  const unsigned Key = 37;
  const unsigned char Code[] = {(unsigned char)(0x6d ^ (Arg ? Key : 0))};
  const char Request[] =
      "{\"schemaVersion\":1,\"output\":\"highc\","
      "\"functions\":[{\"entry\":0,\"end\":1,\"name\":\"plugin_return\"}],"
      "\"profile\":{\"version\":1,\"register_bytes\":8,\"byte_order\":"
      "\"little\","
      "\"encodings\":[{\"size\":1,\"match\":[{\"offset\":0,\"value\":109}],"
      "\"operations\":[{\"op\":\"RETURN\",\"inputs\":[]}]}]}}";
  const char DynamicRequest[] =
      "{\"schemaVersion\":1,\"output\":\"highc\","
      "\"functions\":[{\"entry\":0,\"end\":1,\"name\":\"plugin_return\"}],"
      "\"layout\":{\"version\":1,\"register_bytes\":8,\"byte_order\":"
      "\"little\"}}";
  const char *Report =
      Arg ? neverd_bytecode_recover_decoder_json_v1(
                Code, sizeof(Code), DynamicRequest, sizeof(DynamicRequest) - 1,
                decode, (void *)&Key)
          : neverd_bytecode_recover_json_v1(Code, sizeof(Code), Request,
                                            sizeof(Request) - 1);
  if (!Report)
    return 1;
  // The C API emits compact canonical JSON. Applications needing individual
  // fields should parse the report with their existing JSON library.
  int Failed = strstr(Report, "\"ok\":true") == NULL;
  puts(Report);
  neverd_free_string(Report);
  return Failed;
}

NEVERD_PLUGIN_EXPORT neverd_plugin_t neverd_plugin = {
    .Name = "External Bytecode C",
    .Version = "1.0.0",
    .Author = "NeverD contributors",
    .Description = "Supplies external bytecode rules through the public C ABI",
    .Type = NEVERD_PLUGIN_PROCESSOR,
    .Run = recover,
};
