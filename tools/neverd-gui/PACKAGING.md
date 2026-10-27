# Desktop distribution

NeverD embeds zlib and its core acceleration libraries. The CLI, worker and MCP
server share `libneverd`; Qt libraries and required plugins ship dynamically
beside the desktop application. A prebuilt Qt library can depend on its own
dynamic zlib even though NeverD uses static zlib. Keep that private Qt dependency
in the package.

## Linux and Windows

The GUI's CMake install runs Qt deployment and then completes and audits the
runtime dependency closure. `NEVERD_BUNDLE_DESKTOP_RUNTIME` defaults to `ON`.
Standalone builds import the matching engine through `NEVERD_ENGINE_LIBRARY`
(and `NEVERD_ENGINE_IMPLIB` on Windows); set `NEVERD_DESKTOP_CLI` to include its
matching CLI executable. An integrated build includes the `neverd` target.
`NEVERD_DESKTOP_QT_LICENSES` supplies the matching Qt license/attribution catalog;
the checked-in catalog is selected automatically only for Qt 6.11.1.
`NEVERD_DESKTOP_EXTRA_LICENSES` supplies notices for additional bundled runtime
libraries. Core, zlib and KDDockWidgets notices are installed automatically.
`NEVERD_DESKTOP_RUNTIME_LIBRARIES` lists additional libraries opened by name
instead of linked directly. On Linux it defaults to available HarfBuzz and
OpenSSL libraries, used by FreeType auto-hinting and Qt's TLS backend. Extend
this list for additional runtime-loaded features in a custom Qt kit.

```sh
cmake -S tools/neverd-gui -B build-gui -G Ninja \
  -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH=/path/to/Qt/kit \
  -DNEVERD_ENGINE_LIBRARY=/path/to/core/bin/libneverd.so \
  -DNEVERD_DESKTOP_CLI=/path/to/core/bin/neverd
python3 scripts/build.py build-gui --target neverd-gui neverd-worker neverd-mcp
cmake --install build-gui --prefix "$PWD/build-gui/dist/NeverD"
```

Use the platform's engine/CLI filenames on Windows. Multi-config generators
also require `--config Release` for both build and installation. Build the
engine with `NEVERD_ENABLE_PYTHON_PLUGINS=OFF` for a package without Python.

Linux deployment requires `patchelf`. Libraries and plugins receive paths
relative to the installed package. Original absolute paths and build-relative
`$ORIGIN` paths remain available during dependency discovery, then external
paths are removed before the final audit. glibc, the ELF loader and graphics dispatch
libraries remain operating-system dependencies; other required libraries are
copied privately. Build on the oldest supported Linux baseline: bundling Qt
does not lower the glibc requirement of the compiled binaries. Windows keeps
system DLLs under the actual `SystemRoot` external and bundles non-system
dependencies with the executables. A vendor directory named `Windows` is not
treated as the operating system.
Build natively for the selected architecture and keep Qt, engine and CRT
configurations consistent.

Only Qt's local SQLite driver is retained. Missing required files, unknown SQL
drivers, unresolved dependencies, conflicting library names and dependencies
that still resolve outside the completed package stop installation.
`share/neverd/dependency-audit.txt` records hashes of the deployed runtime.
The collector validates parent directories before writing or removing files;
escaping, dangling and absolute symlinks stop installation. It replaces patched
files, copied libraries and the audit without modifying shared hard-link inodes.

The Linux collector regressions use small real ELF files to check transitive
dependency relocation, missing libraries and SQL drivers, and preservation of
files outside the installation:

```sh
python3 tools/neverd-gui/tests/test_desktop_runtime.py -v
```

With Clang, `lld-link` and `llvm-objdump` available, the same suite also checks
real PE import tables for x86, x64 and ARM64 in Debug and Release. Set
`LLVM_OBJDUMP=/path/to/llvm-objdump` when the executable has a versioned name.
These cross-compiled fixtures validate DLL collection and exclusions; they do
not execute Windows code or qualify native Windows runtime behavior.

Move the completed install directory away from the build tree and test it with
developer environment variables cleared. For Linux, launch a native X11 or
Wayland session (or `xvfb-run` for a headless local check):

```sh
env -u LD_LIBRARY_PATH -u QT_PLUGIN_PATH -u QT_QPA_PLATFORM \
  -u QML2_IMPORT_PATH -u QML_IMPORT_PATH \
  PATH=/usr/bin:/bin /path/to/moved/NeverD/bin/neverd-gui --smoke-test
```

