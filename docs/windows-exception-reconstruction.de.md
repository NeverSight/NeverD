**Sprachen**: [English](windows-exception-reconstruction.md) | [简体中文](windows-exception-reconstruction.zh-CN.md) | [繁體中文](windows-exception-reconstruction.zh-TW.md) | [日本語](windows-exception-reconstruction.ja.md) | [한국어](windows-exception-reconstruction.ko.md) | [Français](windows-exception-reconstruction.fr.md) | [Deutsch](windows-exception-reconstruction.de.md) | [Español](windows-exception-reconstruction.es.md) | [Italiano](windows-exception-reconstruction.it.md) | [Русский](windows-exception-reconstruction.ru.md) | [العربية](windows-exception-reconstruction.ar.md)

# Windows-Ausnahmerekonstruktion

[← Dokumentationsindex](README.de.md)

NeverD trägt tabellenbasierte Windows-Ausnahmeinformationen durch Laden, Lift,
Dekompilierung und binäres Umschreiben. Die Metadaten sind Teil des ausführbaren
Funktionsvertrags: Kann die Konsistenz von generiertem Code, Runtime-Function-
Records, Sprachtabellen und Guard-Tabellen nicht bewiesen werden, wird die
Umschreibung abgelehnt.

Es gibt drei Supportstufen:

- **Analyse**: Native Darstellung wird geprüft, normalisiert und der IR-Pipeline bereitgestellt.
- **Dekompilierung**: Reduzierbare Schutzbereiche werden explizite HighIR-
  Ausnahmeknoten; andere Formen behalten deterministische native Annotationen.
- **Native Rekonstruktion**: Patch mode lässt LLVM einen vollständigen Ersatzvertrag
  erzeugen und installiert ihn im finalen PE.

Analyse-Support bedeutet nicht automatisch native Rekonstruktion.

## Supportmatrix

| Native Form | Lift und Analyse | High-Level-Ausgabe | Patch mode |
|-------------|------------------|--------------------|------------|
| x64 unwind v1/v2 | Vollständige geprüfte Records, Operationen, Ketten, Handlerdaten und Provenienz | Frame/Unwind-Zusammenfassung und ggf. strukturierte Sprachbereiche | Vollständige primäre Records; erzeugte `.pdata`/`.xdata` ersetzen die alte Closure |
| x64 unwind v3/APX | Eigenes v3-Payload, Epiloge und Operationszählung | Explizite v3-Annotation | Nur Analyse; berührte Funktion wird abgelehnt |
| ARM32/ARM64 packed | Bereiche, packed Felder, primary/fragment identity | Frame/Unwind-Zusammenfassung | Nur vollständige primäre Records ohne Sprachhandler und einzeln adressierbare Fragmente |
| ARM32/ARM64 unpacked | Geprüfter xdata-Header/-Umfang, Handlerzuordnung und Fragmente | Frame/Unwind-Zusammenfassung | Nur vollständige primäre Records ohne Sprachhandler und einzeln adressierbare Fragmente |
| `__C_specific_handler` | Scopes, Filter, finally-Ziele, Handler und Fortsetzungen | Reduzierbar als `__try`/`__except`/`__finally`, sonst annotiert | Native x64-Rekonstruktion vollständiger darstellbarer Scope-Graphen |
| `__CxxFrameHandler3` | Unwind-/Try-Maps, Catches, Objekt-/Frame-Offsets, Fortsetzungen und IP-to-State | Reduzierbare Intervalle als C++ HighIR mit C-kompatiblen Typannotationen | Native x64-Rekonstruktion des unten beschriebenen engen verifier-clean Subsets |
| `__CxxFrameHandler4` | Begrenztes variables Decoding in den gemeinsamen C++-Graphen | Gleicher HighIR-Graph mit FH4-Provenienz | Nur Analyse; berührte Funktion wird abgelehnt |
| `__GSHandlerCheck_SEH/EH/EH4` | Wrapped Personality und geprüfte GS-Cookie-Provenienz | Basissprachgraph plus Wrapper-Annotation | Nur Analyse; Ablehnung ohne Downgrade |
| x86 registration-chain EH | Getrennt von tabellenbasiertem EH | Unsupported-Form-Annotation | Keine Rekonstruktion |

Malformed Records gelten nie als vollständig. Partial Decoding bleibt zur
Inspektion nutzbar, berechtigt aber nicht zur nativen Generierung. Beweist ein
ARM-xdata-Header trotz beschädigtem Unwind-Body noch einen begrenzten ausführbaren
Fragmentbereich, bleibt dieser für Disassembly erhalten; der Record bleibt
malformed und nicht patchbar.

