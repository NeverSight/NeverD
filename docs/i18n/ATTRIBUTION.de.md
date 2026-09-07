**Sprachen**: [English](../../ATTRIBUTION.md) | [简体中文](ATTRIBUTION.zh-CN.md) | [繁體中文](ATTRIBUTION.zh-TW.md) | [日本語](ATTRIBUTION.ja.md) | [한국어](ATTRIBUTION.ko.md) | [Français](ATTRIBUTION.fr.md) | [Deutsch](ATTRIBUTION.de.md) | [Español](ATTRIBUTION.es.md) | [Italiano](ATTRIBUTION.it.md) | [Русский](ATTRIBUTION.ru.md) | [العربية](ATTRIBUTION.ar.md)

# Urheberangaben und Quellenangaben

Diese Seite ist eine Übersetzung des [englischen Leitfadens](../../ATTRIBUTION.md).
Maßgeblich sind die Bedingungen in [LICENSE](../../LICENSE).

NeverD wird von **NeverD contributors** entwickelt. Das Quellcode-Repository ist
[NeverSight/NeverD](https://github.com/NeverSight/NeverD).

## Lizenzpflichten bei der Wiederverwendung von Code

Das von NeverD stammende Originalmaterial steht unter der
[GNU AGPL ausschließlich in Version 3](../../LICENSE). Wenn Sie davon erfasste
Kopien oder Bearbeitungen weitergeben, bewahren Sie die geltenden Urheberrechts-,
Lizenz- und Gewährleistungshinweise, einschließlich des Projekthinweises in
[NOTICE](../../NOTICE). Bewahren Sie auch alle Hinweise auf einzelne Autoren.
Geänderter, von der Lizenz erfasster Quellcode muss deutlich sichtbare Hinweise
auf die Änderungen und das zugehörige Datum enthalten.

Diese Pflichten gelten für erfasstes Material, das manuell wiederverwendet,
mit einem KI-Assistenten oder großen Sprachmodell (LLM) kopiert oder bearbeitet
oder durch LLVM IR, Kompilierung, Dekompilierung oder Übertragung in eine andere
Programmiersprache umgewandelt wird. Änderungen an Namen, Formatierung, Sprache
oder Werkzeugen heben die Pflichten für sich genommen nicht auf. Nennen Sie
NeverD als Quelle des wiederverwendeten NeverD-Materials; allein ein KI-Modell
oder LLVM zu nennen, weist diese Quelle nicht aus.

Legen Sie die vollständige Lizenz und die geltenden Hinweise den
Quellcode-Distributionen und dem korrespondierenden Quellcode für weitergegebene
Binärdateien bei. Beachten Sie bei Binärdateien und Netzwerkdiensten außerdem
die jeweils anwendbaren Bestimmungen der AGPL-Abschnitte 6 und 13. Eine
Quellenangabe, ein Link oder eine Danksagung allein ersetzt **nicht** die
AGPL-Anforderungen an Lizenzierung, Änderungshinweise oder die Bereitstellung
des Quellcodes.

Dieser Leitfaden erläutert die bestehende Lizenz; er fügt keine Einschränkungen
oder zusätzlichen Bedingungen nach Abschnitt 7 hinzu. Die maßgeblichen
Bedingungen stehen in [LICENSE](../../LICENSE), insbesondere in den Abschnitten
0, 2, 4–6 und 13, und sind auch bei der
[Free Software Foundation](https://www.gnu.org/licenses/agpl-3.0.html) verfügbar.

## Die Quelle nachvollziehbar machen

Für jeden wiederverwendeten Teil empfehlen wir, die ursprüngliche Datei oder
das Symbol, den genauen Commit oder die Veröffentlichung sowie eine kurze
Beschreibung Ihrer Änderungen neben dem Code oder in den Hinweisen Ihres
Projekts festzuhalten. Verwenden Sie einen GitHub-Permalink mit dem vollständigen
Commit-Hash, damit die Quellenangabe weiterhin dieselbe Quelle identifiziert.
Diese zusätzlichen Herkunftsangaben sind eine Empfehlung für Quellenangaben,
keine zusätzliche Lizenzbedingung.

Ersetzen Sie beispielsweise die Felder in eckigen Klammern durch die tatsächlichen
Angaben zur Quelle:

```text
This project includes material from NeverD.
Copyright (C) 2026 NeverD contributors (https://github.com/NeverSight/NeverD)
License: GNU Affero General Public License, version 3 only (AGPL-3.0-only).
Original source: https://github.com/NeverSight/NeverD/blob/[full-commit]/[path]
Changes: [description], [YYYY-MM-DD].
The applicable license and notices are included with this distribution.
```

Bewahren Sie bei KI-gestützten Arbeitsabläufen diese Herkunftsangaben zusammen
mit dem ausgewählten Quellcode-Kontext auf und übernehmen Sie sie in jeden
von der Lizenz erfassten Code, den Sie veröffentlichen. Prüfen Sie den
entstandenen Code und seine Hinweise vor der Weitergabe. Bewahren Sie bei
Datensätzen, die erfassten NeverD-Quellcode enthalten, die geltenden Hinweise
und Lizenzinformationen, wenn Sie diesen Quellcode weitergeben.

## Forschung, Referenzen und Ausgaben

Bitte zitieren Sie NeverD in wissenschaftlichen Arbeiten, Dokumentationen,
Benchmarks und Projekten, die es verwenden oder auf seiner Implementierung
aufbauen. [CITATION.cff](../../CITATION.cff) enthält maschinenlesbare Metadaten
zum Zitieren der Software. Die folgende Quellenangabe im Klartext können Sie
mit der tatsächlich verwendeten Version oder dem Commit ergänzen:

```text
NeverD contributors. NeverD: Binary analysis and decompilation engine.
https://github.com/NeverSight/NeverD. Version or commit: [revision used].
```

Das bloße Studium einer Idee oder eines Algorithmus unterstellt eine unabhängige
Implementierung nicht automatisch der Lizenz von NeverD. Ebenso stellt die
Ausführung von NeverD auf dem Programm eines anderen dessen Ausgabe nicht
automatisch unter die AGPL: Nach Abschnitt 2 ist eine Ausgabe nur dann erfasst,
wenn ihr Inhalt ein von der Lizenz erfasstes Werk darstellt. Dieselbe
Unterscheidung gilt für KI-Ausgaben; das Trainieren mit oder Lesen von NeverD
macht nicht automatisch jede Modellausgabe zu einem erfassten Werk. Für solche
Nutzungen ohne erfasstes Material wird eine Quellenangabe als wissenschaftliche
und technische Praxis erbeten und nicht als neue Lizenzbedingung vorgeschrieben.

## Material Dritter und frühere Kopien

Komponenten wie LLVM, Capstone und Unicorn behalten ihre eigenen Lizenzen.
Beachten Sie die Hinweise in ihrem Quellcode,
[THIRD_PARTY_NOTICES.md](../../THIRD_PARTY_NOTICES.md) und etwaige
verzeichnisspezifische Lizenzen, einschließlich der
[Lizenz des Testkorpus](https://github.com/NeverSight/testbins/blob/9d9362d2cdfe0b4b0347bd0e12b3b8fac67d3a5b/LICENSE).
Bewahren Sie die ursprünglichen Urheberangaben Dritter und halten Sie deren
Lizenzen ein, wenn Sie dieses Material wiederverwenden. Dieser Leitfaden
lizenziert Material Dritter nicht neu und widerruft keine zuvor für frühere
Kopien erteilten Berechtigungen.
