**Sprachen**: [English](../../CONTRIBUTING.md) | [简体中文](CONTRIBUTING.zh-CN.md) | [繁體中文](CONTRIBUTING.zh-TW.md) | [日本語](CONTRIBUTING.ja.md) | [한국어](CONTRIBUTING.ko.md) | [Français](CONTRIBUTING.fr.md) | [Deutsch](CONTRIBUTING.de.md) | [Español](CONTRIBUTING.es.md) | [Italiano](CONTRIBUTING.it.md) | [Русский](CONTRIBUTING.ru.md) | [العربية](CONTRIBUTING.ar.md)

# Zu NeverD beitragen

NeverD ist ein semantikzentriertes Binäranalyseprojekt. Ein sinnvoller Beitrag
ist klar abgegrenzt, lässt nicht unterstütztes Verhalten sichtbar fehlschlagen
und enthält den kleinsten Test, der den geänderten Vertrag belegt.

Lesen Sie vor der Bearbeitung den
[Architekturleitfaden](../architecture.de.md). Nutzen Sie den
[Testleitfaden](../testing.de.md) zur Auswahl der Suite und die
[Roadmap](../roadmap/README.de.md) für geplante Produktarbeit.

## Voraussetzungen

- Git mit Unterstützung für rekursive Submodule
- CMake 3.20 oder neuer
- Ninja
- Ein C++20-Compiler
- Clang und LLD (`ld.lld` und `lld-link`) für den vollständigen
  zielübergreifenden Fixture-Satz

Die rekursiven Submodule enthalten NeverDs LLVM- und Capstone-Forks, Unicorn
und Signaturdaten. Ersetzen Sie sie beim Validieren einer Änderung nicht durch
beliebige Systemversionen.

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
cmake --build build-release --parallel
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
cmake --build build-debug --parallel
```

Melden Sie niemals Decode- oder Lift-Durchsatz aus einem Debug-Build. Nutzen
Sie Release für Benchmarks oder RelWithDebInfo, wenn das Profiling Symbole
benötigt.

### Vorgefertigtes LLVM unter macOS

Mitwirkende auf Apple Silicon können den lokalen Build des LLVM-Forks
vermeiden:

```bash
cmake -S . -B build-release -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DBUILD_TESTING=ON \
  -DNEVERD_LLVM_PREBUILT=ON
cmake --build build-release --parallel
```

CMake lädt das im Repository konfigurierte Release-Paket herunter, prüft seine
SHA-256-Summe und verwendet den entpackten Benutzer-Cache bei späteren Builds
erneut. Der Prebuilt-Kanal unterstützt ausschließlich macOS arm64. Intel-Macs
und Universal-Builds müssen den lokalen LLVM-Standardbuild verwenden.
Erweiterte Überschreibungen wie `NEVERD_LLVM_PREBUILT_TAG`, Spiegel-URL,
Cache-Verzeichnis und explizite Prüfsumme sind in
`cmake/NeverDLLVMPrebuilt.cmake` dokumentiert.

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

## Codestil

C und C++ folgen den LLVM-Konventionen; `.clang-format` ist die maßgebliche
Formatierungsdefinition des Repositorys. Formatieren Sie nur geänderte Dateien:

```bash
clang-format -i path/to/changed.cpp path/to/changed.h
git diff --check
```

Formatieren Sie für eine gezielte Korrektur nicht das gesamte Repository neu.
Folgen Sie den Namens- und Zerlegungsmustern der Umgebung, belassen Sie
plattformspezifisches Verhalten an der jeweiligen Loader-/Lifter-/Backend-Grenze
und legen Sie keine internen C++-Typen über das reine C-SDK offen.

Markdown soll knapp und aus dem Quellcode überprüfbar sein. Verwenden Sie für
Dateien im Repository relative Links und aktualisieren Sie die Dokumentation
im selben Pull Request, wenn sich CLI-Verhalten, öffentliche APIs,
Supportaussagen, Build-Flags oder Testbefehle ändern.

## Tests ausführen

Führen Sie alle registrierten Tests über das Aggregatziel aus:

```bash
cmake --build build-release --target check-neverd
```

Nutzen Sie während der Entwicklung das kleinste relevante Ziel oder CTest-Label:

```bash
# Main Unicorn differential suite
cmake --build build-release --target check-neverd-semantic

# Lifter/loader/format binary only
cmake --build build-release --target NeverDLiftTests --parallel 4
ctest --test-dir build-release --build-config Release \
  -L '^NeverDLiftTests$' --output-on-failure --parallel 4
```

Der [Testleitfaden](../testing.de.md) dokumentiert alle Komfortziele,
nur per Label auswählbare Transformationssuiten, reguläre Ausdrücke für einen
einzelnen Test, die Fixture-Kompilierung und Unicorn-Roundtrips. Wird ein Ziel
wegen eines fehlenden Cross-Compilers oder Linkers übersprungen, melden Sie
diese Einschränkung; beschreiben Sie den nicht ausgeführten Pfad nicht als
bestanden.

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
- Aktualisieren Sie öffentliche und Mitwirkenden-Dokumentation, wenn sich
  Verhalten, Support, Flags, Befehle oder Testzuständigkeit ändern.

Folgen Sie für sicherheitsrelevante Meldungen, die nicht als öffentlicher Pull
Request beginnen sollten, der [SECURITY.md](../../SECURITY.md).