## Normalisiertes Modell

`ExceptionInfo` gehört `BinaryImage`. Jede `ExceptionFunction` enthält:

- einen geprüften halboffenen Codebereich;
- primary-, chained- oder fragment-Identität;
- native Unwind-Codierung und exakte Runtime/Unwind-Provenienz;
- normalisierte Operationen/Epiloge samt opaken Operanden unbekannter Operationen;
- exakte Personality-Identität und Handlerdaten;
- optionale SEH-Scopes, C++-State-Maps und GS-Cookie-Daten;
- `Complete`-, `Partial`- oder `Malformed`-Status und deterministische Diagnosen.

Der Loader gibt keine rohen Dateizeiger frei. Native RVAs dienen Diagnose und
Ersatz; IR-Verbraucher arbeiten nur mit validierten VAs und Bereichen.

Der bildweite Index erlaubt überlappende chained/fragment Records und liefert
die spezifischste Funktion. Fehlerhafte Verzeichnisse, Bereiche, Zeiger, Zähler,
Zustände, komprimierte Zahlen, Kettenzyklen oder erschöpfte Decode-Budgets senken
den Parse-Status.

Limits gelten pro Tabelle und für den gesamten Funktionsgraphen. Wiederverwendete
Handler-Maps können den Gesamtaufwand nicht vervielfachen. FH3-Records mit
gemeinsamem `FuncInfo` und gleicher Personality bilden eine begrenzte Gruppe:
Catch-Funclets des Parents sind erlaubt, fremde Runtime Functions nicht.

## IR-Vertrag

Exception-Metadaten durchlaufen alle Repräsentationen, ohne den normalen CFG zu ändern:

- LowIR trennt Blöcke an Bereichsgrenzen, State-Transitions, Filtern, Handlern,
  Cleanup-Aktionen und Fortsetzungen.
- Exception-Successors/-Predecessors bleiben von normalen Kanten getrennt.
- MedIR behält normalisierte Deskriptoren und stabile Exception-Kanten.
- HighIR unterscheidet `SEHTry` und `CxxTry` und bewahrt VAs, Typen, Adjektive,
  Objekt-/Frame-Offsets, Aktionen, Zustände und Fortsetzungen.

Der HighIR-Structurer ist intervallkonservativ: Nur vollständig in einem
kompletten Schutzbereich liegende zusammenhängende Statements werden bewegt,
verschachtelte Bereiche von innen nach außen. Kreuzungen, partielle Graphen,
mehrdeutige Grenzen und out-of-line Funclets bleiben im ursprünglichen Kontrollfluss.

Der C-Backend erzeugt MSVC-SEH-Syntax für reduzierbare Ein-Klausel-Bereiche.
Da HighC ein C-Backend ist, werden C++ Catches/Cleanups als deterministische
C-kompatible Kommentare ausgegeben, nicht als angeblich kompilierbares C++.

## LLVM-Metadatenschema

Jede analysierte Exception Function erhält verlustfreie Metadaten, auch ohne
natives WinEH-Lowering:

- Attachment `neverd.windows.eh`;
- nativer Marker `neverd.windows.eh.native`;
- Modultabelle `neverd.windows.eh.functions`;
- Schemaversion `3`.

Der feste Record bewahrt Status, Encoding, Bereich, native Runtime/Unwind-RVAs,
Recordtyp/Kette, packed Wort, Frame, Personality-Namen, Handler, Unwind-Bytes,
Operationen/Epiloge, SEH-Scopes, C++-Maps, GS-Daten, Diagnosen und Regeneration.
Patch-Validierung verlangt exakte Version und exakte Übereinstimmung mit dem Image.

Natives x64-SEH-Lowering verwendet LLVM WinEH und erzeugt nur für vollständig
darstellbare Scope-Graphen verifier-clean `invoke`/Funclet-Fluss. FH3 verlangt:

- x64 COFF, unwind v1/v2, vollständige Metadaten, gültiger synchroner FH3-Graph;
- kein `noexcept`, async, separated-funclet, GS, FH4 oder unbekannte Flags;
- verschachtelte oder disjunkte, nie kreuzende Intervalle;
- keine Destruktor-/Unwind-Aktion, Catch-Objekterzeugung oder Parent-Frame-Abhängigkeit;
- Handler in einem predecessor- und call-freien normalen Block;
- LLVM `invoke` für jede geschützte potenziell unwindende Operation.

