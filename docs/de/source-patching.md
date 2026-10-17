**Languages**: [English](../source-patching.md) | [简体中文](../zh-CN/source-patching.md) | [繁體中文](../zh-TW/source-patching.md) | [日本語](../ja/source-patching.md) | [한국어](../ko/source-patching.md) | [Français](../fr/source-patching.md) | [Deutsch](source-patching.md) | [Español](../es/source-patching.md) | [Italiano](../it/source-patching.md) | [Русский](../ru/source-patching.md) | [العربية](../ar/source-patching.md)

<!-- i18n-source: 1eae101089e730c29ba3fda92db515fe148fe41b758d6f5b69156252591d9b3f -->

[← Documentation Index](README.md)

# Quellcode- und native Objektpatches

NeverD kompiliert die Ersatzfunktion mit dem ausgewählten Compiler und installiert sie am ausdrücklich angegebenen ursprünglichen Funktionseinstieg. Der Compiler bestimmt die C/C++-Semantik; NeverD übernimmt Adressbindung, Relokationen, Speicher, Unwind-Daten und Veröffentlichung. Binärreparatur und Umschreiben gelifteten Codes behalten ihre eigenen Pipelines. Das übrige Programm wird nicht neu gebaut.

<!-- i18n-section: compiler -->
## Compiler auswählen

Wählen Sie `--compiler=clang`, `--compiler=gcc` oder `--compiler=neverc`. Clang und NeverC im normalen Modus erzeugen LLVM IR; `object` erhält native Maschinenbefehle. Die CLI wählt für GCC standardmäßig Objekte. Das Python-SDK wählt für GCC ebenfalls Objekte. Ein anderer Compiler wird niemals stillschweigend eingesetzt.

Übergeben Sie Include-Pfade, Makrodefinitionen und unterstützte Quelloptionen mit `--compiler-arg` oder `compiler_args` im SDK. Änderungen an Ziel oder Ausgabemodus sowie Antwortdatei-Token (`@file`) werden an jeder Argumentposition abgelehnt, auch als separate Optionswerte. Gewöhnliche `-I`-Pfade und `-D`-Definitionen bleiben unterstützt.

Ersetzen Sie Beispieladressen durch bestätigte ursprüngliche Funktionseinstiege. Die Deklaration muss zur ursprünglichen Maschinen-ABI passen, einschließlich Aufrufkonvention und Strukturaufbau; eine Adresse beweist dies nicht. C++ benötigt `--language=c++` und das passende Clang++/G++. Verwenden Sie einen `extern "C"`-Einstieg oder den exakten dekorierten Symbolnamen. Zusätzliche Hilfsfunktionen sind erlaubt. `--entry` und `--func` wählen den Ersatz ausdrücklich; auch das ältere `--from-c` beachtet die Adresse.

`artifact="object"` lässt sich weiterhin explizit wählen.

```sh
# Clang C -> LLVM IR
neverd patch app --from-source=replacement.c --entry=replacement \
  --func=0x401140 --compiler=clang --compiler-path=/path/to/clang \
  --patch-report -o app.clang

# NeverC ordinary C -> native object
neverd patch app --from-source=replacement.c --entry=replacement \
  --func=0x401140 --compiler=neverc --compiler-path=/path/to/neverc \
  --artifact=object --patch-report -o app.neverc

# GCC -> native object (CLI default for GCC)
neverd patch app --from-source=replacement.c --entry=replacement \
  --func=0x401140 --compiler=gcc --compiler-path=/path/to/gcc \
  --patch-report -o app.gcc

# Independently compiled ELF x64 object
clang --target=x86_64-unknown-linux-gnu -fPIC -fno-lto -c replacement.c -o replacement.o
neverd patch app --from-object=replacement.o --entry=replacement \
  --func=0x401140 --patch-report -o app.object
```

<!-- i18n-section: profiles -->
## Geprüfte Profile und Grenzen

