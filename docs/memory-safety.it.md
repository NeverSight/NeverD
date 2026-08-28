**Lingue**: [English](memory-safety.md) | [简体中文](memory-safety.zh-CN.md) | [繁體中文](memory-safety.zh-TW.md) | [日本語](memory-safety.ja.md) | [한국어](memory-safety.ko.md) | [Français](memory-safety.fr.md) | [Deutsch](memory-safety.de.md) | [Español](memory-safety.es.md) | [Italiano](memory-safety.it.md) | [Русский](memory-safety.ru.md) | [العربية](memory-safety.ar.md)

[← Indice della documentazione](README.it.md)

# Audit e hunt di sicurezza della memoria

NeverD analizza un binario caricato per due famiglie di difetti di sicurezza della memoria e li riporta come JSON strutturato. Entrambe le piste girano sull’IR sollevato, indipendente dal formato, quindi **PE/COFF, ELF e Mach-O sono bersagli di primo livello, paritari** — un reperto non resta mai dietro allo scanner o alla tabella degli import di un solo formato.

| Pista | Comando | Riporta |
|-------|---------|---------|
| **Audit** | `neverd audit <binary>` | Difetti di vita dell’heap e letture locali dello stack non inizializzate |
| **Hunt** | `neverd hunt <binary>` | Overflow di copie pericolose con evidenza simbolica e valori candidati; `replayable=true` solo con un piano `process-input-v1` completo |

Il motore riutilizza l’esecuzione simbolica e il solver bitvector interni di NeverD per testimoni e raggiungibilità. Nessun solver esterno, VM o contenitore.

---

## Invariante centrale: fallire chiuso

Un’operazione non sollevata, una chiamata i cui argomenti il passo ABI non ha recuperato, un bersaglio indiretto non risolto o un budget esaurito producono **UNKNOWN**, mai SAFE. Una destinazione la cui capacità non è recuperabile è UNKNOWN. Il lifting strict resta invariato; lo strato di sicurezza aggiunge solo verdetti conservativi sopra di esso.

Gli effetti delle chiamate usano semantica closed-world: un riepilogo si applica solo quando sono note le sue precondizioni e tutti gli effetti pertinenti. Un effetto sconosciuto o un riepilogo applicabile solo in parte resta UNKNOWN; la lacuna non viene mai trattata come nessun effetto o come una chiamata riuscita.

---

## Contratto di identità per formato

Entrambe le piste richiedono la pipeline di lift (recupera gli argomenti per chiamata). Ogni callee è nominato con la stessa vista di identità del resto di NeverD. L’ordine di scoperta del debug non cambia.

| Formato | Debug (in ordine di precedenza) | Risoluzione import / thunk |
|---------|---------------------------------|----------------------------|
| **PE/COFF** | `--pdb`, directory di debug o `.pdb` adiacente, poi `/MAP` MSVC | Slot IAT e thunk `__imp_`, import ordinali |
| **ELF** | DWARF nell’immagine, `*.debug` separato, poi MAP GNU/LLD | Stub PLT risolti nel nome importato |
| **Mach-O** | DWARF nell’immagine, `.dSYM` adiacente, poi `-map` ld64 | Bind dyld / slot di simboli indiretti e helper di stub |

`--pdb` / `--map` nominano un file companion autorevole: fallire la lettura è un errore, non un fallback silenzioso. `--no-debug` legge solo l’immagine su ogni formato.

Le firme di procedura del PDB servono a distinguere gli allocatori che restituiscono un valore dalle funzioni di rilascio `void`. Il recupero ricco dei tipi locali e di stack da un PDB resta limitato; quando non riesce a stabilire una dimensione esatta dell’oggetto, la caccia ripiega sul modello di frame o di allocazione e riporta UNKNOWN invece di inventare una dimensione.

### Precedenza di `name_source`

Ogni reperto porta un `name_source` che descrive da dove proviene il nome del callee, con questa precedenza:

1. `rename` — un rinomina fornito dal chiamante
2. `import` — una voce IAT (PE), PLT (ELF) o dyld-bind / stub (Mach-O)
3. `export` / `symbol` — un export o una voce simbolo già dichiarata dall’immagine
4. `pdb` / `dwarf` / `map` — un simbolo di debug che stabilisce un segnaposto o coincide con il nome dichiarato
5. `sig` — una corrispondenza di firme
6. `synthetic` — un segnaposto per una routine senza nome

Un `memcpy` linkato staticamente nominato da DWARF riporta `dwarf`; un `memcpy` importato riporta `import` su ogni formato. Una corrispondenza di firme non sposta mai un nome già stabilito dal debugger o dalla tabella degli import.

---

## Catalogo di sink e source

Il catalogo è una tabella configurabile, non un insieme cablato. Ogni **sink** dichiara la classe di debolezza, il ruolo (copy, format, alloc, free, realloc) e gli slot di argomento rilevanti (destinazione, origine, lunghezza, capacità). Un sink JSON di tipo copy o format fornisce anche un effetto di chiamata eseguibile. Ogni **source** nomina un fornitore di input influenzato dall’attaccante.

Le voci integrate risiedono in [`SafetySinks.def`](../include/neverd/safety/SafetySinks.def) e [`SafetySources.def`](../include/neverd/safety/SafetySources.def); coprono la famiglia di copie C comune (`memcpy`/`memmove`/`strcpy`/`strcat`/`strncpy`/`gets`/…), le varianti fortificate `_chk` (limite esplicito di destinazione), la famiglia di allocazione e rilascio (`malloc`/`calloc`/`realloc`/`free`, operator `new`/`delete`) e API heap Win32 opzionali. Le sorgenti di input includono POSIX (`getenv`, `read`, `recv`, `fgets`, `fread`, `scanf`, argomenti del programma) **e** Win32 (`GetCommandLineA/W`, `ReadFile`, `GetEnvironmentVariable*`), così un hunt PE non è limitato agli input POSIX.

Le grafie per formato si piegano su una sola voce: gli underscore iniziali vengono rimossi (`_malloc`, `___strcpy_chk`) e gli operator `new`/`delete` mangled coincidono tramite alias.

Quando un sink JSON di tipo copy o format omette `effect`, l’applicabilità viene dedotta dallo slot di argomento referenziato più alto. Una copia richiede quindi quell’arità esatta; un sink di formato accetta chiamate da tale arità minima fino al massimo variadico. Un oggetto `effect` opzionale può impostare esplicitamente con `min_arity` e `max_arity` (o `"variadic"`) un intervallo di arità accettato, inclusi argomenti wrapper aggiuntivi oltre l’arità copy esatta dedotta; `min_arity` deve essere almeno lo slot di ruolo referenziato più alto più uno, mentre `formats` e `abis` limitano l’applicabilità. Se l’arità, il formato oggetto o l’ABI della chiamata non corrispondono, non si applica alcun riepilogo e il risultato closed-world resta UNKNOWN.

Estendi o sostituisci il catalogo con un file di specifica:

```bash
neverd hunt --sinks extra_sinks.json --sources extra_sources.json app
```

```json
{ "sinks": [
    { "name": "my_copy", "kind": "copy", "dst": 0, "src": 1, "len": 2 },
    { "name": "my_format", "kind": "format", "dst": 0, "fmt": 2,
      "effect": { "min_arity": 3, "max_arity": "variadic",
                  "formats": ["elf"], "abis": ["sysv"] } }
  ],
  "sources": [
    { "name": "my_read", "out": 1, "return_tainted": true }
  ]
}
```

Per una source personalizzata, `out` e `return_tainted` sono solo metadati di discovery. Non stabiliscono effetti eseguibili sulla memoria, sul valore restituito o sul taint. Lo schema source attuale non dispone dei contratti tipizzati di successo, mutazione, formato e ABI necessari per tali semantiche; un’analisi che dipende dall’effetto di una source personalizzata resta quindi UNKNOWN. Le source integrate non sono interessate: i loro descrittori tipizzati e verificati per applicabilità continuano a fornire effetti eseguibili.