Andernfalls bleibt die IR analysierbar und verlustfrei, aber nativer Ersatz wird
abgelehnt. PE Entry Point, TLS-Callbacks und CRT-Wurzeln sind Erhaltungsgrenzen.

## Patch-Transaktion

Eine unterstützte Umschreibung ist eine einzelne PE-Transaktion:

1. Jede betroffene Funktion gegen geladenen Graphen und LLVM-Metadaten validieren.
2. Code unter Erhalt von Section-Identität, Alignment, Traits und semantischen
   Referenzen kompilieren; lokale Windows-Personality externalisieren und xdata
   an den nachgewiesenen ursprünglichen ausführbaren Handler binden.
3. Unberührte Runtime Functions behalten und die gesamte ersetzte Closure samt
   chained Records entfernen.
4. Code/xdata relokieren, pdata mergen/sortieren, Overlaps ablehnen, Personality-
   Abdeckung beweisen und ein einziges Ersatzverzeichnis installieren.
5. CFG-Modus behalten, `.gfids`/`.gehcont` auflösen, Guard CF/EH-Tabellen mergen
   und load-config aktualisieren. Unaufgelöste Helper brechen ab; CFW,
   Return-Flow Guard, Retpolines und XFG bleiben analysis-only.
6. Das fertige Byte-Image vor dem Schreiben erneut parsen.

Die LLVM-Fork-Erweiterung bleibt generisch: Der Final-Image-Writer bewahrt
Section-Traits und Symbolreferenzen. PE/MSVC-Parsing, Policy, Merging,
load-config und Endvalidierung bleiben in NeverD.

Originale Guard-CF/EH-Continuation-Einträge bleiben erhalten, weil ihre
Trampoline gültige indirekte Ziele bleiben. Erzeugte Ziele müssen in emitted
code liegen; Ergebnistabellen sind strikt nach RVA sortiert.

## Final-Image-Validierung

Ein gepatchtes PE wird abgelehnt, wenn nicht alles gilt:

- LLVM akzeptiert COFF und Machine, Klasse, Sections, Directories, Base und Extent stimmen;
- raw/virtuelle Section-Bereiche sind begrenzt und überschneiden sich nicht;
- Exception Directory ist file-backed und im Image;
- Runtime Functions sind sortiert, nicht leer, nicht überlappend und ausführbar;
- x64 Unwind-RVAs, Header, Versionen, Flags, Handler und Ketten sind gültig;
- finale Imports, Exports und COFF-Symbole erlauben erneutes SEH/FH3-Parsing;
- ARM-Records/xdata enthalten unterstützte Versionen und Bereiche;
- Guard-Felder existieren, wenn Flags Tabellen ankündigen;
- Guard-Pointer/Counts/Strides liegen in Datei/Image, alle Ziele sind sortiert und ausführbar.

Fehler brechen die Transaktion ab; ein Best-Effort-Image wird nie geschrieben.

## Fokussierte Verifikation

```bash
cmake --build build --target NeverDLiftTests --parallel 4
build/bin/NeverDLiftTests \
  --gtest_filter='COFFException*:*PatchCOFF_X64.ReconstructsGuardedSEHAndContinuationTable:*PatchCOFF_X64.ReconstructsNativeFH3StateGraph:*PatchCOFF_X64.RejectsInteriorExceptionDirectoryPadding:*PatchCOFF_X64.RebuildsSortedExceptionDirectoryInAppendedSection'
```

Das geschützte x64-Fixture wird mit `/guard:cf` und `/guard:ehcont` gebaut und
prüft SEH-Scopes, Guard-Tabellen, HighC, Patch, Reload, Sortierung und Ziele. Das
FH3-Fixture prüft feste Tabellen, Annotationen, Personality, Try/Catch und
IP-to-State. Bei Parseränderungen sind auch ARM-Formatfälle auszuführen.

## Erweiterung nativen Supports

Neue native Formen müssen im selben Change enthalten:

- vollständigen begrenzten Parser und Modellinvarianten;
- HighIR-/LLVM-Metadata-Roundtrip;
- verifier-clean native IR für jede neue Graphform;
- notwendige Section-/Referenzerhaltung;
- gelinktes PE-Fixture für exakte Architektur/Personality/Version;
- Exception-Directory-, load-config- und Final-Image-Validierung;
- explizite Ablehnungstests benachbarter nicht unterstützter Formen.

Decodierbarkeit allein erweitert niemals die Allow-List. Entscheidend ist die
Erhaltung des Runtime-Ausnahmeverhaltens im final gelinkten Image.
