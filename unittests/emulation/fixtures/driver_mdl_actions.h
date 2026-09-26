//===- driver_mdl_actions.h - Shared MDL chain fixture actions ------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#ifndef NEVERD_TEST_DRIVER_MDL_ACTIONS_H
#define NEVERD_TEST_DRIVER_MDL_ACTIONS_H

enum DriverMdlChainAction {
  MdlAttachPrimary = 16,
  MdlCyclicChain = 28,
  MdlAppendChain = 32,
  MdlAppendFirst,
  MdlReplacePrimary,
  MdlUnlinkMiddle,
  MdlFreeAttached,
  MdlAppendDirect,
  MdlReplaceDirect
};

#endif // NEVERD_TEST_DRIVER_MDL_ACTIONS_H