Un sink personalizzato non limitato con il solo argomento di destinazione non viene dedotto da una voce source omonima. Un sink personalizzato simile a `gets` deve attivare esplicitamente `"unbounded": true`; aggiungere lo stesso nome al catalogo source non gli conferisce un effetto eseguibile e i campi origine/lunghezza contraddittori vengono rifiutati in modo transazionale.

---

## Hunt: verdetti di overflow di copia

Per ogni sink di copia il hunt recupera la capacità di destinazione — dimensione di array dichiarata dal debug, poi un sito di allocazione heap di dimensione nota, poi un bound sano del frame di stack — e classifica l’argomento che decide la lunghezza di scrittura con una camminata SSA all’indietro (seguendo spill/reload tramite slot di stack):

- Una **lunghezza costante** entro una capacità esatta è SAFE. Un overflow costante è UNSAFE solo se il sink è raggiungibile su un percorso corroborato; altrimenti resta UNKNOWN.
- Le copie **fortificate** `_chk` portano un bound di destinazione a runtime. Un rifiuto o un bound dimostrato compatibile è SAFE; una scrittura possibile oltre l’oggetto è UNSAFE; un bound non recuperato o inconcludente è UNKNOWN.
- Lunghezza **dimostrabilmente limitata** (chiamata che restituisce una lunghezza, maschera, clamp) ritirata prima del solver, con il motivo. È SAFE solo con una dimensione di destinazione esatta; un solo upper bound di regione resta UNKNOWN.
- Lunghezza **influenzata dall’attaccante** con capacità nota: solver bitvector. Se una lunghezza maggiore della capacità è soddisfacibile, il verdetto è UNSAFE. I candidati sono riproducibili solo con un piano `process-input-v1` completo: inizialmente valori letterali esatti dell’ambiente e, al massimo, i byte restituiti dal primo consumo dello standard input. argv, file, rete, fonti personalizzate o ambigue restano non riproducibili con una motivazione.
- Qualsiasi altra cosa — lunghezza o capacità sconosciuta — è UNKNOWN.

Ogni capacità recuperata è un **upper bound** sulla dimensione reale, quindi un overflow dimostrato non è mai un falso positivo.

### Input formattato

Per `scanf`/`fscanf` e le relative grafie versionate, un formato costante leggibile associa ogni conversione non soppressa al suo effettivo argomento di output variadico. Gli output `%s`/`%[` non limitati propagano il taint agli usi successivi della stringa; gli output numerici e di carattere propagano il taint ai valori caricati dall’oggetto scritto, ma non al valore del puntatore di output stesso. `sscanf` propaga tali effetti solo quando la stringa di input è già influenzata dall’attaccante. Gli output di testo limitati come `%Ns`/`%N[` propagano il taint insieme a un’estensione `MaxBytes` che include il terminatore; le varianti wide-character calcolano tale estensione in byte usando la larghezza di `wchar_t` della piattaforma. Le conversioni soppresse, gli argomenti in eccesso, i formati dipendenti dalla posizione o non supportati e `%n` restano UNKNOWN invece di essere ipotizzati.

---

## Audit: verdetti di vita dell’heap

Per ogni allocazione l’audit segue l’handle nel grafo di controllo, anche tramite spill/reload di stack, e applica un riassunto di escape (restituito, memorizzato tramite un indirizzo non-stack, o passato a un callee opaco):

- **Leak** — l’handle non è né rilasciato né lasciato sfuggire.
- **Doppia free** — un secondo rilascio è raggiungibile dopo il primo su un cammino.
- **Use-after-free** — un dereference o un uso opaco è raggiungibile dopo un rilascio.

I **wrapper** di allocazione e rilascio sono riconosciuti tramite riassunti di escape per funzione, così un forwarder `malloc`/`free` non nasconde il difetto. Rilasci su rami mutuamente esclusivi non sono riportati come doppia free.

