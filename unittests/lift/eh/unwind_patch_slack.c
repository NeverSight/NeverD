//===- unwind_patch_slack.c - File-backed unwind fixture limits ----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#if defined(NEVERD_DWARF_EH_SLACK)

static const unsigned char EhFrameLimit
    __attribute__((used, section(".neverd.eh_frame.limit"))) = 0;
static const unsigned char EhFrameHdrLimit
    __attribute__((used, section(".neverd.eh_frame_hdr.limit"))) = 0;

#elif defined(NEVERD_ARM_EHABI_SLACK)

static const unsigned char ArmExIdxLimit
    __attribute__((used, section(".neverd.arm_exidx.limit"))) = 0;

#else
#error "select an unwind fixture layout"
#endif
