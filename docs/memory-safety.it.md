**Lingue**: [English](memory-safety.md) | [简体中文](memory-safety.zh-CN.md) | [繁體中文](memory-safety.zh-TW.md) | [日本語](memory-safety.ja.md) | [한국어](memory-safety.ko.md) | [Français](memory-safety.fr.md) | [Deutsch](memory-safety.de.md) | [Español](memory-safety.es.md) | [Italiano](memory-safety.it.md) | [Русский](memory-safety.ru.md) | [العربية](memory-safety.ar.md)

[← Indice della documentazione](README.it.md)

# Audit e hunt di sicurezza della memoria

NeverD analizza un binario caricato per due famiglie di difetti di sicurezza della memoria e li riporta come JSON strutturato. Entrambe le piste girano sull’IR sollevato, indipendente dal formato, quindi **PE/COFF, ELF e Mach-O sono bersagli di primo livello, paritari** — un reperto non resta mai dietro allo scanner o alla tabella degli import di un solo formato.

| Pista | Comando | Riporta |
|-------|---------|---------|
| **Audit** | `neverd audit <binary>` | Difetti di vita dell’heap: leak, doppia free, use-after-free |
| **Hunt** | `neverd hunt <binary>` | Overflow di copie pericolose con un testimone concreto riproducibile |

Il motore riutilizza l’esecuzione simbolica e il solver bitvector interni di NeverD per testimoni e raggiungibilità. Nessun solver esterno, VM o contenitore.

---

## Invariante centrale: fallire chiuso

Un’operazione non sollevata, una chiamata i cui argomenti il passo ABI non ha recuperato, un bersaglio indiretto non risolto o un budget esaurito producono **UNKNOWN**, mai SAFE. Una destinazione la cui capacità non è recuperabile è UNKNOWN. Il lifting strict resta invariato; lo strato di sicurezza aggiunge solo verdetti conservativi sopra di esso.

---

## Contratto di identità per formato

Entrambe le piste richiedono la pipeline di lift (recupera gli argomenti per chiamata). Ogni callee è nominato con la stessa vista di identità del resto di NeverD. L’ordine di scoperta del debug non cambia.

| Formato | Debug (in ordine di precedenza) | Risoluzione import / thunk |
|---------|---------------------------------|----------------------------|
| **PE/COFF** | `--pdb`, directory di debug o `.pdb` adiacente, poi `/MAP` MSVC | Slot IAT e thunk `__imp_`, import ordinali |
| **ELF** | DWARF nell’immagine, `*.debug` separato, poi MAP GNU/LLD | Stub PLT risolti nel nome importato |
| **Mach-O** | DWARF nell’immagine, `.dSYM` adiacente, poi `-map` ld64 | Bind dyld / slot di simboli indiretti e helper di stub |

`--pdb` / `--map` nominano un file companion autorevole: fallire la lettura è un errore, non un fallback silenzioso. `--no-debug` legge solo l’immagine su ogni formato.

### Precedenza di `name_source`

Ogni reperto porta un `name_source` che descrive da dove proviene il nome del callee, con questa precedenza:

1. `rename` — un rinomina fornito dal chiamante
2. `import` — una voce IAT (PE), PLT (ELF) o dyld-bind / stub (Mach-O)
3. `pdb` / `dwarf` / `map` — un simbolo di debug, secondo il loader
4. `export` / `symbol` — un export o una voce della tabella dei simboli
5. `sig` — una corrispondenza di firme
6. `synthetic` — un segnaposto per una routine senza nome

Un `memcpy` linkato staticamente nominato da DWARF riporta `dwarf`; un `memcpy` importato riporta `import` su ogni formato. Una corrispondenza di firme non sposta mai un nome già stabilito dal debugger o dalla tabella degli import.

---

## Catalogo di sink e source

Il catalogo è una tabella configurabile, non un insieme cablato. Ogni **sink** dichiara la classe di debolezza, il ruolo (copy, format, alloc, free, realloc) e gli slot di argomento rilevanti (destinazione, origine, lunghezza, capacità). Ogni **source** nomina un fornitore di input influenzato dall’attaccante.