La macchina a stati dell’heap emette dapprima una sequenza di eventi candidata (allocazione, rilascio, uso o uscita per ritorno). Una seconda passata deve rieseguire quella sequenza su un percorso LowIR simbolico e dimostrare che il suo predicato di percorso è soddisfacibile prima che il risultato diventi un UNSAFE ad ALTA confidenza. LowIR mancante, operazioni opache, chiamate senza riassunto, incertezza del solutore e limiti di esplorazione declassano il candidato a UNKNOWN. L’havoc di memoria may-alias conservativo è tracciato a parte, così le normali scritture nel frame di stack non invalidano una prova di raggiungibilità per il resto esatta.

---

## Budget, output e binding

L’esplorazione del hunt e il solver sono limitati (`--max-paths`, `--max-steps`, `--max-loop`, `--solver-conflicts`); l’esaurimento del budget produce UNKNOWN. Entrambi i comandi stampano JSON e onorano `-o`. Il codice di uscita è `0` per SAFE, `2` per UNSAFE e `1` per UNKNOWN o un errore.

Le stesse analisi sono disponibili tramite l’API C (`neverd_session_audit_json` / `neverd_session_hunt_json` con `neverd_safety_options` versionato) e l’SDK Python (`Session.audit()` / `Session.hunt()`).

### Schema di un reperto

```json
{
  "class": "buffer_overflow",
  "function": "parse_header",
  "name": "strcpy",
  "name_source": "import",
  "call_va": "0x11a4",
  "source": "reader.c:42",
  "sink": "strcpy",
  "arg_index": 1,
  "flow": "TAINTED",
  "verdict": "UNSAFE",
  "confidence": "HIGH",
  "capacity": 16,
  "capacity_kind": "exact",
  "corroboration": "path predicate and overflow are jointly satisfiable",
  "evidence": { "concrete_input": { "copy_length": "17", "argv[1]": "16 bytes" }, "candidate_values": [{ "name": "copy_length", "value": "17" }, { "name": "argv[1]", "value": "16 bytes" }], "replayable": false, "replay": { "adapter": "process-input-v1", "reason": "argv input is not supported by process-input-v1" }, "symbolic_model": [{ "id": 0, "name": "copy_len", "width": 64, "value_hex": "0x11", "origin": "input" }] }
}
```

`replayable` è evidenza derivata, non una promessa indipendente: è vero solo se `replay` contiene un piano di input completo per l’adapter `process-input-v1`. Il piano registra i byte esatti dell’ambiente, la prima sequenza dello standard input se usata e i binding dagli ID delle assegnazioni del solver; altrimenti `replay.reason` ne spiega il motivo. I campi sono additivi; lo `schema_version` principale resta `1`.

---

## Limiti dei falsi positivi e ambito

- La capacità è esatta o un upper bound della dimensione reale, quindi UNSAFE riflette un overflow reale. Senza una dimensione esatta, un bound di regione insufficiente a provare sicurezza produce UNKNOWN.
- Una copia limitata è ritirata prima del solver e conta in `skipped`; una capacità esatta può provare SAFE, mentre un solo upper bound resta UNKNOWN.
- Le copie catalogate wide-character e append restano UNKNOWN finché non sono recuperati la larghezza dell’elemento e l’estensione esistente della destinazione. Gli allocator con parametro di uscita e la proprietà condizionale di `realloc` restano UNKNOWN quando la transizione dell’handle non può essere provata.
- **P0** (questa versione, tutti e tre i formati): catalogo di sink, prefiltro degli argomenti, hunt di overflow di copia, audit di vita dell’heap. Ogni host esegue sei fixture PE, ELF e Mach-O per x86-64 e AArch64.
- **P1**: overflow di stack/globale, letture locali non inizializzate e controlli delle stringhe di formato sono disponibili; tipi di stack PDB più ricchi e ulteriori allocator di piattaforma restano copertura incrementale, mentre l’assenza di un sommario esatto resta UNKNOWN.
- **P2**: controlli runtime inseriti da patch, raggiungibilità interprocedurale dell’attaccante.
