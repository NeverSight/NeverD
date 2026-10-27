/* Copyright (C) 2026 NeverC contributors.
SPDX-License-Identifier: AGPL-3.0-only
Adapted 2026-10-27 from NeverSight/NeverC 880d1e96ff48f41c5069c6d8f9839eaa8f37d64a,
tests/fixtures/x86/3dnow. Use a 63 MiB stack and enable native #MF delivery.
*/
ENTRY(_start)
SECTIONS { . = 0x100000; .text : { *(.multiboot) *(.text) } . = 0x110000; .payload : { *(.payload) } }
