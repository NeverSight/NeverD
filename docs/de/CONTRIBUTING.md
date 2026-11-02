**Sprachen**: [English](../../CONTRIBUTING.md) | [简体中文](../zh-CN/CONTRIBUTING.md) | [繁體中文](../zh-TW/CONTRIBUTING.md) | [日本語](../ja/CONTRIBUTING.md) | [한국어](../ko/CONTRIBUTING.md) | [Français](../fr/CONTRIBUTING.md) | [Deutsch](CONTRIBUTING.md) | [Español](../es/CONTRIBUTING.md) | [Italiano](../it/CONTRIBUTING.md) | [Русский](../ru/CONTRIBUTING.md) | [العربية](../ar/CONTRIBUTING.md)

<!-- i18n-source: af6c34901bacab00e5d1ed68b031f0a48a3ff64ac85ee208695647ec935b9790 -->

# Zu NeverD beitragen

NeverD ist ein semantikzentriertes Binäranalyseprojekt. Ein sinnvoller Beitrag
ist klar abgegrenzt, lässt nicht unterstütztes Verhalten sichtbar fehlschlagen
und enthält den kleinsten Test, der den geänderten Vertrag belegt.

Bewahren Sie beim Einbinden bestehenden Codes die Urheberrechts-, Lizenz- und
Quellenhinweise, auch wenn der Code mit KI-/LLM-Werkzeugen kopiert oder
bearbeitet wurde. Befolgen Sie den [Leitfaden zu Quellenangaben](ATTRIBUTION.md),
geben Sie für wiederverwendetes Material die ursprüngliche Quelle und Revision
an und prüfen Sie vor dem Einreichen dessen Lizenz. Behalten Sie Hinweise
Dritter unter ihren ursprünglichen Lizenzen bei.

Lesen Sie vor der Bearbeitung den
[Architekturleitfaden](architecture.md). Nutzen Sie den
[Testleitfaden](testing.md) zur Auswahl der Suite und die
[Roadmap](roadmap.md) für geplante Produktarbeit.

<!-- i18n-section: prerequisites -->
## Voraussetzungen

- Git mit Unterstützung für rekursive Submodule
- CMake 3.20 oder neuer
- Ninja
- Ein C++20-Compiler
- CPython 3.10+ mit Entwicklungsdateien zum Einbetten, wenn Python-Plugins aktiviert sind
- Clang und LLD (`ld.lld` und `lld-link`) für den vollständigen
  zielübergreifenden Fixture-Satz

Die rekursiven Submodule enthalten NeverDs LLVM- und Capstone-Forks, Unicorn
und Signaturdaten. Ersetzen Sie sie beim Validieren einer Änderung nicht durch
beliebige Systemversionen.

<!-- i18n-section: clone -->
## Klonen und initialisieren

Die Entwicklung wird in `dev` integriert; dies ist zugleich der
Standardbranch des Repositorys. Klonen Sie ihn mit allen Submodulen:

```bash
git clone --branch dev --recurse-submodules \
  https://github.com/NeverSight/NeverD.git
cd NeverD
```

Synchronisieren Sie in einem bestehenden Clone die Submodule vor dem ersten
Build und nach jedem Commit, der ihre aufgezeichneten Revisionen ändert:

```bash
git submodule sync --recursive
git submodule update --init --recursive
```

<!-- i18n-section: build -->
## Build-Profil auswählen

| Profil | Verwendung | Wichtiges Verhalten |
|--------|------------|---------------------|
| Release | Normale Entwicklung, vollständige Tests, Decode-/Lift-Benchmarks | Optimiert; repräsentativer Durchsatz |
| RelWithDebInfo | Profiling oder Debugging optimierter Hot Paths | Optimiert mit Debug-Symbolen |
| Debug | Assertions, schrittweise Quellcodeanalyse, lokale Korrektheitsarbeit | Nicht optimiert; Decode-Benchmarks absichtlich deutlich langsamer |

