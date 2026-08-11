**Lingue**: [English](windows-exception-reconstruction.md) | [简体中文](windows-exception-reconstruction.zh-CN.md) | [繁體中文](windows-exception-reconstruction.zh-TW.md) | [日本語](windows-exception-reconstruction.ja.md) | [한국어](windows-exception-reconstruction.ko.md) | [Français](windows-exception-reconstruction.fr.md) | [Deutsch](windows-exception-reconstruction.de.md) | [Español](windows-exception-reconstruction.es.md) | [Italiano](windows-exception-reconstruction.it.md) | [Русский](windows-exception-reconstruction.ru.md) | [العربية](windows-exception-reconstruction.ar.md)

# Ricostruzione delle eccezioni Windows

[← Indice della documentazione](README.it.md)

NeverD conserva le informazioni sulle eccezioni Windows basate su tabelle
durante caricamento, lift, decompilazione e riscrittura binaria. I metadata fanno
parte del contratto eseguibile di una funzione: la riscrittura viene rifiutata
se non si può dimostrare la coerenza tra codice generato, record
runtime-function, tabelle di linguaggio e tabelle di protezione.

Sono distinti tre livelli di supporto:

- **Analisi**: decodifica la forma nativa in record normalizzati e verificati,
  esposti alla pipeline IR.
- **Decompilazione**: le regioni protette riducibili diventano nodi di eccezione
  HighIR; le altre mantengono annotazioni native deterministiche.
- **Ricostruzione nativa**: patch mode può far generare a LLVM un contratto di
  eccezione sostitutivo completo e installarlo nel PE finale.

Il supporto di analisi non implica la ricostruzione nativa.

## Matrice di supporto

| Forma nativa | Lift e analisi | Output high-level | Patch mode |
|--------------|----------------|-------------------|------------|
| unwind x64 v1/v2 | Record, operazioni, catene, dati handler e provenienza completi e verificati | Sommario frame/unwind e regioni strutturate quando applicabile | Record primary completi; `.pdata`/`.xdata` generate sostituiscono la closure obsoleta |
| unwind x64 v3/APX | Payload v3, epiloghi e conteggio operazioni dedicati | Annotazioni v3 esplicite | Solo analisi; funzione toccata rifiutata |
| unwind ARM32/ARM64 packed | Intervalli, campi packed e identità primary/fragment | Sommario frame/unwind | Solo record primary completi senza linguaggio o fragment indirizzabili separatamente |
| unwind ARM32/ARM64 unpacked | Header/estensione xdata verificati, handler e fragment | Sommario frame/unwind | Solo record primary completi senza linguaggio o fragment indirizzabili separatamente |
| `__C_specific_handler` | Scope, filtri, finally, handler e continuation | `__try`/`__except`/`__finally` se riducibile, annotazione altrimenti | Ricostruzione x64 nativa di grafi completi e rappresentabili |
| `__CxxFrameHandler3` | Unwind/try map, catch, offset object/frame, continuation e IP-to-state | Intervalli riducibili come C++ HighIR con annotazioni di tipo compatibili C | Ricostruzione x64 del subset stretto e verifier-clean descritto sotto |
| `__CxxFrameHandler4` | Decodifica variabile limitata nel grafo C++ comune | Stesso HighIR con provenienza FH4 | Solo analisi; funzione toccata rifiutata |
| `__GSHandlerCheck_SEH/EH/EH4` | Personality avvolta e provenienza GS cookie verificata | Grafo base e annotazione wrapper | Solo analisi; rifiuto senza downgrade |
| EH x86 registration-chain | Distinto dall’EH tabellare | Annotazione di forma non supportata | Non ricostruito |

Un record malformed non è mai completo. Una decodifica partial resta utile per
l’ispezione ma non autorizza la generazione nativa. Se un header xdata ARM prova
ancora un intervallo di fragment eseguibile limitato nonostante un corpo unwind
danneggiato, l’intervallo resta per il disassembly; il record è malformed e non
patchable.

## Modello normalizzato

`ExceptionInfo` appartiene a `BinaryImage`. Ogni `ExceptionFunction` contiene:

- intervallo di codice semiaperto verificato;
- identità primary, chained o fragment;
- encoding unwind nativo e provenienza runtime/unwind esatta;
- operazioni/epiloghi normalizzati e operand opachi non interpretati;
- identità personality esatta e dati handler;
- scope SEH, mappe di stato C++ e dati GS cookie opzionali;
- stato `Complete`, `Partial` o `Malformed` e diagnostica deterministica.

Il loader non espone puntatori raw. Le RVA native servono a diagnosi e
sostituzione; i consumer IR usano solo VA e intervalli validati.

L’indice globale consente overlap chained/fragment e restituisce la funzione più
specifica. Directory, intervalli, puntatori, conteggi, transizioni, interi
compressi o catene corrotti e budget esauriti abbassano lo stato di parse.

I limiti valgono per tabella e per l’intero grafo della funzione. Riutilizzare
una handler map in molte try entry non supera il budget aggregato. I record FH3
con `FuncInfo` e personality comuni formano un gruppo limitato: accettano i propri
catch funclet, non indirizzi runtime estranei.

## Contratto IR

I metadata attraversano tutte le rappresentazioni senza alterare il CFG ordinario:

- LowIR divide i blocchi a limiti, stati, filtri, handler, cleanup e continuation.
- Successor/predecessor eccezionali restano separati dagli edge normali.
- MedIR conserva descrittore normalizzato ed edge eccezionali stabili.
- HighIR distingue `SEHTry` e `CxxTry` e preserva VA, type descriptor,
  adjective, offset, azioni, stati e continuation.

