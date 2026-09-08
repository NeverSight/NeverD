**Lingue**: [English](../../ATTRIBUTION.md) | [简体中文](../zh-CN/ATTRIBUTION.md) | [繁體中文](../zh-TW/ATTRIBUTION.md) | [日本語](../ja/ATTRIBUTION.md) | [한국어](../ko/ATTRIBUTION.md) | [Français](../fr/ATTRIBUTION.md) | [Deutsch](../de/ATTRIBUTION.md) | [Español](../es/ATTRIBUTION.md) | [Italiano](ATTRIBUTION.md) | [Русский](../ru/ATTRIBUTION.md) | [العربية](../ar/ATTRIBUTION.md)

# Attribuzione e citazione

Questa pagina traduce la [guida in inglese](../../ATTRIBUTION.md).
Fanno fede i termini di [LICENSE](../../LICENSE).

NeverD è sviluppato da **NeverD contributors**. Il suo repository del codice
sorgente è [NeverSight/NeverD](https://github.com/NeverSight/NeverD).

## Obblighi di licenza quando si riutilizza il codice

Il materiale originale di NeverD è concesso in licenza secondo la
[GNU AGPL, solo versione 3](../../LICENSE). Quando si distribuiscono copie
o adattamenti soggetti alla licenza, occorre conservare gli avvisi applicabili
di copyright, licenza e garanzia, compreso l’avviso del progetto in
[NOTICE](../../NOTICE). Occorre conservare anche gli eventuali avvisi dei
singoli autori. Il codice sorgente modificato soggetto alla licenza deve
riportare avvisi ben visibili che identifichino le modifiche e la relativa data.

Questi obblighi si applicano al materiale soggetto alla licenza riutilizzato
manualmente, copiato o adattato con un assistente di IA o un modello linguistico
di grandi dimensioni (LLM), oppure trasformato mediante LLVM IR, compilazione,
decompilazione o conversione in un altro linguaggio di programmazione.
Cambiare nomi, formattazione, linguaggio o strumenti non elimina di per sé
gli obblighi. Indicare NeverD come fonte del materiale di NeverD riutilizzato;
citare soltanto un modello di IA o LLVM non identifica tale fonte.

Accompagnare le distribuzioni del codice sorgente e il codice sorgente
corrispondente ai binari distribuiti con la licenza completa e gli avvisi
applicabili. Per i binari e i servizi di rete, rispettare anche le disposizioni
applicabili delle sezioni 6 e 13 della AGPL. Una citazione, un collegamento
o un ringraziamento, da soli, **non** sostituiscono i requisiti della AGPL
relativi alla licenza, agli avvisi di modifica o alla disponibilità del codice
sorgente.

Questa guida spiega la licenza esistente; non aggiunge restrizioni o termini
aggiuntivi ai sensi della sezione 7. I termini che fanno fede sono in
[LICENSE](../../LICENSE), in particolare nelle sezioni 0, 2, 4–6 e 13,
disponibili anche presso la
[Free Software Foundation](https://www.gnu.org/licenses/agpl-3.0.html).

## Rendere tracciabile la fonte

Per ogni porzione riutilizzata, consigliamo di annotare il file o il simbolo
originale, il commit o la versione esatta e una breve descrizione delle
modifiche, accanto al codice oppure negli avvisi del progetto. Usare un
collegamento permanente GitHub con l’hash completo del commit, affinché la
citazione continui a identificare la stessa fonte. Questi dettagli aggiuntivi
sulla provenienza sono una raccomandazione per la citazione, non una condizione
di licenza aggiuntiva.

Ad esempio, sostituire i campi tra parentesi quadre con i dettagli effettivi
della fonte:

```text
This project includes material from NeverD.
Copyright (C) 2026 NeverD contributors (https://github.com/NeverSight/NeverD)
License: GNU Affero General Public License, version 3 only (AGPL-3.0-only).
Original source: https://github.com/NeverSight/NeverD/blob/[full-commit]/[path]
Changes: [description], [YYYY-MM-DD].
The applicable license and notices are included with this distribution.
```

Nei flussi di lavoro assistiti dall’IA, conservare queste informazioni sulla
provenienza insieme al contesto sorgente selezionato e riportarle in ogni
codice soggetto alla licenza che si pubblica. Controllare il codice risultante
e i relativi avvisi prima di condividerlo. Per i dataset contenenti codice
sorgente NeverD soggetto alla licenza, conservare gli avvisi e le informazioni
di licenza applicabili quando si distribuisce quel codice sorgente.

## Ricerca, riferimenti e risultati prodotti

Si prega di citare NeverD negli articoli, nella documentazione, nei benchmark
e nei progetti che lo utilizzano o si basano sulla sua implementazione.
[CITATION.cff](../../CITATION.cff) fornisce metadati di citazione del software
leggibili dalle macchine. La seguente citazione in testo semplice può essere
usata indicando la versione o il commit effettivamente utilizzato:

```text
NeverD contributors. NeverD: Binary analysis and decompilation engine.
https://github.com/NeverSight/NeverD. Version or commit: [revision used].
```

Il semplice studio di un’idea o di un algoritmo non rende automaticamente
un’implementazione indipendente soggetta alla licenza di NeverD. Analogamente,
eseguire NeverD sul programma di un’altra persona non pone automaticamente
i risultati prodotti sotto la AGPL: secondo la sezione 2, un risultato è
soggetto alla licenza soltanto se il suo contenuto costituisce un’opera
coperta dalla licenza. La stessa distinzione vale per i risultati dell’IA;
l’addestramento su NeverD o la sua lettura non rendono automaticamente ogni
risultato del modello un’opera coperta. Per questi usi senza materiale
soggetto alla licenza, la citazione viene richiesta come prassi scientifica
e ingegneristica, anziché imposta come nuova condizione di licenza.

## Materiale di terzi e copie precedenti

Componenti come LLVM, Capstone e Unicorn mantengono le proprie licenze.
Consultare gli avvisi nel loro codice sorgente,
[THIRD_PARTY_NOTICES.md](../../THIRD_PARTY_NOTICES.md) e le eventuali licenze
specifiche delle directory, compresa la
[licenza del corpus di test](https://github.com/NeverSight/testbins/blob/9d9362d2cdfe0b4b0347bd0e12b3b8fac67d3a5b/LICENSE).
Conservare le attribuzioni originali ai terzi e rispettare le loro licenze
quando si riutilizza quel materiale. Questa guida non cambia la licenza del
materiale di terzi e non revoca le autorizzazioni già concesse per le copie
precedenti.
