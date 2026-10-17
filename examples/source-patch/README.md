# Native source patch examples

Build a library, replace `target(x)` through both CLI and Python SDK, and run
the results through the real OS loader. No NeverC executable is used.

- [original.c](original.c): prints `x + 1`; does not import the extra library.
- [extra.c](extra.c): exports `extra_function(x) = 3*x` and `extra_state = 7`.
- [replacement.c](replacement.c): calls the saved original and both new imports,
  producing `4*x + 8`.
- [plugin/patch.py](plugin/patch.py): the actual SDK call, using the host's
  `Session`. [run.py](run.py) starts that host with `neverd plugins`.

Use native Linux, macOS or Windows x64/ARM64, Python 3.10+, and Clang on `PATH`
(validated producers include Clang 18 and 22). macOS needs the Xcode command
line tools; Windows needs a matching Visual Studio developer shell and Windows
SDK. Use a matching NeverD build with `NEVERD_ENABLE_PYTHON_PLUGINS=ON` and its
staged `bin/sdk/python` package. A standalone `Session("app")` is not supported.

If NeverD is not built yet, from the repository root:

```sh
cmake -S . -B build-example-host -G Ninja -DCMAKE_BUILD_TYPE=Release -DNEVERD_ENABLE_PYTHON_PLUGINS=ON -DNEVERD_ENABLE_CPU_EMULATION=OFF -DNEVERD_ENABLE_DRIVER_EMULATION=OFF
cmake --build build-example-host --target neverd --parallel 4
```

These examples use the OS loader and need no CPU or driver emulation.

Run from the repository root; `--output` must name a new directory. An existing
Python-enabled NeverD build can replace `build-example-host` below.

```sh
# Linux
python3 examples/source-patch/run.py --neverd build-example-host/bin/neverd --clang clang --output build-example-linux
# macOS
python3 examples/source-patch/run.py --neverd build-example-host/bin/neverd --clang clang --output build-example-macos
```

```powershell
# Windows: Visual Studio developer PowerShell, matching the host architecture
python examples/source-patch/run.py --neverd build-example-host/bin/neverd.exe --clang clang --output build-example-windows
```

Both APIs run by default. Add `--api cli` or `--api sdk` to run one path. The
driver prints every build/patch command and saves `commands.json`, both
publication receipts, process logs and `verified.json`. It discovers the
exported target address instead of hard-coding a VA. Each image runs with
`-5`, `0` and `7`; for `7`, the original prints `8` and each patch prints `36`.
A copied patch without its provider must fail in the OS loader. Failures retain
their logs; use a new output directory when retrying.

The library build commands used by the driver are, with a previously created
`build-demo` directory:

```sh
# Linux: SONAME and the executable's $ORIGIN rpath keep the example relocatable.
clang -shared -fPIC examples/source-patch/extra.c -Wl,-soname,libpatch_extra.so -o build-demo/libpatch_extra.so
# macOS: the executable uses @loader_path as its rpath.
clang -dynamiclib -fPIC examples/source-patch/extra.c -Wl,-install_name,@rpath/libpatch_extra.dylib -o build-demo/libpatch_extra.dylib
```

```powershell
# Windows: select aarch64-pc-windows-msvc in a native ARM64 developer shell.
clang --target=x86_64-pc-windows-msvc -shared examples/source-patch/extra.c -Wl,/implib:build-demo/patch_extra.lib -o build-demo/patch_extra.dll
```

The CLI uses `--bind=original:saved_target=VA` and two `--dependency` requests.
The SDK uses `bindings={"saved_target": ("original", VA)}` and
`dependencies={symbol: (library, kind)}`. Windows replacement declarations use
`__declspec(dllimport)` for the new function/data imports, not the saved body.

Layout growth is automatic in the ordinary `section` installation strategy.
The original requests no patch reserve; Mach-O explicitly uses `-headerpad,0`.
On Windows, [header_section.c](header_section.c) fills the section table with
small exported read-only data sections until the combined patch exceeds the
remaining header capacity. The driver requires PE headers and Mach-O prefixes
to actually grow, and checks that the target VA is unchanged.
Supported PE header growth and checked Mach-O PIE header/unwind growth preserve
the original code/data VAs. This is not permission to expand arbitrary layouts.
Unprovable prefixes, unsupported unwind/relocations, TLS, IFUNC, new symbol
versions, constructors and unsupported storage/layout shapes still fail.
Declarations must match the original ABI. See the
[support and refusal contract](../../docs/source-patching.md#original-callbacks-layout-growth-and-dependencies).