Verwenden Sie Release, sofern die Aufgabe nicht ausdrücklich Debug-Verhalten
benötigt:

```bash
cmake -S . -B build-release -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DBUILD_TESTING=ON
python scripts/build.py build-release --parallel
```

Standardmäßig wird `third_party/llvm-project` als integrierte Abhängigkeit
gebaut. Der erste Build dauert üblicherweise 30–60 Minuten; spätere Builds sind
inkrementell. `CMakePresets.json` definiert außerdem die Konfigurations- und
Build-Presets `release`, `relwithdebinfo` und `debug`. Oben werden explizite
Build-Verzeichnisse verwendet, damit die aktivierte Testoption sichtbar ist.

Nutzen Sie für das Debugging auf Quellcodeebene ein separates Verzeichnis,
anstatt den Release-Baum neu zu konfigurieren:

```bash
cmake -S . -B build-debug -G Ninja \
  -DCMAKE_BUILD_TYPE=Debug \
  -DBUILD_TESTING=ON
python scripts/build.py build-debug --parallel
```

Melden Sie niemals Decode- oder Lift-Durchsatz aus einem Debug-Build. Nutzen
Sie Release für Benchmarks oder RelWithDebInfo, wenn das Profiling Symbole
benötigt.

<!-- i18n-section: allocator -->
### Prozessallokator