[NeverC producer: 182fd2c5dc](https://github.com/NeverSight/NeverC/commit/182fd2c5dc)

**NeverC C und hosted DynCode**

NeverC akzeptiert ausschließlich reinen C-Quelltext auf ELF, PE und Mach-O für x64/ARM64. NeverD lehnt NeverC mit `--language=c++` vor dem Compilerstart ab; es übersetzt nicht implizit und wechselt den Compiler nicht. C++ und Ausnahmen verwenden Clang oder einen passenden GCC-Treiber für das geprüfte ELF-Objektprofil. Hosted-C-Rahmen können mit echten Unwind-Daten eine vom umgebenden C++-Programm ausgelöste und gefangene Ausnahme weitergeben. Das ist keine C++-Sprachunterstützung in NeverC.

`dyncode`: `--compile-mode=dyncode` verwendet `-fdyncode-hosted`: Quelle, ein expliziter Einstieg, natives Objekt und keine Originalbindungen. CLI und Python wählen dafür und für GCC standardmäßig Objekte. Beide Hashes, sämtliche gelinkten Codebytes und der Einstieg werden vor Veröffentlichung geprüft; echte CFI/SEH oder vollständige native Compact-Daten liefern Unwind-Abdeckung. Veränderlicher/TLS-Zustand, implizite syscall/PEB/heap/string-Umwandlung, ungelöste Imports und umgeordneter Code werden abgelehnt. Daten und Callback-Zeiger können Parameter sein.

```sh
neverd patch app --from-source=replacement.cpp --language=c++ \
  --compiler=clang --compiler-path=/path/to/clang++ \
  --entry=replacement --func=0x401140 --patch-report -o app.cpp
neverd patch app --from-source=replacement.c --compiler=neverc \
  --compiler-path=/path/to/neverc --compile-mode=dyncode \
  --entry=replacement --func=0x401140 --patch-report -o app.dyncode
```

Clang LLVM IR unterstützt Windows ARM64 C++ FH3 mit geprüfter xdata-Zuordnung, Elternframe-ABI und funclet/FuncInfo-Gruppen. Native Objekt-Sprachhandler bleiben ausgeschlossen. `check_source_patch_cpp_runtime.py` führt neun Szenarien auf nativem Windows x64/ARM64 aus; CPU- und OS-Unwinder-Belege bleiben getrennt.

| Compiler | Artefakt | Geprüftes OS-Laufzeitprofil |
|---|---|---|
| Clang 22.1.x | LLVM IR | ELF x64/ARM64 C/C++; Mach-O x64/ARM64 C/C++; PE x86/x64/ARM64 C; PE x64/ARM64 C++ FH3; ELF x86 C (`compatibility`, `qemu-user`); ELF ARM32 C (ARM/Thumb, `compatibility`, `qemu-user`) |
| Clang 22.1.6 | Natives Objekt | ELF x64/ARM64 C/C++; PE x86/x64/ARM64 C; Mach-O x64/ARM64 C; ELF x86 C (`compatibility`, `qemu-user`); ELF ARM32 C (ARM/Thumb, `compatibility`, `qemu-user`) |
| GCC/G++ 13.3.0 | Natives Objekt | ELF x64/ARM64 C/C++ mit kompatibler Itanium-Laufzeit; ELF x86 C (`compatibility`, `qemu-user`); ELF ARM32 C (ARM/Thumb, `compatibility`, `qemu-user`) |
| NeverC 3389.1.5 normaler Modus | LLVM IR | ELF x64 C; Mach-O x64/ARM64 C |
| NeverC 3389.1.5 normaler Modus | Natives Objekt | ELF x64 C |
| NeverC 3389.1.5 C | LLVM IR / natives Objekt | ELF / PE / Mach-O x64/ARM64: reiner C-Quelltext |
| NeverC 3389.1.5 `hosted-v1` | Natives Objekt + versiegelte Nutzlast | ELF / PE / Mach-O x64/ARM64: C-Quelltext und Ausnahmeweitergabe durch C-Rahmen |

Die Tabelle nennt geprüfte OS-Laufzeitprofile, keine Zusage für jede Compiler-Version oder Option. Die nativen Installer für ELF, PE und Mach-O akzeptieren alle vier Architekturen. GCC benötigt einen passenden ELF-Treiber und erzeugt kein LLVM IR. Unbekannte Relokationen, TLS, globale Konstruktoren beim Laden, LTO, common/IFUNC-Symbole und unaufgelöste Imports führen zu klaren Fehlern. Der JSON-Beleg enthält Compiler, Argumente, Bindungen und Adressen; native Objekte erhalten zusätzlich SHA-256.

Das Feld `target` im Beleg eines nativen Objekts folgt der ABI des geladenen Images, einschließlich der ARM-ELF-Gleitkommaflags und der unterschiedlichen iOS/watchOS-Plattformen. Kompilierte und bereitgestellte Objekte verwenden dieselbe Identitätsregel.

Die CPU-Matrix verlangt erfolgreiche Installation von Quellcode, bereitgestelltem IR und nativen Objekten in allen zwölf Kombinationen. Sie prüft ARMv7k watchOS und ARMv7 iOS getrennt sowie veränderliche／konstante Daten, Fließkomma- und Stackargumente originaler Funktionen und Einsprünge nach Änderung der Ladeadresse. CPU-Ausführung bestätigt weder OS-Loader noch C++-Unwinder oder beliebige ABIs; dafür gelten die OS-Profile oben.

| Format | `x86` | `x64` | `ARM32` | `ARM64` |
|---|---|---|---|---|
| `ELF` | `CPU` | `CPU` | `CPU` | `CPU` |
| `PE` | `CPU` | `CPU` | `CPU` | `CPU` |
| `Mach-O` | `CPU` | `CPU` | `CPU` | `CPU` |

`CPU` bedeutet Quellcode／IR／Objekt-Installation, erneutes Laden und exaktes Funktionsergebnis. ARM ELF wählt EABI5, ARM/Thumb-Modus und Fließkomma-ABI anhand des Images und registriert einen vollständigen sortierten `EHABI`-Index. PE bewahrt x64/ARM-Laufzeitzeilen und `HIGHLOW`/`DIR64`/`MOV32T`-Basisrelokationen; x86 verwendet Compiler-Registrierungscode. Mach-O unterscheidet watchOS DWARF und iOS `SjLj` anhand des CPU-Untertyps. ARM/Thumb-Einsprungzweige sind relativ und überstehen Rebasierung.

Native Objekte verwenden eigene begrenzte Funktionssymbole, Hilfs／Abschnittsdefinitionen und Unwind-Daten; MC-Besitzer-IDs werden nicht erfunden und IR wird nicht neu kompiliert. COFF-Funktionsbereiche erfordern genaue Hilfsgrößen, Abschnitte mit einer Funktion (`-ffunction-sections`) oder dekodierte native Laufzeitfunktionseinträge; die Größe wird nie aus dem nächsten Symbol geschätzt. Der watchOS-Adapter behält Blattfunktionsrahmen für echte Unwind-Einträge. Native COFF/Mach-O-Sprachhandler gehören nicht zum geprüften Objektprofil; C++-Ausnahmen verwenden den geprüften IR-Pfad. Statische ARM-Unwinder mit festem exidx-Register, unregistrierte absolute Mach-O-Zeiger und nicht unterstützter RELRO-Speicher werden abgewiesen.

GCC-C-Objekte wurden auch auf ELF x86, ARM32 und ARM64 installiert und mit Linux QEMU user mode ausgeführt. Der Adapter wählt ARM/Thumb anhand des Images; GNU-Property-Einträge folgen der ELF-Zeigerbreitenausrichtung.

<!-- i18n-section: references -->
## Originalaufrufe, Layouterweiterung und neue Abhängigkeiten

Bindungen benennen externe Deklarationen: `function` authentifiziert eine Originalfunktion oder einen Importstub, `data` verlangt beschreibbaren Speicher und `const` eine konstante IR-Deklaration. Objektdaten benötigen eine Ausdehnung ungleich null; Speichergrenzen und RELRO-Schreibschutz dürfen nicht überschritten werden. Mit `original` wird ausdrücklich der gesicherte Rumpf der ersetzten Funktion aufgerufen. Der öffentliche Einstieg ruft weiterhin den Ersatz auf; der gesicherte Einstieg führt ein aus vollständigen Anweisungen relocatiertes Präfix aus und setzt im Original fort. Quelltext, gelieferte IR und native Objekte teilen diesen Vertrag einschließlich Gleitkomma-/Stackargumenten und mehrerer Rückrufe.

Rückrufe verlangen authentifizierte Funktionsgrenzen, Befehlsmodus, Fortsetzung und Unwind-Nachweise. Unbekannte Befehle, unsichere PC-relative Referenzen, Sprünge in das kopierte Präfix, nicht unterstützte Loader-Relokationen, überlappende Rückrufe und nicht modellierte Prologe mit Handlern werden vor Veröffentlichung abgelehnt. Die passende ABI-Deklaration bleibt Verantwortung des Aufrufers. Der Beleg erfasst gesicherte Einstiege unter `original_callbacks`.

PE32/PE32+ kann eine volle Abschnittskopftabelle erweitern, indem Dateibereiche verschoben und modellierte Debug-, Symbol-, Zertifikats- und Overlay-Zeiger aktualisiert werden; die ursprünglichen RVAs bleiben erhalten. Mach-O x64/ARM64 PIE kann bei benachbartem page-zero-Schutzbereich und abschließendem linkedit-Segment durch eine geprüfte Header-Präfixerweiterung Platz für Befehle und Unwind-Daten gewinnen; ursprüngliche Code-/Datenadressen bleiben fest. Dieses Profil benötigt weder `-Wl,-headerpad,0x400` noch angrenzende freie Unwind-Seiten. Explizite Headeradressreferenzen, unbekannte Ladebefehle, gemischte Code-/Datenzuordnung, fehlerhafte Bereiche und nicht unterstützte Layouts werden abgelehnt. Header- und Unwind-Kapazität werden vor der Installation gemeinsam geplant.

Neuer Code bleibt RX, veränderlicher Zustand RW. Explizite `--dependency`-Anfragen ergänzen Funktions-/Datenimporte über dynamische ELF-Metadaten, PE-Importdeskriptoren/IAT oder Mach-O-dylib-/bind-Metadaten. Bestehende Importidentitäten, Ordinalzahlen und Versionen bleiben stabil; Slots und aufrufbare Stubs behalten getrennte Rollen. Bibliotheksname/-pfad muss zum OS-Loader des Ziels passen. Das Beispiel verwendet einen ELF-Bibliotheksnamen. Fehlende Bibliotheken oder Symbole müssen beim Ausführen echte Loaderfehler auslösen. TLS, IFUNC, neue Symbolversionsverträge, Konstruktoren beim Laden und nicht modellierte Relokationen bleiben ausgeschlossen; ELF `.data.rel.ro` und CET/IBT erfordern weitere Verträge. Native COFF-Objekte müssen neue Funktions-/Datenimporte mit `__declspec(dllimport)` deklarieren, damit ihre Relokationen die IAT benennen. Gewöhnliche direkte externe Referenzen werden abgelehnt.

```sh
# ELF example: the original target is at 0x401140.
neverd patch app --from-source=replacement.c --entry=replacement --func=0x401140 \
  --bind=original:saved_target=0x401140 \
  --bind=function:original_helper=0x401190 \
  --bind=data:shared_counter=0x404020:4 \
  --bind=const:lookup_table=0x402040:64 \
  --dependency=function:extra_function=libpatch_extra.so \
  --dependency=data:extra_state=libpatch_extra.so \
  --patch-report -o app.bound
```

Die am Strukturende ergänzten, größenabhängig geprüften Abhängigkeitsoptionen gelten für Quelltext, gelieferte IR und native Objekte. Der Beleg enthält `added_imports`; nach erneutem Laden werden alle Rückrufe, Importe, Speicherrechte und installierten Bytes nochmals geprüft. Fehler bewahren Eingabe und bestehende Zieldateien.

<!-- i18n-section: examples -->
## Ausführbare Beispiele

[Vollständige Beispiele für Windows/macOS/Linux](../../examples/source-patch/README.md) enthalten Quellcode für Original, Ersatz und Bibliothek, Build-Befehle und ein echtes Python-SDK-Plugin im Host. Aus dem Repository-Hauptverzeichnis mit einem Python-fähigen NeverD-Build und einem neuen Ausgabeverzeichnis ausführen.

```sh
# Linux / macOS
python3 examples/source-patch/run.py --neverd build-example-host/bin/neverd --clang clang --output build-example-native
```

```powershell
# Windows: Visual Studio developer PowerShell
python examples/source-patch/run.py --neverd build-example-host/bin/neverd.exe --clang clang --output build-example-native
```

Standardmäßig laufen beide Wege; mit `--api cli` oder `--api sdk` lässt sich einer wählen. Die Zieladresse wird automatisch ermittelt. Bei Eingabe `7` gibt das Original `8` und jeder Patch `36` aus. Der Treiber prüft Ausführung, Belege und echte Ladefehler bei fehlender Bibliothek; vollständige Build- und Patch-Befehle stehen in `commands.json`.

<!-- i18n-section: exceptions -->
## C++-Ausnahmen

ELF verwendet Compiler-FDE/LSDA; Mach-O verwendet compact unwind/DWARF. PE x64/ARM64 akzeptiert Compiler-FH3-Tabellen mit vorhandener kompatibler MSVC-Laufzeit und passenden Typidentitäten. Type-info-Daten können ausdrückliche Bindungen erfordern. FH4, eigene Personalities, SEH-Quellcode und aktive CFG/EHCont/XFG-Modi werden abgelehnt; ursprüngliche Schutzflags bleiben erhalten.

ELF-x86/x64-Originale mit IBT-Anforderungen werden derzeit abgewiesen: Der Quellcode-Installer bietet noch keinen Trampolin, der ENDBR erhält.

Laufzeitfälle prüfen normale Rückkehr, internes catch, neue Ausnahmen im ursprünglichen Aufrufer, Ausnahmen eines ursprünglichen Aufgerufenen durch den Ersatz, genau einmalige RAII-Bereinigung, erneutes Werfen, noexcept-Beendigung und unveränderte Funktionen. Die ursprüngliche Aufrufstelle muss Weitergabe bereits zulassen. Entfernte catches um eine bewiesen nicht werfende Funktion kann ein Patch nicht wiederherstellen.

<!-- i18n-section: publication -->
## SDK und Veröffentlichung

Die additive C-API ist `neverd_patch_source` mit `neverd_source_patch_options_v1`; `neverd_patch_source_report` liefert den Beleg. Objektinput benötigt seine Bytelänge. Die ältere API `neverd_patch_from_c` bleibt verfügbar.

Eingaben werden als Snapshot gesichert. Der Kandidat entsteht neben dem Ziel, wird erneut geladen und vor exklusiver Veröffentlichung gegen jeden Einstieg geprüft. Vorhandene Dateien und Symlink-Aliasse werden nicht überschrieben. Eine fehlende Zuordnung lässt die ganze Anfrage scheitern. `--mode=inplace` wird abgelehnt. IR-Transformationen der Sitzung gelten nicht für native Objekte. Mach-O prüft und erneuert Signaturen strikt; PE-Signaturen werden nicht stillschweigend entfernt.

```python
report = session.patch_source(
    replacement_source, "app.patched",
    mappings={"replacement": original_address},
    compiler="clang", compiler_path="/path/to/clang", artifact="object",
    bindings={
        "saved_target": ("original", original_address),
        "shared_counter": ("data", original_counter_address, 4),
    },
    dependencies={
        "extra_function": ("libpatch_extra.so", "function"),
        "extra_state": ("libpatch_extra.so", "data"),
    },
)
```

<!-- i18n-section: verification -->
## Build und Prüfung

Die sechs ausgewählten nativen OS/ISA-Jobs bestanden ohne Fehler oder Überspringen: 3669 GoogleTest-Ausführungen, 1008 native Prozessbeobachtungen (504 funktionale und 504 C++-Ausnahmebeobachtungen), 12 echte Loader-Ablehnungen, 96 Images und 84 Veröffentlichungsbelege.

| Host | Quelle | Harness | CI-Job | Tests | Beobachtungen / Ablehnungen |
|---|---|---|---|---|---|
| Linux x64 | [61fb1d3e759](https://github.com/NeverSight/NeverD/commit/61fb1d3e759f6796d23341782e7f4d624295f27c) | [197126208b6](https://github.com/gmh5225/test_mac_intel/commit/197126208b665bec0db5c6dc74278931300229af) | [116845056048](https://github.com/gmh5225/test_mac_intel/actions/runs/38861651227/job/116845056048) | 626 | 168 / 2 |
| Linux ARM64 | [61fb1d3e759](https://github.com/NeverSight/NeverD/commit/61fb1d3e759f6796d23341782e7f4d624295f27c) | [197126208b6](https://github.com/gmh5225/test_mac_intel/commit/197126208b665bec0db5c6dc74278931300229af) | [116845055863](https://github.com/gmh5225/test_mac_intel/actions/runs/38861651227/job/116845055863) | 600 | 168 / 2 |
| macOS Intel | [b7a180c7ac3](https://github.com/NeverSight/NeverD/commit/b7a180c7ac35fc2f05be3568a86104d42fc32800) | [cba0906a114](https://github.com/gmh5225/test_mac_intel/commit/cba0906a11461d6e29bbe68f9337515fe084b610) | [117185095063](https://github.com/gmh5225/test_mac_intel/actions/runs/38963782346/job/117185095063) | 624 | 168 / 2 |
| macOS ARM64 | [61fb1d3e759](https://github.com/NeverSight/NeverD/commit/61fb1d3e759f6796d23341782e7f4d624295f27c) | [f6311a611e0](https://github.com/gmh5225/test_mac_intel/commit/f6311a611e095bcffb7537c7ae3b40c826d1e2e1) | [116879179194](https://github.com/gmh5225/test_mac_intel/actions/runs/38872352550/job/116879179194) | 624 | 168 / 2 |
| Windows x64 | [76ce83e138d](https://github.com/NeverSight/NeverD/commit/76ce83e138de639b8d75206c78bd5ccbb9cfd8dd) | [a42b117681f](https://github.com/gmh5225/test_mac_intel/commit/a42b117681f81d922e09d98ccb1a1c4525055947) | [116908799030](https://github.com/gmh5225/test_mac_intel/actions/runs/38881414688/job/116908799030) | 598 | 168 / 2 |
| Windows ARM64 | [76ce83e138d](https://github.com/NeverSight/NeverD/commit/76ce83e138de639b8d75206c78bd5ccbb9cfd8dd) | [cba0906a114](https://github.com/gmh5225/test_mac_intel/commit/cba0906a11461d6e29bbe68f9337515fe084b610) | [117014878954](https://github.com/gmh5225/test_mac_intel/actions/runs/38913475295/job/117014878954) | 597 | 168 / 2 |

Die Artefakte qualifizieren genau die unten genannten Quell- und Harness-Commits. Spätere dev-Integrations- und Dokumentationscommits haben eigene lokale und Dokumentationsprüfungen. Frühere fehlgeschlagene oder abgebrochene Jobs bleiben Fehlschläge. Die historischen 216 ergänzenden Beobachtungen sowie PE ARM32 und ältere 32-Bit-Apple-CPU-/Kompatibilitätsergebnisse belegen keine neue native Hardware- oder C++-Ausnahmeabdeckung.

Die Windows-ARM64-Qualifikation nutzt eine native ARM64-Unicorn-C-DLL, tatsächlich aus dem festgelegten Submodul mit verifiziertem LLVM-MinGW gebaut, eine MSVC-Importbibliothek und eine explizite CMake-Zielersetzung. Die isolierte Build-Kopie authentifiziert drei Hostkorrekturen: das Sprungfunktionspaar ohne SEH, einen ARM64-Seiten-Commit-Callback mit Befehls-Cache-Synchronisierung und 64-Bit-Sofortwertmasken für Windows LLP64. Submodulquellen und Revision bleiben unverändert. Dies qualifiziert den Testhost für die CPU-Matrix; standardmäßige MSVC-ARM64-JIT-Unterstützung folgt daraus nicht. Die fünf Host-Wiederherstellungsprüfungen werden von nativen Patchprozess- und C++-Ausnahmenachweisen getrennt geführt.

Wählen Sie über `NEVERD_SOURCE_PATCH_TEST_CLANG_EXECUTABLE` einen kompatiblen Compiler (Clang 22.1.6 geprüft). Der integrierte Build korrigiert nur eine LLVM-Kopie im Buildverzeichnis; Submodul und gemeinsamer Cache bleiben unverändert. Der x64-PLT-Fehler der r3/r4-Pakete kann IR-Patches verhindern; native Objekte umgehen diesen MC-Pfad. `NEVERD_TEST_REQUIRE_FIXED_LLVM=1` verlangt die exakte korrigierte PLT-Erzeugung.

`NeverDSourcePatchTests` enthält native C/API/Objekt/EH-Fälle; `NeverDSourcePatchMatrixTests` prüft alle Kombinationen mit Unicorn. `SourcePatchPE` führt sieben C++-Szenarien mit `NEVERD_TEST_WINDOWS_CRT_LIB`, `NEVERD_TEST_WINDOWS_UCRT_LIB` und `NEVERD_TEST_WINDOWS_SDK_LIB` aus. Außerhalb Windows ist zusätzlich `NEVERD_TEST_WINDOWS_RUNNER` nötig. `NEVERD_TEST_REQUIRE_WINDOWS_RUNTIME=1` macht fehlende Eingaben zum Fehler. Der eigene Windows-Workflow verlangt native Ausführung und lehnt übersprungene Fälle ab.

Die Linux-Nachweise auf dem Entwicklungs-Mac nutzen Docker und einen expliziten Linux-Loader; PE nutzt Wine 11. Mach-O ARM64 läuft direkt auf dem Mac. Native Windows-CI-Ergebnisse müssen gesondert geprüft werden: Ein konfigurierter Job ist kein bestandener Lauf. Historische Zahlen, Baseline-Fehler und Emulatorgrenzen stehen in [den detaillierten Verträgen und Prüfaufzeichnungen](../source-patching-details.md).

```sh
cmake -S . -B build-source -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DBUILD_TESTING=ON -DNEVERD_LLVM_PREBUILT=OFF \
  -DNEVERD_ENABLE_SEMANTIC_TESTS=ON \
  -DNEVERD_SOURCE_PATCH_TEST_CLANG_EXECUTABLE=/path/to/clang
cmake --build build-source --target NeverDSourcePatchTests \
  NeverDSourcePatchMatrixTests --parallel 4
ctest --test-dir build-source -L \
  '^(NeverDSourcePatchTests|NeverDSourcePatchMatrixTests)$' \
  --output-on-failure --parallel 4
```

Die LLVM-Kopie korrigiert auch Mach-O-x64-GOT-Datenreferenzen: Clangs typisierte catch-Tabelle ist relativ zum Ende des vier Byte langen Relokationsworts. r3/r4 verschiebt sie um vier Byte; die Prüfung vor Veröffentlichung lehnt das ab. Auf dem Entwicklungs-Mac läuft x64 unter Rosetta 2 und ARM64 direkt.

Die native Windows-x64-Prüfung [2ed7e78fc](https://github.com/NeverSight/NeverD/actions/runs/38307461805) bestand alle 16 gezielten PE/Objekt/trampoline-Tests und alle 143 Tests der CPU-Zielmatrix ohne übersprungene Fälle. Der FH3-Test führte sieben C++-Laufzeitszenarien aus; die Zielmatrix verwendet Unicorn. Dieser Lauf verwendete Clang 22.1.6 mit nativem MSVC, Windows SDK und integriertem LLVM.

Der Windows-Workflow erzeugt außerdem 27 PE-Ersetzungen für x86, x64 und ARM64. `scripts/check_source_patch_pe_runtime.py` prüft Nicht-Blattaufrufe, veränderlichen Zeigerzustand sowie Gleitkomma- und Stackargumente über Quell-IR, kompilierte und bereitgestellte Objekte. Jedes Original und jedes Patch-Ergebnis wird dreimal ausgeführt: 30 Prozessbeobachtungen je Architektur. x86/x64 laufen unter Windows x64, ARM64 auf `windows-11-arm`. Die Prüfung verlangt den erwarteten nativen Host, übereinstimmende Commits und Dateiprüfsummen sowie sämtliche Testfälle; externe Laufzeitumgebungen werden getrennt erfasst. Dieses Profil belegt keine Unterstützung für ARM64-C++-Exceptions.

`NEVERD_TEST_MATRIX_NEVERC` wählt den optionalen Compiler der Matrix. NeverC 3389.1.5 bestand alle 66 Prüfungen für x64/ARM64 mit ELF, PE und Mach-O, einschließlich IR-/Objektinstallation und CPU-Ausführung. Diese Version unterstützt weder x86 noch ARM32; verwenden Sie dort Clang oder für ELF einen passenden GCC-Treiber. Bei PE-Kompilierung mit NeverC ergänzt der Adapter `-fno-autolink`: Referenzen werden im bestehenden Image aufgelöst, ohne eine weitere CRT zu linken. Bereitgestellte Objekte mit `.drectve`-Linkeranforderungen werden weiterhin abgelehnt.

Die native Windows-Prozessprüfung [a4a5ae3cb](https://github.com/NeverSight/NeverD/actions/runs/38350079817) bestand alle 90 Beobachtungen: jeweils 30 für x86, x64 und ARM64 über Quell-IR, kompilierte und bereitgestellte Objekte. x86/x64 liefen unter Windows x64; ARM64 lief auf `windows-11-arm` mit denselben durch Prüfsummen bestätigten Images. Derselbe Lauf bestand außerdem alle 16 gezielten PE/Objekt/trampoline-Prüfungen und alle 143 Unicorn-Zielmatrixprüfungen ohne übersprungene Fälle. Der FH3-Test behielt seine sieben x64-C++-Laufzeitszenarien bei.

Die native Qualifikation verwendet NeverC `182fd2c5dc`, unabhängige Originale und den aktuellen NeverD-Installierer. `check_source_patch_hosted_pe_runtime.py` verlangt je Windows-Architektur 36 Beobachtungen für C-IR/Objekte/Hosted-Ergebnisse und Ausnahmeweitergabe durch zwei C-Rahmen. `check_source_patch_elf_runtime.py` verlangt je Linux-Architektur 120 Beobachtungen für C, Hosted-Weitergabe und neun Clang-C++-Ausnahmeszenarien. Belege, Hashes und tatsächliche Hostarchitektur müssen übereinstimmen; NeverC selbst wird damit nicht auf dem CI-Host ausgeführt. Unter Linux x64 und ARM64 laufen die neun Ausnahmeszenarien jeweils mit Clang-IR, nativen Clang-Objekten und nativen GCC-Objekten. NeverC bleibt auf C beschränkt.

Die 204 Beobachtungen des früheren Übersetzungsprofils sind historische Belege und weisen den aktuellen C-Vertrag nicht nach. Die drei aktuellen NeverC-Wege verwenden C; Clang C++ FH3 wird getrennt geprüft. Auf dem Entwicklungs-Mac mit Apple Silicon nutzt macOS x64 Rosetta 2. Unicorn/QEMU ersetzen keine nativen Betriebssystem- und Unwinder-Prüfungen.

AArch64-Instruktionsrelokationen benötigen explizite RELA-Addenden. Implizite REL-Instruktionsaddenden werden vor der Veröffentlichung abgelehnt; ihre Befehlsbitfelder werden nicht als gewöhnliche Daten interpretiert. Unterstützte PREL64-Datenrelokationen behalten bei REL und RELA die vollständigen vorzeichenbehafteten Addenden.

Die C-Produzentenqualifizierung [cc91afb3b](https://github.com/NeverSight/NeverD/actions/runs/38591930213) bestand alle 312 nativen Prozessbeobachtungen: je 36 unter Windows x64 und ARM64 sowie je 120 unter Linux x64 und ARM64. Linux prüft neun Ausnahmeszenarien getrennt für Clang IR, native Clang-Objekte und native GCC-Objekte; alle NeverC-Eingaben für IR, Objekte und hosted DynCode sind C.

Die Qualifizierung des neu gebauten Installers [cc91afb3b](https://github.com/NeverSight/NeverD/actions/runs/38591928600) bestand 19 PE-/Objektprüfungen, alle 143 CPU-Matrixfälle und 474 native Prozessbeobachtungen unter Windows/Linux. Windows-x86-Prozesse liefen auf Windows x64; x64 und ARM64 verwendeten passende Hosts. Die separate [Linux-Qualifizierung](https://github.com/NeverSight/NeverD/actions/runs/38585759996) bestand die ursprünglichen 108 Fälle sowie drei LLVM-C- und drei Clang/GCC-C++-Prüfungen ohne Fehler oder übersprungene Fälle.

Die [Integrationsqualifikation 0a6f6d768](https://github.com/NeverSight/NeverD/actions/runs/38627694097) bestätigte erneut alle 312 Prozessbeobachtungen auf nativem Windows/Linux mit x64 und ARM64. Alle 30 endgültigen Images entsprechen den zuvor qualifizierten Bytes; Quellcode-Prüfsummen und C-only-Veröffentlichungsbelege wurden erneuert und unabhängig geprüft.

Die erweiterte [Linux-Qualifikation bei 0a6f6d768](https://github.com/NeverSight/NeverD/actions/runs/38626866257) bestand 429 ausgeführte Prüfungen ohne Fehler oder Überspringen: die ursprünglichen 108 Fälle, 55 vollständige Swift-Prüfungen, 143 CPU-Zielprüfungen, 117 native Sitzungsprüfungen sowie je drei LLVM-C- und Clang/GCC-C++-Prüfungen. Auch der vollständige lokale Binärpatch-Testbestand bestand bei dieser Integration alle 22.961 Fälle. Native Prozesszahlen und CPU-Emulationsabdeckung bleiben getrennt.

Die [Qualifikation des neu gebauten Windows-Installers bei 0a6f6d768](https://github.com/NeverSight/NeverD/actions/runs/38627581732) bestand alle 182 gezielten Prüfungen und 474 nativen Prozessausführungen, ohne Fehler oder übersprungene Tests. Eine unabhängige Artefaktprüfung kontrollierte jeden Image-Hash, jede Architektur, jedes Szenario und jede Wiederholung. Nach der ABI-/Unwind-Integration bei `e1cac949f` bestand die vollständige Binärpatch-Suite erneut alle 22,961 Fälle, ohne Fehler oder übersprungene Tests. NeverC bleibt auf reines C beschränkt; C++-Ersetzungen verwenden Clang/GCC.

<!-- i18n-section: runtime-coverage -->
## Prozessqualifikation für macOS und 32 Bit

Der Workflow für Prozessabbilder verlangt native Ausführung auf macOS ARM64 (`macos-15`) und Intel x64 (`macos-15-intel`). Jede Architektur führt 96 Beobachtungen aus: Clang-C-Quelltext und Objekte, veränderlicher Zeigerzustand, Gleitkomma-/Stackargumente, neun Clang-C++-Ausnahmeszenarien sowie vorhandene reine C-Ausgaben von NeverC als IR/Objekt/hosted. Mach-O-Signaturen werden vor der Ausführung streng geprüft; installierter hosted-Code muss bytegenau der Compiler-Nutzlast entsprechen.

Derselbe Workflow verlangt 30 Windows-x86-Beobachtungen auf Windows x64, 48 Linux-x86-Beobachtungen auf Linux x64 und 96 Linux-ARM32-Beobachtungen über QEMU im Benutzermodus, je zur Hälfte ARM und Thumb. Die Linux-Prüfungen umfassen passende GCC-Objekte. Jedes Szenario wird dreimal ausgeführt. Berichte unterscheiden `native`, `compatibility`, `rosetta` und `qemu-user` und halten Ziel- und tatsächliche Hostarchitektur fest. Rosetta erfüllt keine native Intel-Anforderung.

Alle 14 CPU-Zielprofile bleiben durch die Matrix mit 283 Quelltext-/IR-/Objektprüfungen abgedeckt. PE ARM32, Mach-O i386, ARMv7 iOS und ARMv7k watchOS haben derzeit CPU-Qualifikation, ohne Nachweis für native OS-Lader oder C++-Unwinder. NeverC bleibt auf reines C für x64/ARM64 beschränkt; Clang und passende ELF-GCC-Treiber erzeugen die 32-Bit-Ausgaben. Der Prüfer baut oder verändert NeverC nicht.

Windows x64 und ARM64 führen außerdem jeweils 36 Clang-C++-FH3-Beobachtungen aus.

Veränderliche Mach-O-Zeiger werden während der Ausführung initialisiert; statische Adressinitialisierer benötigen weiterhin nicht unterstützte Laufzeit-Rebase-Registrierung und werden mit einem ausdrücklichen Ablehnungstest geprüft. ELF-COMDAT-Funktionen ohne Größenangabe werden nur akzeptiert, wenn die Signatur die einzige Funktion am Anfang ihres ausführbaren Abschnitts ist und CFI exakt den gesamten Abschnitt abdeckt. Innere Offsets, gemeinsame Abschnitte, Padding, fehlende Gruppen und fehlende CFI werden weiter abgelehnt.

ELF ARM und Thumb haben getrennte Profile. Quellcodeproduzenten erhalten explizit `-marm` oder `-mthumb`; native Funktionsbindungen erhalten das ursprüngliche Befehlszustandsbit. Die 32-Bit-ELF-Prozesse verwenden PIE-Abbilder mit echten dynamischen Relokationen. ARM32-QEMU-Ausführung zeichnet einen expliziten glibc-`--loader` und Sysroot auf: QEMUs AUXV-Berechnung folgt verschobenen Programmheadern anders als der Linux-Kernel. Dieser Weg prüft glibc-Laden und Ausführung unter QEMU, nicht physische ARM32-Hardware oder den nativen Kernel-Lader.

Für diese gepatchten ELF-Abbilder benötigen die Linux-x86- und ARM32-Wege mit `qemu-user` jeweils einen expliziten glibc-`--loader` aus dem zur Zielarchitektur passenden Sysroot. Linux x86 mit `compatibility` nutzt die 32-Bit-Unterstützung des Linux-x64-Hostkernels und wird getrennt von der QEMU-Ausführung erfasst; dies belegt keine Ausführung auf einem nativen 32-Bit-x86-Host.

Die [Prozessqualifikation](https://github.com/NeverSight/NeverD/actions/runs/38694434082) von `fcee81c9a5e2f20a71090ade23ae833175f64c59` bestand alle 750 Beobachtungen: 576 auf nativen Hosts mit passender Architektur, 78 im x86-Kompatibilitätsmodus und 96 für ARM/Thumb im QEMU-User-Modus. Eine unabhängige Artefaktprüfung bestätigte die Hashes aller 126 Images, die Ausführungsumgebungen und den vollständigen Szenarienbestand. Der [Windows-Neubau](https://github.com/NeverSight/NeverD/actions/runs/38694628242) bestand 194 gezielte Prüfungen und 474 Windows/Linux-Prozessbeobachtungen. Der [Linux-Neubau](https://github.com/NeverSight/NeverD/actions/runs/38694638358) bestand 444 Prüfungen einschließlich aller ursprünglich 62 fehlgeschlagenen und 46 übersprungenen Fälle, ohne Fehler oder übersprungene Tests. Die oben genannten Hardware- und Loader-Grenzen gelten weiterhin. NeverC bleibt auf reines C beschränkt; sein Repository wurde nicht geändert.

Die zusätzliche [ARM/Thumb-Validierung](https://github.com/gmh5225/test_mac_intel/actions/runs/38750504263) bestand alle 96 Beobachtungen (48 ARM, 48 Thumb) für die 32 C-Testabbilder aus `fcee81c9a5e2f20a71090ade23ae833175f64c59` auf `ubuntu-24.04-arm`. Der Linux-Kernel lud jedes Abbild direkt über `/lib/ld-linux-armhf.so.3`, ohne QEMU oder expliziten Loader-Aufruf. Dies ist `compatibility` auf einem ARM64-Host und keine Validierung auf einem nativen 32-Bit-Host. Der Commit des Testprogramms `3ad17ac9608d6bd9f5a4d44953ec375524e5730b` und der Abbild-Commit werden getrennt erfasst. Unabhängige Artefaktprüfungen bestätigten die Abbild-Hashes, alle 30 Veröffentlichungsbelege, sämtliche Szenarien und Wiederholungen sowie das Fehlen passender `binfmt_misc`-Interpreterregeln.

Separate [Prüfungen der gehosteten Runner](https://github.com/gmh5225/test_mac_intel/actions/runs/38749138719) zeigten, dass der bereitgestellte Windows-11-ARM64-Runner ARM32-Programme mit `WinError 216` zurückweist. Unter macOS 15.7.9 mit Xcode 16.4 konnten die vorhandenen SDKs keine macOS-i386- oder iOS-ARMv7-Programme linken. Ein watchOS-ARMv7k-Programm wurde erfolgreich gelinkt, doch eine Ausführung auf einem physischen Gerät wurde nicht nachgewiesen. Erfolgreiche Prüfjobs validieren diese Plattformen nicht; die native Hardwareabdeckung bleibt offen. NeverC wurde weder gebaut noch verändert.

Die ergänzenden [Linux-Prüfungen in den persönlichen Actions](https://github.com/gmh5225/test_mac_intel/actions/runs/38773830274) bestanden 96 ARM/Thumb-Ausführungen in einem vollständigen QEMU-TCG-ARM32-Gast mit 32-Bit-Linux-Kernel und glibc sowie 30 PE-ARM32-Ausführungen über Wine im selben Gast. Der Gastkernel lädt die ELF-Abbilder direkt. Diese Systememulation wird vom `qemu-user`-Pfad mit explizitem Loader getrennt erfasst. Die Installation der Wine-Registry in der Kompatibilitätsschicht des ARM64-Hosts wird gesondert dokumentiert und zählt als null Patch-Ausführungen. Die Wine-Initialisierung im Gast und jeder gemessene Start müssen innerhalb der ursprünglichen Zeitgrenzen erfolgreich sein.

Die [Apple-Kompatibilitätsprüfungen](https://github.com/gmh5225/test_mac_intel/actions/runs/38771214358) bestanden 30 Mach-O-i386-Ausführungen unter Darling auf Linux und 60 ARMv7/ARMv7k-Ausführungen unter touchHLE auf Intel-macOS. Jedes Original und jeder Ersatz aus Quell-IR, kompiliertem Objekt oder bereitgestelltem Objekt wird dreimal ausgeführt. Unabhängige Audits prüften alle 72 Abbilder, 66 Veröffentlichungsbelege und 216 Ausführungen dieser zusätzlichen Läufe einschließlich GitHub-Artefaktgrößen, SHA256, exakter Commits und vollständiger Falllisten. Die Linux-Eingaben bleiben auf `fcee81c9a5` festgelegt; PE-Eingaben wurden bei `a542f97ba`, Mach-O-Eingaben bei `8f5f06762` erzeugt. Die Commits der Testskripte sind in den verlinkten Läufen separat erfasst.

Die Mach-O-Korrekturen authentifizieren ausgewählte Thumb-Funktionen unabhängig von der gemischten ARM/Thumb-Einstufung des gesamten Abbilds. Sie normalisieren belegte Thumb-Exportzeiger und ARM32-Unwind-Codeadressen, ohne Datenzeiger oder Bereichslängen zu ändern. Der Build mit integrierten LLVM-Quellen bestand lokal sämtliche 283 CPU-Matrixprüfungen und 258 Prüfungen der Ausnahmeverträge, ohne Fehler oder übersprungene Fälle. Die alten Mach-O-Proben verwenden einen dokumentierten quelloffenen Linker und reservieren Unwind-Speicher; i386 besitzt einen korrekt ausgerichteten Starteinstieg und ein 64-KiB-Page-Zero-Segment.

Diese Ergebnisse belegen C-Ausführung in den genannten Emulatoren und Kompatibilitätsschichten. Wine ist kein Microsoft Windows, Darling kein macOS. ARMv7k-Ausführung in der iOS-HLE qualifiziert weder watchOS noch WatchKit oder Apple-Geräte. Daraus folgt keine Abdeckung alter Apple-SDKs oder nativer 32-Bit-Hardware; auch C++-Exception-Unwinding wird durch diese Proben nicht qualifiziert. Frühere native und Kompatibilitätsnachweise bleiben getrennt. NeverC wurde weder gebaut noch verändert.
