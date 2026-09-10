# macOS development bundle

Build the GUI and worker in the same configuration as the selected engine, then
run the packaging helper on macOS. The output path must not already exist.

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

The helper copies the GUI, worker, engine, QML modules, Qt plugins and recursive
non-system dependencies. It rewrites library IDs and dependencies, supplies
bundle-relative search paths, rejects unresolved references and escaping or
dangling symlinks, and derives `LSMinimumSystemVersion` from every bundled
Mach-O image. `Contents/Resources/dependency-audit.json` records that audit;
hashes are taken before signing. The app icon comes from the repository's SVG
logo. Every Mach-O image is ad-hoc signed before the complete bundle is signed
and verified.

For a smaller portable GUI, configure the engine with
`NEVERD_ENABLE_PYTHON_PLUGINS=OFF`. The standalone MCP adapter remains available
at `NeverD.app/Contents/MacOS/neverd-mcp` and requires Python 3.10+ on `PATH`.
The GUI and worker do not require Python in this configuration. A build linked
to Python.framework includes that framework and uses its interpreter for MCP.
The native launcher disables Python bytecode caching so running MCP leaves the
signed application resources unchanged.
The packaged adapter requires explicit `--worker` and `--file` arguments, or an
explicit `--attach` credential path, as described in the [MCP guide](../neverd-mcp/README.md).

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

Qt SQL deployment is limited to the local SQLite driver used by the deployed
QtQuick.LocalStorage module. After `macdeployqt`, the helper validates the whole
SQL-driver directory before removing the known Mimer, ODBC and PostgreSQL
drivers. SQLite, QtSql and LocalStorage remain in the bundle; external SQL
connectors are outside this GUI's deployment scope. Unknown entries, symbolic
links, invalid directories or a missing required SQLite driver stop packaging
before any optional driver is removed. Dependency repair and auditing remain
strict for all retained components.

The deployment-scope tests use only temporary files and require no Qt tools:

```sh
PYTHONDONTWRITEBYTECODE=1 python3 tools/neverd-gui/tests/test_package_macos.py -v
```