Linux, macOS und Windows mit MSVC/clang-cl aktivieren `NEVERD_ENABLE_MIMALLOC`
standardmäßig. Werkzeuge, Analyse-Worker und Tests aus dem gemeinsamen CMake-Helfer
verwenden den Standardbranch des NeverSight-mimalloc-Forks nach NeverCs Host-Modell.
Unter Unix wird er einmal pro Programm eingebettet; Windows verwendet eine mimalloc-DLL
und die architekturgleiche `mimalloc-redirect*.dll` für dynamische CRT-Allokationen,
einschließlich libneverd und nativer Plugins. Beide liegen neben den Programmen und
werden nach `bin` installiert; liefern Sie beide aus. Windows benötigt `/MD` oder
`/MDd`; bei statischer CRT mimalloc deaktivieren. Die SDK-Bibliothek schreibt dem
einbettenden Host keinen Allokator vor. Windows ARM64 benötigt einen nativen ARM64-Build:
eine x64-DLL kann die ARM64EC-CRT der x64-Emulation nicht umleiten. MinGW behält den
Systemallokator. Siehe [Windows-Anforderungen des Forks](https://github.com/NeverSight/mimalloc/blob/main3/bin/readme.md).

`-DNEVERD_ENABLE_MIMALLOC=OFF` wählt die Systembaseline oder erlaubt externe Allokatoren;
`NEVERD_WORKER_MIMALLOC=OFF` deaktiviert nur den Worker. CMake unterdrückt mimalloc bei
Address-, Thread-, Memory- und Leak-Sanitizern, auch bei kombinierten oder
konfigurationsspezifischen Compiler-/Linkerflags. UBSan allein ist kompatibel.
Bei ausschließlich zielspezifischen Sanitizerflags zusätzlich OFF setzen.
Windows erlaubt Laufzeitvergleiche ohne Neubau mit `MIMALLOC_DISABLE_REDIRECT=1`;
`MIMALLOC_VERBOSE=1` zeigt die Aktivierung.

Der unterstützte Einstieg `python scripts/build.py BUILD_DIR` aktualisiert vor jedem
Build alle fünf NeverSight-Forks vom jeweiligen Remote-Standardbranch. Wie Capstone
und Unicorn sind sie Submodule in `third_party`. Das Skript ermittelt die Branchnamen,
aktualisiert die fünf Submodule und exportiert eingecheckte Quellen nach
`BUILD_DIR/third_party/<library>/<commit>/src`. Nicht eingecheckte oder divergierende
lokale Submodularbeit sowie Netzwerkfehler stoppen den Build; nichts wird überschrieben.
Das Build-Verzeichnis bleibt während Aktualisierung, Konfiguration und Kompilierung
gesperrt; der Snapshot gilt bis zum nächsten Aufruf. Auch inkrementelle Builds übernehmen neue Commits.

`neverd-dependency-inputs.json` dokumentiert Konfiguration, Commit und Export-SHA-256
und begleitet installierte Produkte; es zertifiziert kein Binärprogramm.
`neverd-build-receipt.json` erfasst Erfolg/Fehlschlag nur für angeforderte Ziele und
Konfiguration. CI bewahrt beide auf. Installation verweigert fehlgeschlagene Builds,
geänderte Eingaben und ungebaute Konfigurationen; Produkt-CI baut alle Installationsziele
vor dem Installationstest. Vergleichen Sie Commits vor plattformübergreifender Qualifikation,
da sich Mainline zwischen Läufen ändern kann. Einfaches `cmake --build` prüft Aktualität
und verweigert alte Snapshots; der Python-Einstieg aktualisiert sie. Seine automatische
Neugenerierung behält denselben Snapshot. `FETCHCONTENT_SOURCE_DIR_NEVERD_*` ist für
isolierte lokale Fixtures verfügbar, wird aber vom Mainline-Einstieg abgelehnt.
Mehrkonfigurationsgeneratoren benötigen `--config`. Kompilierungsziele über den Einstieg
bauen, Tests und `cmake --install` separat ausführen; das direkte Ziel `install` wird vor Kompilierung abgelehnt.

Siehe [Allokatorvalidierung](../testing.md#allocator-validation) für Smoke-Tests und Leistungsvergleiche.

<!-- i18n-section: acceleration -->
### Beschleunigungsbibliotheken

Normale Builds aktivieren Highway-Bytesuche, simdutf-UTF-8-Prüfung, simdjson-Transportparser
und oneTBB-Pipelineplanung unter Beibehaltung des Prozessallokators. JSON behält nlohmann-DOM
und Serialisierung: gültige reine Ganzzahlframes werden aus dem SIMD-Tape kopiert;
Dezimalzahlen und negative Null behalten die ursprüngliche Konvertierung. Eingaben außerhalb
des simdjson-Bereichs verwenden den bisherigen Zulassungspfad; ungültiger externer Text behält
bytegetreue Escapes. Diese Bibliotheken und mimalloc stammen aus den NeverSight-Forks in
`third_party/forks.json`; Kompatibilitätskorrekturen liegen auf ihren Standardbranches.
Der Build kopiert aktuelle Lizenzen und protokolliert genaue Commits, statt die fünf Abhängigkeiten auf alte Releases festzulegen.

`NEVERD_ENABLE_HIGHWAY`, `NEVERD_ENABLE_SIMDUTF`, `NEVERD_ENABLE_SIMDJSON` und
`NEVERD_ENABLE_TBB` stehen auf `ON`; einzeln auf `OFF` setzen für Vergleiche.
oneTBB behält Worker zwischen Batches, prüft deren 8-MiB-Stacks und baut/lädt weder
`tbbmalloc` noch dessen Proxy. Aufrufer mit großem Stack nutzen vorhandenen Spielraum,
andere erhalten einen nativen Koordinator. Hosts mit fork nach erster Nutzung oder
dynamischem SDK-Entladen müssen die native Threadbaseline `NEVERD_ENABLE_TBB=OFF`
verwenden; der persistente Scheduler unterstützt diese Lebenszyklen nicht. oneTBB wird
statisch in den gemeinsamen Kern eingebunden; seine Symbole bleiben auf diesen Kern
beschränkt. CLI-Komponentenaufrufe und GUI-Worker-Anfragen verwenden denselben Scheduler;
eine TBB-DLL oder Shared Library wird nicht mitgeliefert. Targets, die `neverd_shared`
linken, wählen den gemeinsamen Scheduler automatisch. Clients, die das SDK über ein
anderes Target einbinden und zusätzlich Komponentenarchive linken, müssen
`neverd_use_shared_task_scheduler` auf ihr finales Target anwenden. Eigenständige
Komponentenwerkzeuge behalten ihre eigene Laufzeit.
Diese eingeschränkte Integration vermeidet mehrere NeverD-Scheduler in einem Prozess;
Upstream bezeichnet [statische oneTBB-Builds](https://uxlfoundation.github.io/oneTBB/main/intro/static_linking.html)
weiterhin als nicht unterstützt und rät von ihrer allgemeinen Verwendung ab.

Siehe [Beschleunigungsvalidierung](../testing.md#acceleration-libraries) für native Tests, Vergleiche und Schedulergrenzen.

<!-- i18n-section: prebuilt-llvm -->
### Vorgefertigtes LLVM unter macOS

Mitwirkende auf Apple Silicon können den lokalen Build des LLVM-Forks
vermeiden:

```bash
cmake -S . -B build-release -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DBUILD_TESTING=ON \
  -DNEVERD_LLVM_PREBUILT=ON
python scripts/build.py build-release --parallel
```

CMake lädt das im Repository konfigurierte Release-Paket herunter, prüft seine
SHA-256-Summe und verwendet den entpackten Benutzer-Cache bei späteren Builds
erneut. Der Prebuilt-Kanal unterstützt ausschließlich macOS arm64. Intel-Macs
und Universal-Builds müssen den lokalen LLVM-Standardbuild verwenden.
Erweiterte Überschreibungen wie `NEVERD_LLVM_PREBUILT_TAG`, Spiegel-URL,
Cache-Verzeichnis und explizite Prüfsumme sind in
`cmake/NeverDLLVMPrebuilt.cmake` dokumentiert.

LLVM-Korrekturen gehören in den [NeverSight-LLVM-Fork](https://github.com/NeverSight/llvm-project).
Committen und mergen Sie sie dort und aktualisieren Sie danach NeverDs LLVM-Submodulrevision.
Integrierte Builds kompilieren diese Quellen direkt; fügen Sie NeverD keine LLVM-Patchdateien oder Quellüberlagerungen im Buildverzeichnis hinzu.
Vorgefertigte Pakete müssen aus dem korrigierten Fork neu gebaut werden, um dasselbe Verhalten zu qualifizieren.

<!-- i18n-section: workflow -->
## Branch- und Pull-Request-Ablauf

Beginnen Sie mit einem aktuellen `dev` und erstellen Sie einen fokussierten
Themenbranch:

```bash
git switch dev
git pull --ff-only origin dev
git switch -c docs/contributor-guide
```

Öffnen Sie Pull Requests gegen `dev`, nicht gegen einen vermuteten
Release-Branch. Halten Sie Commits gut prüfbar: ein zusammenhängender Zweck,
keine erzeugten Build-Ausgaben, keine unzusammenhängende Formatierung und keine
geänderten Submodul-Revisionen, sofern sie nicht Teil des Vorschlags sind.

<!-- i18n-section: code-style -->
## Codestil

C/C++ folgen den [LLVM Coding Standards](https://llvm.org/docs/CodingStandards.html).
[`.clang-format`](../../.clang-format) bestimmt die Formatierung,
[`.editorconfig`](../../.editorconfig) Kodierung, Zeilenenden und Leerraum.
Die C++-Version bestimmt `CMakeLists.txt`, nicht die Upstream-Stilrichtlinie. Geänderte Dateien formatieren:

```bash
clang-format -i path/to/changed.cpp path/to/changed.h
git diff --check
```

CI verwendet die in [`scripts/check_clang_format.py`](../../scripts/check_clang_format.py)
festgelegte Version; nutzen Sie dieselbe für reproduzierbare Ergebnisse. Der Formatter
prüft weder Headerzuständigkeit noch Regel-Literale. Kein repositoryweites Neuformatieren
für lokale Korrekturen. Benennung und Zerlegung der Umgebung folgen, Plattformverhalten
an der Loader/Lifter/Backend-Grenze halten und interne C++-Typen nicht im reinen C-SDK offenlegen.

<!-- i18n-section: headers -->
### Datei- und Headeraufbau

- Neue C++-Dateien beginnen mit LLVM-Dateiname/Zweck-Banner, `NeverD Decompiler` und
  Doxygen-`\file`, wie `lib/debug/PDBLoader.cpp`. Urheberrechts-/Lizenzhinweise erhalten.
- Eigenständig einbindbare Header verwenden `.h` und explizite Abhängigkeiten. Implementierungen
  binden zuerst ihr Interface ein. `.clang-format` sortiert weitere Includes: private Nachbarheader,
  öffentliche `neverd/`-Header, LLVM/Capstone in Anführungszeichen, dann spitze Klammern.
  Keine zufällige Reihenfolge oder transitive Includes voraussetzen.
- Neue Header erhalten pfadbasierte `NEVERD_..._H`-Guards, etwa `NEVERD_SDK_LIMITS_H`
  für `neverd/sdk/Limits.h`. Genügend private/Werkzeugverzeichnisse für eindeutige Namen
  einbeziehen; Guardname am `#endif` kommentieren. Bei gezielter Vereinheitlichung übernehmen,
  unbeteiligte Header unverändert lassen.
- Gemeinsame Interfaces nach `include/neverd/<component>/`, private Header neben Quellen in
  `lib/` oder `tools/`. Der Ort garantiert keine stabile öffentliche C-SDK-API. Abhängigkeiten
  zyklusfrei; Kernmodelle/Reader unabhängig von Sitzungen und Transportadaptern. Algorithmen
  im zuständigen Verzeichnis statt mehrfacher Semantik in CLI/SDK/Worker. Siehe [Architektur](architecture.md).
- Zuständigen Namensraum und klare Namen verwenden: neue Typen/Felder `UpperCamelCase`,
  Funktionen `lowerCamelCase`, Regelkonstanten `kUpperCamelCase`. Bestehende Interfaces
  konsistent erweitern; öffentliche C/Python-Verträge und STL-Konventionen erhalten.
  Kein namensraumweites `using namespace` in Headern.
- Absichtlich mehrfach eingebundene `.def`/`.inc`-Tabellen dokumentieren ihren Makrovertrag;
  sie sind keine eigenständigen `.h`-Interfaces und erhalten keinen expansionsverhindernden
  Guard. Generierte Header über ihren Erzeuger ändern.

Beispiel eines eigenständigen Regelheaders:

```cpp
//===- Limits.h - SDK resource limits and defaults ------------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// SDK request bounds, retained-state limits and default work budgets.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_SDK_LIMITS_H
#define NEVERD_SDK_LIMITS_H

#include <cstdint>

namespace neverd::sdk {

/// Traversal ceiling for a function without a recorded instruction extent.
inline constexpr uint64_t kMaxUnsizedInstructions = 65536;

} // namespace neverd::sdk

#endif // NEVERD_SDK_LIMITS_H
```

<!-- i18n-section: policy -->
### Zuständigkeit für Grenzen, Standardwerte und Konfiguration

Verwenden Sie `ResourceLimits.h` in Werkzeugverzeichnissen, die direkt zum Include-Suchpfad gehören. Auf einem Dateisystem ohne Unterscheidung der Groß- und Kleinschreibung kann ein lokales `Limits.h` das Systemheader `<limits.h>` verdecken.

Jede Regel erhält genau eine maßgebliche Definition im zuständigen Grenz-/Konfigurationsheader,
auch bei nur einem Aufrufer: Ein-/Ausgabekappen, Rekursion/Graphen, Arbeits-/Iterationsbudgets,
Heuristikschwellen, Cache, Seiten/Batches, Wiederholungen und Zeitlimits. Dies gilt für
`constexpr`, Makros, Literalvergleiche, `min`/`clamp`, Konstruktor-/Feldwerte und Standardparameter.

Bestehende Zuständigkeit zuerst wählen:

| Zuständig | Definitionsort |
| --- | --- |
| Gemeinsamer Kern/Pipeline | [`include/neverd/Limits.h`](../../include/neverd/Limits.h) |
| SDK | [`include/neverd/sdk/Limits.h`](../../include/neverd/sdk/Limits.h) |
| Web | [`include/neverd/web/Limits.h`](../../include/neverd/web/Limits.h) oder bestehende spezielle Definition |
| Andere Bibliothekskomponente | Bestehender Header/Definitionstabelle, sonst gezieltes `Limits.h` der Schicht |
| Nur CLI, Worker oder GUI | Werkzeuglokales `ResourceLimits.h` oder `Config.h`; Kern hängt nicht davon ab |
| Aufruferoptionen | Zuständiges Modell `Config.h`, `*Options.h` oder `*Configuration.h` |

Header schlank und zweckbezogen halten. Bestehende `*Limits.def`-/Konfigurationstabellen wiederverwenden
und über eigenständige C++-Header zugänglich machen. Keine konkurrierenden Definitionen in
`Limits.h`, `Constants.h`, `Config.h`; Kernregeln nicht im SDK/Werkzeug, unabhängige Modulregeln nicht alle im Wurzel-`Limits.h`.

C++-Skalare als typisierte `inline constexpr` im bisherigen Namensraum (`neverd::limits`,
`neverd::sdk`). Zweck und Einheit benennen, etwa `kMaxSourceBytes`, `kDefaultPageRows`,
`kRetryDelayMilliseconds`; Null/Sentinels und ungewöhnliche Grenzen erklären. Standardwert
und harte Grenze bei verschiedener Bedeutung trennen. Optionsmodelle dürfen Feldstandards besitzen;
Verbraucher nutzen Modell, Standardfabrik oder dieselbe Konstante. Verwandte Werte gemeinsam ableiten statt Literale zu wiederholen.

Adapter, Persistenzprüfung, CLI/GUI/Worker und Python/MCP verwenden dieselbe Autorität,
ebenso Schemamaxima und grenzbezogene Diagnosen. Sprachübergreifende Werte aus Tabelle oder
generierter Bindung ableiten; andernfalls Driftprüfung hinzufügen. Reine C-Kompatibilität erhalten.
Strengere Vorschau-/Seitengrenzen separat benennen und zuordnen; gleiche Zahlen allein bedeuten keine gemeinsame Regel.

Binärlayout, ABI/Protokoll/Instruktionen, Integerbereiche und beweisalgorithmusspezifische
Abrechnung bleiben im semantischen Modell. Anfangszustände, Indizes, Allokationshinweise und
unabhängige Testeingaben sind nicht automatisch Regeln. Unabhängige erwartete Testgrenzen
nicht durch die zu prüfende Implementierungskonstante ersetzen.

Deklarationen und Literale prüfen, auch mehrzeilige Guards, Schleifen, Makros, Standards,
Timer und Bindungen. Jede neue/geänderte Regel nach Bedeutung/Zuständigkeit prüfen; `Max*`
allein reicht nicht. Zusammenführung erhält Typen, Werte, Vergleichsrichtung, Sentinels und
Erschöpfungs-/Fehlerverhalten. Regeländerungen getrennt behandeln und betroffene [Tests](testing.md) ausführen.

<!-- i18n-section: artifacts -->
### Dokumentation und lokale Artefakte

Markdown knapp und quellprüfbar halten; relative Repositorylinks verwenden. CLI/API,
Support, Buildflags und Testbefehle im selben PR dokumentieren. Änderungen dieses Leitfadens
in alle unterstützten Sprachen übernehmen. Übersetzung vor Aktualisierung von `i18n-source`
SHA-256 prüfen; Abschnitte, Beispiele, APIs/Optionen und Linkziele abgleichen.
`python3 scripts/check_docs_i18n.py` prüft Links und Übersetzungsparität.

Maschinenlokale Leistungsberichte, Rohtraces und Zeitübersichten in ignorierten Build-/benchmark-results-
Verzeichnissen halten. Wiederverwendbare Messwerkzeuge und Regressionstests einchecken, keine lokalen Berichte oder kopierten Ergebnistabellen.

<!-- i18n-section: tests -->
## Tests ausführen

Führen Sie alle registrierten Tests über das Aggregatziel aus:

```bash
python scripts/build.py build-release --parallel
cmake --build build-release --target check-neverd
```

Nutzen Sie während der Entwicklung das kleinste relevante Ziel oder CTest-Label:

```bash
# Main Unicorn differential suite
cmake --build build-release --target check-neverd-semantic

# Lifter/loader/format binary only
python scripts/build.py build-release --target NeverDLiftTests --parallel 4
ctest --test-dir build-release --build-config Release \
  -L '^NeverDLiftTests$' --output-on-failure --parallel 4
```

Der [Testleitfaden](testing.md) dokumentiert alle Komfortziele,
nur per Label auswählbare Transformationssuiten, reguläre Ausdrücke für einen
einzelnen Test, die Fixture-Kompilierung und Unicorn-Roundtrips. Wird ein Ziel
wegen eines fehlenden Cross-Compilers oder Linkers übersprungen, melden Sie
diese Einschränkung; beschreiben Sie den nicht ausgeführten Pfad nicht als
bestanden.

<!-- i18n-section: python-sdk -->
### Python-Plugin-SDK

Bei SDK-Änderungen zuerst reine Python- und Driftprüfungen, danach native Adaptertests ausführen:

```bash
PYTHONPATH=pluginsdk/python python3 -m unittest discover \
  -s pluginsdk/python/tests -v
python3 -m mypy --config-file pluginsdk/python/pyproject.toml \
  pluginsdk/python/neverd_plugin
PYTHONPATH=pluginsdk/python python3 scripts/check_python_plugin_sdk.py
python scripts/build.py build-release --target \
  NeverDPluginRuntimeTests NeverDPythonRuntimeTests NeverDPythonPluginTests
ctest --test-dir build-release -R 'NeverD(Python|Plugin)' --output-on-failure
```

Öffentliche C-Deklarationen, Python-ABI-Signaturen, Paket-/CMake-Versionen und Auslieferungsworkflow
bilden einen gemeinsam geprüften Vertrag; zusammen ändern. Siehe [Python-Plugin-Leitfaden](python-plugins.md).

<!-- i18n-section: review -->
## Pull-Request-Checkliste

Vor der Review-Anfrage:

- Rebasen oder mergen Sie das aktuelle `dev` gemäß dem bevorzugten Ablauf der
  Maintainer und lösen Sie Submoduländerungen bewusst auf.
- Bauen Sie die betroffenen Ziele in Release oder erklären Sie, warum ein
  anderes Profil erforderlich ist.
- Führen Sie die engen Regressionstests und die breiteste praktisch mögliche
  relevante Suite aus; nennen Sie genaue Befehle und alle übersprungenen Tests
  in der PR-Beschreibung.
- Bewahren Sie striktes Lifting: Eine nicht unterstützte Instruktion darf nicht
  still zu einer geratenen Operation oder zu `NOP` werden.
- Ergänzen Sie bei Verhaltensänderungen semantische Abdeckung, nicht nur
  textuelle IR-Snapshots.
- Halten Sie unzusammenhängende Aufräumarbeiten, generierte Dateien und lokale
  Build-Artefakte aus dem Diff heraus.
- [Header- und Regelzuständigkeit](#codestil) prüfen, einschließlich numerischer Guards/Standards
  und Bindungsdrift; erfolgreiche Formatierung allein reicht nicht.
- Aktualisieren Sie öffentliche und Mitwirkenden-Dokumentation, wenn sich
  Verhalten, Support, Flags, Befehle oder Testzuständigkeit ändern.

Folgen Sie für sicherheitsrelevante Meldungen, die nicht als öffentlicher Pull
Request beginnen sollten, der [SECURITY.md](../../SECURITY.md).
