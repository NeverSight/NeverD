/* Copyright (C) 2026 NeverC contributors.
SPDX-License-Identifier: AGPL-3.0-only
Adapted 2026-10-27 from NeverSight/NeverC 880d1e96ff48f41c5069c6d8f9839eaa8f37d64a,
tests/fixtures/x86/3dnow. Use a 63 MiB stack and enable native #MF delivery.
*/
ENTRY(stage64)
SECTIONS { . = 0x110000; .text : { *(.text.start) *(.text*) } .rodata : { *(.rodata*) } .data : { *(.data*) } .bss : { __bss_start = .; *(.bss*) *(COMMON) __bss_end = .; } /DISCARD/ : { *(.eh_frame*) *(.comment*) *(.note*) } }
