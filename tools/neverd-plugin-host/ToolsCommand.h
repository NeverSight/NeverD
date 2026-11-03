//===- ToolsCommand.h - Headless tools consumer ----------------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// JSON-lines tools consumer, without any Agent or model dependency.
//===----------------------------------------------------------------------===//
#ifndef NEVERD_TOOLS_PLUGIN_TOOLSCOMMAND_H
#define NEVERD_TOOLS_PLUGIN_TOOLSCOMMAND_H

#include <functional>

int runTools(int Argc, char **Argv, std::function<bool()> Cancelled);

#endif // NEVERD_TOOLS_PLUGIN_TOOLSCOMMAND_H
