# Archer C7 V5 firmware analysis receipt

This static analysis uses the US Archer C7 V5 firmware published on TP-Link's
[official download page](https://www.tp-link.com/us/support/download/archer-c7/v5/).
No vendor executable was run and no device was flashed.

## Source and extraction

- Version: `1.3.1 Build 20260821`, published 2026-11-03.
- [Official archive](https://static.tp-link.com/upload/firmware/2026/202611/20261103/Archer%20C7_V5_1.3.1%20Build%2020260821.zip).
- Archive SHA-256: `6e74fd46f5faa5a60fe0b1acaaaeec05262069e0e832d90cd0cc794c302ebdb0`.
- Firmware member: `c7v5_us-up-ver1-3-1-P1[20260821-rel41256]_2026-08-21_11.49.18.bin`.
- Firmware size: 15,309,525 bytes.
- Firmware SHA-256: `e67f2ab65182b69faa008e46966d281904c2ff25c4e1cfee28600a591e0a6bce`.
- SquashFS 4/XZ starts at file offset `0x120a88`, with 14,114,313 bytes in
  its declared filesystem extent. Carve that range and extract it with 7-Zip.

The local ignored `build-mips/firmware` directory retains the source receipt,
official archive, selected `ubus`/`libubus`/`libubox` files and the ELF inventory.
The duplicate firmware member, disposable filesystem carve and unused extracted
files were removed after inventory; they can be reproduced from the archive.
There are 387 ELF children: 385 MIPS32r2/o32 and two MIPS I/o32. All are 32-bit
big endian. This is the evidence for prioritizing MIPS32 big endian; MIPS64
coverage uses separately generated owned fixtures.

## Selected executable

`bin/ubus` is a 6,533-byte stripped ELF with no section headers, eight program
headers, entry `0x400e30` and flags `0x70001005`. Its SHA-256 is
`6e1129aa58cb398a2453876ea4aa2395feecc84ecf78ed8ab43efd534b40e475`.

The dynamic table still identifies 42 symbols and 25 PLT imports. Independent
LLVM disassembly and dynamic relocation inspection establish these anchors:

| Address | Evidence |
| --- | --- |
| `0x400e30` | Process entry prepares libc startup arguments. |
| `0x400bd0` | Address passed as the first argument to `__uClibc_main`. |
| `0x411980` | Main GOT (`DT_PLTGOT`); startup establishes `$gp = 0x419970`. |
| `0x411914` | PLT GOT (`DT_MIPS_PLTGOT`). |
| `0x411990` | `R_MIPS_COPY` object `optarg`. |
| `0x411998` | `R_MIPS_COPY` object `optind`. |
| `0x4119a0` | `R_MIPS_COPY` object `stderr`. |

Imports include `getopt`, `atoi`, `strcmp`, `ubus_connect`, `ubus_invoke`,
`ubus_lookup`, `ubus_free` and uloop functions. The main function is the first
end-to-end NeverD target; its instruction stream uses ordinary integer
operations, calls and delay slots. The `call` handler additionally uses the
SPECIAL2 `MUL` instruction to scale its timeout. Its HI/LO clobbers must be
represented even though this handler does not read those results.

The independent dispatch-table decode at `0x4118e0` contains these four
command handlers:

| Command | Handler |
| --- | --- |
| `list` | `0x4011b4` |
| `call` | `0x4012d0` |
| `listen` | `0x400fcc` |
| `send` | `0x4011ec` |

The option string at `0x4017f8` is `vs:t:S`. The entry function loops over
`getopt`, sets a socket path (`-s`), parses a timeout with `atoi` (`-t`),
increments verbosity (`-v`) and sets a simplified-output byte (`-S`). It then
connects to ubus, matches the command name with `strcmp` and invokes the
selected handler with the connection, remaining argument count and argument
vector. The connection is released on the normal cleanup path. These are
static observations, not an execution trace or a vulnerability finding.

## NeverD analysis

The new Release CLI successfully loaded this sectionless MIPS32 big-endian
image and emitted both HighC and LLVMC for these original entries:

| Entry | Role | HighC | LLVMC |
| --- | --- | --- | --- |
| `0x400bd0` | main / `_ftext` | emitted | emitted |
| `0x4011b4` | `list` | emitted | emitted |
| `0x400fcc` | `listen` | emitted | emitted |
| `0x4011ec` | `send` | emitted | emitted |
| `0x4012d0` | `call` | emitted | emitted |

The former blocker at `0x401390` was SPECIAL2 `MUL v0,v1,v0`
(`0x70621002`), which scales the timeout by 1000. The 68-instruction handler
does not use merge loads; those are qualified by separate owned fixtures.
Successful emission alone is not a proof of runtime equivalence for these
vendor functions.

The main output retains the `getopt` loop, the `atoi(optarg)` timeout update,
`ubus_connect`, the four-entry `strcmp` dispatch loop, the indirect handler call
with three arguments, and the later `ubus_free`. The `list` handler checks for
at most one argument, passes either its first argument or null to `ubus_lookup`,
and supplies callback address `0x4013e0`. `send` accepts one or two arguments,
initializes a blob buffer, conditionally parses JSON, and calls
`ubus_send_event`. Names and address relationships agree with the independently
decoded PLT, startup and dispatch-table anchors above. These observations
establish useful static recovery, not a vulnerability finding.

The `call` view checks for two or three arguments and returns `-2` otherwise.
It initializes the blob buffer at `0x4119cc`, optionally parses the third
argument as JSON, and returns `-1` on parse failure. It resolves the first
argument with `ubus_lookup_id`, then passes the second argument, blob head,
callback `0x40114c`, null callback data and timeout multiplied by 1000 to
`ubus_invoke`. Lookup and invocation status values reach the return. These
relationships agree with the independently decoded instructions and PLT;
external prototypes and complete structure types remain inferred.

Artifacts under the ignored `build-mips/firmware` directory include
`ubus-{main,list,call,listen,send}-{highc,llvmc}.c`, per-invocation logs,
`neverd-analysis-results.json`, the original disassembly/import inventory,
and main/list/call LowIR, MedIR, HighIR and CFG JSON/DOT. `neverd-ir-results.json`
records success, diagnostics and hashes per view. The list and call handlers also emit
raw LLVM IR through the SDK's LLVM verifier. The raw LLVM
inspection route has a stricter code-pointer identity contract than LLVMC's
original-image source projection; main's dispatch table is not yet qualified
for that route.
Do not treat an empty `.ll` file or an emitter diagnostic as successful LLVM
inspection. Generated fixture LLVM is verified independently by the MIPS
pipeline suite.

Remaining source limitations include inferred external prototypes, raw numeric
string/data addresses and partial type/structure recovery. LLVMC refers to
original loaded image storage. The C files are analysis views; this receipt
does not claim that the complete firmware can be rebuilt, linked, executed,
modified or signed. No vendor executable was run.

### Reproduce selected views

After extracting the official image to `build-mips/firmware/rootfs`:

```sh
build-mips/bin/neverd info build-mips/firmware/rootfs/bin/ubus
build-mips/bin/neverd decompile --func 0x400bd0 -o build-mips/firmware/ubus-main-highc.c build-mips/firmware/rootfs/bin/ubus
build-mips/bin/neverd decompile --llvm --func 0x400bd0 -o build-mips/firmware/ubus-main-llvmc.c build-mips/firmware/rootfs/bin/ubus
```

Select the other addresses from the table with the same commands. The C API
`neverd_ir_low`, `neverd_ir_med`, `neverd_ir_high` and `neverd_cfg_json` expose
per-entry intermediate views; returned strings must be freed with
`neverd_free_string`. Firmware is downloaded locally, not redistributed in
the repository. See [MIPS testing](../testing.md#mips-elf-analysis-and-source-roundtrips)
for owned samples that can be executed and compared without vendor code.