Il catalogo integrato copre la famiglia di copie C comune (`memcpy`/`memmove`/`strcpy`/`strcat`/`strncpy`/`gets`/…), le varianti fortificate `_chk` (limite esplicito di destinazione), la famiglia di allocazione e rilascio (`malloc`/`calloc`/`realloc`/`free`, operator `new`/`delete`) e API heap Win32 opzionali. Le sorgenti di input includono POSIX (`getenv`, `read`, `recv`, `fgets`, `fread`, `scanf`, argomenti del programma) **e** Win32 (`GetCommandLineA/W`, `ReadFile`, `GetEnvironmentVariable*`), così un hunt PE non è limitato agli input POSIX.

Le grafie per formato si piegano su una sola voce: gli underscore iniziali vengono rimossi (`_malloc`, `___strcpy_chk`) e gli operator `new`/`delete` mangled coincidono tramite alias.

Estendi o sostituisci il catalogo con un file di specifica:

```bash
neverd hunt --sinks extra_sinks.json --sources extra_sources.json app
```

```json
{ "sinks": [
    { "name": "my_copy", "kind": "copy", "dst": 0, "src": 1, "len": 2 }
] }
```

---

## Hunt: verdetti di overflow di copia

Per ogni sink di copia il hunt recupera la capacità di destinazione — dimensione di array dichiarata dal debug, poi un sito di allocazione heap di dimensione nota, poi un bound sano del frame di stack — e classifica l’argomento che decide la lunghezza di scrittura con una camminata SSA all’indietro (seguendo spill/reload tramite slot di stack):

- **Lunghezza costante** confrontata direttamente con la capacità → SAFE o UNSAFE.
- Copie **fortificate** `_chk` con bound di destinazione a runtime → SAFE.
- Lunghezza **dimostrabilmente limitata** (chiamata che restituisce una lunghezza, maschera, clamp) ritirata come SAFE skip, con il motivo.
- Lunghezza **influenzata dall’attaccante** con capacità nota: solver bitvector. Se una lunghezza maggiore della capacità è soddisfacibile, il verdetto è UNSAFE e il modello del solver è il testimone concreto.
- Qualsiasi altra cosa — lunghezza o capacità sconosciuta — è UNKNOWN.

Ogni capacità recuperata è un **upper bound** sulla dimensione reale, quindi un overflow dimostrato non è mai un falso positivo.

---

## Audit: verdetti di vita dell’heap

Per ogni allocazione l’audit segue l’handle nel grafo di controllo, anche tramite spill/reload di stack, e applica un riassunto di escape (restituito, memorizzato tramite un indirizzo non-stack, o passato a un callee opaco):

- **Leak** — l’handle non è né rilasciato né lasciato sfuggire.
- **Doppia free** — un secondo rilascio è raggiungibile dopo il primo su un cammino.
- **Use-after-free** — un dereference o un uso opaco è raggiungibile dopo un rilascio.

I **wrapper** di allocazione e rilascio sono riconosciuti tramite riassunti di escape per funzione, così un forwarder `malloc`/`free` non nasconde il difetto. Rilasci su rami mutuamente esclusivi non sono riportati come doppia free.

---

## Budget, output e binding

L’esplorazione del hunt e il solver sono limitati (`--max-paths`, `--max-steps`, `--max-loop`, `--solver-conflicts`); l’esaurimento del budget produce UNKNOWN. Entrambi i comandi stampano JSON e onorano `-o`. Il codice di uscita è `0` per un’esecuzione pulita, `2` se c’è un reperto UNSAFE, `1` in errore.

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
  "evidence": { "concrete_input": { "copy_length": "17", "argv[1]": "17 bytes" } }
}
```

---

## Limiti dei falsi positivi e ambito

- La capacità è sempre un upper bound, quindi UNSAFE riflette un overflow reale. Un buffer troppo piccolo la cui dimensione dichiarata non è disponibile può essere riportato SAFE anziché UNSAFE (omissione conservativa, mai falso allarme).
- Una copia limitata in lunghezza è ritirata come SAFE skip; questo privilegia la precisione sui casi controllati dall’attaccante che il hunt è progettato per dimostrare.
- **P0** (questa versione, tutti e tre i formati): catalogo di sink, prefiltro degli argomenti, hunt di overflow di copia, audit di vita dell’heap.
- **P1**: overflow di stack/globale, letture non inizializzate, stringhe di formato, tipi di stack PDB più ricchi, ulteriori allocator di piattaforma.
- **P2**: controlli runtime inseriti da patch, raggiungibilità interprocedurale dell’attaccante.