Also open a binary through the bundled worker and run the bundled CLI. Verify
that loaded Qt and non-system dependency paths stay inside the package. Linux
checks do not qualify Windows or macOS runtime behavior. Before public release,
retain the matching Qt/dependency notices and corresponding sources, and apply
the platform's signing and release qualification process.

## macOS development bundle

Build the GUI and worker in the same configuration as the selected engine, then
run the packaging helper on macOS. The output path must not already exist.
The helper reads the GUI from CMake's `bin/neverd-gui.app` bundle when present;
older builds with a plain `bin/neverd-gui` executable remain supported.

```sh
python3 tools/neverd-gui/package_macos.py \
  --build-dir build-gui \
  --engine build-gui-core/bin/libneverd.dylib \
  --qt-dir /opt/homebrew/opt/qt \
  --kddw-source /path/to/KDDockWidgets-2.4.1 \
  --json-license build-gui/_deps/neverd_worker_json-src/LICENSE.MIT \
  --output build-gui/dist/NeverD.app
```

KDDockWidgets and JSON license inputs must match the sources used by the build.
The checked-in Qt license catalog matches Qt 6.11.1; a different Qt build requires
its matching `--qt-licenses` directory. Homebrew component notices and available
SPDX catalogs are collected from the libraries actually copied into the bundle.
The pinned static zlib notice and any notices staged beside the engine in its
`licenses` directory are retained too; build and package matching engine inputs.

The helper copies the GUI, worker, engine, Qt plugins and recursive non-system
dependencies. It rewrites library IDs and dependencies, supplies
bundle-relative search paths, rejects unresolved references and escaping or
dangling symlinks, and derives `LSMinimumSystemVersion` from every bundled
Mach-O image. Engine frameworks' dependencies are collected before
`macdeployqt` rewrites their original paths, including Python extension
libraries such as Homebrew's keg-only SQLite. The final repair pass also covers
the plugins deployed by Qt. `Info.plist` declares the `neverd` URL scheme
(`CFBundleURLTypes`), so that LaunchServices hands a clicked `neverd://` link
to the workbench; on Linux and Windows, `neverd-gui --register-link-handler`
registers it for the user instead. `Contents/Resources/dependency-audit.json` records that audit;
hashes are taken before signing. The app icon comes from the repository's SVG
logo. Every Mach-O image is ad-hoc signed before the complete bundle is signed
and verified.

Build a new bundle in a staging directory. Move it to the user-facing path only
after the helper completes and a native launch succeeds; a failed dependency
repair leaves an unfinished bundle whose Mach-O signatures are invalid.

For a smaller portable GUI, configure the engine with
`NEVERD_ENABLE_PYTHON_PLUGINS=OFF`; the GUI, worker and MCP server then do not
require Python. The MCP server is the native executable
`NeverD.app/Contents/MacOS/neverd-mcp`, which runs the bundled engine and, like
the worker, keeps the Hypervisor entitlement. It requires an explicit `--file`
input or `--attach` credential path, as described in the
[MCP guide](../neverd-mcp/README.md).

Verify the bundle with a native Cocoa launch in a clean environment:

```sh
env -u DYLD_LIBRARY_PATH -u DYLD_FRAMEWORK_PATH \
  -u QML2_IMPORT_PATH -u QML_IMPORT_PATH -u QT_PLUGIN_PATH -u QT_QPA_PLATFORM \
  PATH=/usr/bin:/bin:/usr/sbin:/sbin \
  build-gui/dist/NeverD.app/Contents/MacOS/neverd-gui --smoke-test
```

`macdeployqt` packages the native Cocoa platform plugin; the offscreen test
plugin used by development CTest runs is not part of this application bundle.
The helper produces a local development artifact. It does not notarize an app,
produce a universal binary, or publish a release. A public distribution also
needs the matching complete corresponding sources and dependency sources
required by the licenses preserved in `Contents/Resources/Licenses`.

Qt SQL deployment is limited to the local SQLite driver that reads and writes
`.nddb` project databases. After `macdeployqt`, the helper validates the whole
SQL-driver directory before removing the known Mimer, ODBC and PostgreSQL
drivers. SQLite and QtSql remain in the bundle; external SQL connectors are
outside this GUI's deployment scope. Unknown entries, symbolic
links, invalid directories or a missing required SQLite driver stop packaging
before any optional driver is removed. Dependency repair and auditing remain
strict for all retained components.

The deployment-scope tests use only temporary files and require no Qt tools:

```sh
PYTHONDONTWRITEBYTECODE=1 python3 tools/neverd-gui/tests/test_package_macos.py -v
```