Lo structurer HighIR è conservativo: sposta solo uno slice contiguo interamente
contenuto in una regione completa e tratta i nesting dall’interno. Regioni
incrociate, grafi partial, limiti ambigui e funclet esterni mantengono il flusso.

Il backend C emette sintassi MSVC SEH per una regione riducibile a singola
clausola. Poiché HighC è un backend C, catch e cleanup C++ diventano commenti C
deterministici, senza dichiarare che l’output sia C++ compilabile.

## Schema dei metadata LLVM

Ogni funzione eccezione analizzata riceve metadata lossless, anche senza
lowering WinEH nativo:

- attachment `neverd.windows.eh`;
- marker nativo `neverd.windows.eh.native`;
- tabella modulo `neverd.windows.eh.functions`;
- versione schema `3`.

Il record fisso conserva stato, encoding, intervallo, RVA runtime/unwind, tipo e
catena, parola packed, frame, nomi personality, handler, byte unwind,
operazioni/epiloghi, scope SEH, mappe C++, dati GS, diagnostica e rigenerazione.
Il patch richiede versione esatta e intervallo identico all’immagine caricata.

Il lowering SEH x64 usa LLVM WinEH ed emette `invoke`/funclet verifier-clean solo
quando l’intero grafo è rappresentabile. FH3 richiede inoltre:

- x64 COFF, unwind v1/v2, metadata completi e grafo FH3 sincrono valido;
- nessun `noexcept`, async, separated-funclet, GS, FH4 o flag ignoto;
- intervalli nested o disjoint, mai crossing;
- nessun destructor/unwind action, catch-object construction o parent frame;
- handler in un blocco normale senza predecessor né call;
- LLVM `invoke` per ogni operazione protetta potenzialmente unwind.

Altrimenti l’IR resta analizzabile e lossless ma la sostituzione nativa viene
rifiutata. PE entry point, callback TLS e root CRT sono confini da preservare.

## Transazione di patch

Una riscrittura supportata è una singola transazione PE:

1. Validare ogni funzione toccata contro grafo caricato e metadata LLVM.
2. Compilare preservando identità, alignment e trait di sezione e riferimenti
   semantici; externalizzare la personality Windows locale e legare xdata
   all’handler eseguibile originale dimostrato.
3. Conservare runtime function intatte e rimuovere l’intera closure sostituita,
   record chained inclusi.
4. Rilocare code/xdata, fondere e ordinare pdata, rifiutare overlap, dimostrare
   la classe personality e installare una sola exception directory.
5. Preservare CFG, risolvere `.gfids`/`.gehcont`, fondere Guard CF/EH continuation
   e aggiornare load-config. Helper irrisolti abortiscono; CFW, return-flow guard,
   retpoline e XFG restano analysis-only.
6. Riparsare l’immagine byte completa prima della scrittura.

L’estensione del fork LLVM resta generica: il writer final-image conserva trait
di sezione e riferimenti simbolici. PE/MSVC, policy, merge, load-config e
validazione finale restano in NeverD.

Le entry Guard CF/EH continuation originali restano perché i trampoline sono
target indiretti validi. I target generati devono essere nell’emitted code e le
tabelle strettamente ordinate per RVA.

## Validazione dell’immagine finale

Un PE patchato è rifiutato salvo che:

- LLVM accetti COFF e coincidano machine, classe, sezioni, directory, base ed estensione;
- estensioni raw/virtual siano limitate e non sovrapposte;
- exception directory sia file-backed e nell’immagine;
- runtime function siano ordinate, non vuote, non sovrapposte ed eseguibili;
- RVA/header/version/flag/handler/catene unwind x64 siano validi;
- import, export e simboli COFF finali permettano il nuovo parse SEH/FH3;
- record ARM/xdata descrivano versione e intervallo supportati;
- i campi Guard CF/EH esistano quando i flag annunciano tabelle;
- puntatori, conteggi e stride siano in file/immagine con target eseguibili ordinati.

Ogni errore abortisce il patch; non viene scritta un’immagine best-effort.

## Verifica mirata

```bash
cmake --build build --target NeverDLiftTests --parallel 4
build/bin/NeverDLiftTests \
  --gtest_filter='COFFException*:*PatchCOFF_X64.ReconstructsGuardedSEHAndContinuationTable:*PatchCOFF_X64.ReconstructsNativeFH3StateGraph:*PatchCOFF_X64.RejectsInteriorExceptionDirectoryPadding:*PatchCOFF_X64.RebuildsSortedExceptionDirectoryInAppendedSection'
```

La fixture x64 protetta usa `/guard:cf` e `/guard:ehcont` e verifica scope SEH,
Guard, HighC, patch, reload, ordine e target. La fixture FH3 verifica tabelle
fisse, annotazioni, personality, try/catch e IP-to-state. Eseguire anche i casi
ARM quando cambia il parser.

## Estendere il supporto nativo

Ogni nuova forma nativa deve includere nello stesso change:

- parser completo e limitato e invarianti del modello;
- round-trip HighIR/metadata LLVM;
- IR nativo verifier-clean per ogni nuovo grafo accettato;
- conservazione necessaria di sezioni e riferimenti;
- fixture PE collegata per architettura/personality/versione esatta;
- validazione exception-directory, load-config e final-image;
- test di rifiuto espliciti per forme non supportate vicine.

La sola decodifica non amplia l’allow-list. Il criterio è preservare il
comportamento delle eccezioni nell’immagine finale collegata.
