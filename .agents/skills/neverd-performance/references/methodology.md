# Performance: method

Basis: work on `origin/dev` between 2026-10-27 and 2026-11-12. `SKILL.md` says
what fast code looks like and how one A/B is run. This file is the order of
work that produced the speedups cited here, and the mistakes that cost the
most time. Each step is an instruction, its reason and the case it comes from.

## 1. Fix the bar before the code

- Name the request, the input, the statistic and the opponent before
  optimizing. "Pseudocode on a Windows kernel image: first visit, median and
  90th percentile of a 3,000-view sample, against the reference tool on the
  same host" is a bar. "Make it faster" is not.
- One request has several statistics, and they move independently. On that
  image the first visit and the 90th percentile passed the opponent while the
  median still lost, because half the sample met a callee cap the tail never
  reached. Judge a change on the statistic it was aimed at and report the
  others beside it.
- A bar the user sets is binding as stated. Section 10 records one for changes
  to the Capstone fork.

## 2. Make the measurement trustworthy before reading it

- **Calibrate the instrument.** A sampler's first sample is not the process
  start. Window scripts that took it as the origin were 7 ms late, longer than
  a small function's whole request, so every small-function profile described
  the start of the next request. Fit the offset that puts the most samples
  inside the windows (the worker is idle between requests), then check that an
  aligned window begins in pipeline entry and ends in page encoding.
- **Balance the order and keep a control.** A fixed-order A/B on a loaded host
  showed 17% on the treated group and 11% on a control group the change could
  not reach. The balanced rerun showed a ratio of 0.87 on the 2% of functions
  the change touched and 1.002 on the whole sample (`a5afca5405`). A control
  that improves with the treatment means the run is measuring the host.
- **Take enough pairs.** On a shared 20-core host, four alternating rounds made
  one batch look 3% slower; six rounds put the medians at 86.4 s and 86.9 s of
  user CPU. Take at least six pairs, pin the cores, and report medians with
  their spread. CPU time is the signal for compute and wall time for latency.
- **Compare inside one run.** An idle host is a different host: a worker round
  trip measured 0.2 ms while other work ran and 0.4 ms once everything else
  stopped, because the cores slept between requests. Interleave the two sides;
  never set a number against one taken in an earlier session.
- **Measure the binary you mean.** `bin/neverd` and the test binaries that link
  the library dynamically load `libneverd.so` at run time. Copying the launcher
  compares nothing, and a relink during a run swaps the library under it. Keep
  one library per variant, select it through the loader path, and confirm with
  `ldd`. Unit-test binaries link the archives statically and need their own
  link.
- **Confirm the code runs in the request.** The MBA simplifier takes 0% of a
  default decompile; a change there matters only with the symbolic mode on. A
  1,500-view sample lacked the one function that reset a session cache and
  showed 0.1% where the 3,000-view sample showed 10% of the median band
  (`85e71477f4`). Choose the sample before looking at the candidate, and keep
  the inputs where the candidate loses.
- **Without `perf`,** sample with `tools/neverd-bench/sigprof`. For one thread
  of a GUI process, read `CLOCK_THREAD_CPUTIME_ID` around the call in a
  throwaway test slot.

- **A rest between samples must sleep.** A GUI probe rested 1.5 s between edits
  in an event loop while a handler it had left connected asked a loop that had
  ended to quit at every turn, and `QEventLoop::quit()` wakes the dispatcher
  whatever the loop's state: the GUI thread spun through each rest. The
  workbench was measured hot against a tool that really idled, 0.5 ms where the
  slept path takes 1 to 3, and each run wrote 650 MB of marks until the disk
  was full. After changing how a probe waits, look at the size of one trace
  and at the marks of its loop before reading a number.
- **Read the instrument's output before its figures.** QtTest stops printing
  after 2000 messages (`-maxwarnings 0` lifts it): diagnostics added for a
  failure late in a suite were never printed. Put what a failing step saw
  into the failure's own text.

## 3. Rank by what the request waits for

- Record both shares of a hotspot, CPU and serial wall (`SKILL.md` § Measuring).
- Split by phase and by population before choosing a target. For functions of
  64 bytes or less the pipeline was 44% of a pseudocode request and the C
  writer 24%; larger functions rank differently. A fix that reaches 2% of the
  population cannot move a median.
- Judge per function when the change is per function. An image total hides a
  regression in one shape behind a gain in another.
- Separate first use from steady state. A session's first type declaration
  cost 33 to 54 ms until its preparation moved to the worker's idle time; it
  then cost 1.2 to 2.2 ms. A first visit, a revisit answered from the kept
  render (0.55 ms) and an edit are three requests with three bars.

## 4. Look for the growth before the constant

- The largest gains removed work repeated per item that belongs once per
  phase:
  - a return-tail fold recounted temporary reads for each statement
    (`68daede30b`: one library's decompile fell from 1,207 s to 288 s of CPU);
  - register DCE rewalked the operations for each intrinsic (`e54aa42afd`:
    288 s to 176 s);
  - every pseudocode request converted the same library records
    (`4b28263e42`: 23% of the request);
  - a system-call stub's request scanned 40,850 symbols six times
    (`a5afca5405`).
- **Scale the input and watch the cost per item.** A rename took 0.7 ms on a
  fresh database and 8.3 ms after 2,000 edits, because each save rewrote all
  user state. Row-by-row history made it flat (`ea00172eed`).
- **A cache that is reset is a loop that restarts.** One fallback function's
  jump-table rebuild wiped 19,293 graphs for every later request of the
  session (`85e71477f4`). Count resets, not only hits.
- **Then the constant.** Reading strings only from the byte runs that can hold
  one took a kernel image's string scan from 260 ms to 41 ms on one thread
  and 9.5 ms on eight (`e75c37d2bc`).

## 5. Price the lever before building it

- **Measure the ceiling.** Make the suspected work free behind an experiment
  switch, or count what a perfect memo would save, and see how far the request
  moves. One proposed witness had ceilings of 28%, 9% and 7% on three inputs;
  a version that could be built reached 0%, 4% and 0%. It was not built
  (`6f12ecc01d` is the part that paid).
- **Experiment in minutes.** Compile the changed sources with their commands
  from `compile_commands.json` and link the objects ahead of the build's static
  archives into a private `libneverd.so`. That takes seconds where a rebuild
  after a widely included header takes hours. To learn why a proof refuses a
  case, print its reason under an environment variable in such a library and
  run the saved input through the CLI: on 2026-11-12 that found in minutes one
  cause behind sixteen failing round trips (`984dbf8e70`) that had been filed
  under a guessed cause.
- **Experiment switches never ship.** A bundling step should refuse an engine
  that still contains one.
- **Record the levers that failed, with their numbers.** ThinLTO, another
  allocator, huge pages, probe warm-up, a lower retention cap and a spin-wait
  on the worker thread were each measured and left out; `remaining-work.md`
  § Rejected lists them. A lever nobody wrote down is tried again.

## 6. Prove the fast path exact

- **Identity over a corpus, not a spot check.** Hash every non-timing output,
  the whole payload and not only the text, for each view of a large sample, in
  forward and in reverse order: 2,980 of 2,980 identical both ways is the form
  of the claim. For the C writers, compare every function of the 384-binary
  corpus (60,471 functions) and the whole files of the regression inputs.
- **A memo is exact when turning it off changes nothing.** Add a switch that
  disables it and compare every output; run the sample in reverse to rule out
  history dependence. Two session-cache defects were found this way
  (`85e71477f4`, `bcbbc2926a`).
- **Tell information state from memo error.** A view rendered before the
  image's call summaries are solved may differ from the same view rendered
  after. That is one answer per state. A memo defect is two answers from one
  state.
- **Prove the tests can see a wrong fast path.** Plant a small defect in it,
  such as a bound off by one or a dropped invalidation, and require a test or
  the differential fuzzer to fail. Eight plants were all caught for the name
  index; the string scan's fuzzer was accepted only after planted defects
  failed it. A test binary that links the library statically proves nothing
  when only the shared library is swapped: rebuild the binary, and rebuild
  from clean sources afterwards.
- The identity runs cost more machine time than the change does. They are
  still cheaper than one wrong-output report.

## 7. Compare with another tool on equal work

- Same host, same run, alternating, each tool driven the way its user drives
  it: an interactive request path, not a batch mode that skips work.
- **State what each side computes.** A tool that answers from a stale cache
  has not done the work: one served a caller's old pseudocode after its callee
  was retyped. The fair figure is the time to a correct view.
- **Report where you lose.** The statistic that still loses is the next item
  of work.
- **Start both where a user starts.** The other tool repainted nothing after a
  scripted rename and both views after its own Rename command, so its figure
  is taken from the moment its dialog is accepted. The workbench is then
  measured the same way: focus on the view, the cursor on the name, the key,
  the dialog filled and accepted, the clock started there. A call of the
  session's API after 1.5 s asleep is another regime, two to five times
  slower, with no counterpart in the other tool; report it apart.
- **Say what the figures were taken on.** Offscreen, a dialog does not always
  take the window's activation and give it back; under a window system it
  does, around every dialog, in both tools. A margin smaller than the repaint
  that follows activation is not yet a margin there.
- Decoders follow `tools/neverd-bench/decoder-comparison/README.md`: public
  APIs only, one build configuration for every library, modes that do
  different amounts of work kept apart, and acceptance differences left
  visible.

## 8. Latency paths: worker and GUI

- **Measure the real client.** A harness that modelled the GUI assumed pages
  stayed kept across an edit. The real GUI sent blank tags and received whole
  pages (`751d69c211`), and refreshed the listing twice (`085b510e74`).
- **Draw the chain of round trips for one user action.** After an edit, list
  refreshes queued ahead of the visible view's own read on a serial
  dispatcher. Letting the visible read go first took a cold pseudocode view
  from 33 ms to 12.6 ms (`d2c7d58d6e` and the priority change after it).
- **Look for synchronous durability in the interactive path.** An `fsync`
  inside an edit belongs behind the reply, with a stated recovery rule for the
  window it opens.
- **Carry what the next request will ask.** An edit that returns the reads of
  the pages its views show saves their round trips (`1a7897e82b`); renders kept
  through edits that cannot reach them answer a reopen without work
  (`9523b03d49`).
- **What is carried is paid for by whoever waits for the reply.** The lines a
  listing shows cost the edit's reply 0.2 ms warm and about 1.2 ms after a
  rest, and the pseudocode's page waits with it. Carry them where the listing
  is the view worked in, or where the other view already shows the result;
  elsewhere let the listing read them behind the reply.
- **Ask which view is worked in, not which has the focus.** An edit made
  through a dialog leaves as the dialog closes, and whether the view has the
  focus back by then differed from run to run. The window already knows the
  last analysis view used; the rule reads that.
- **Give the answer to the view its reader looks at first.** With one turn for
  every view, a listing took its carried lines ahead of a pseudocode view that
  still had the change to show: 11.5 ms where it had taken 7.6. Two turns,
  the view worked in and the views with something new first, brought it to
  9.4.
- **A synchronous repaint pays for the whole window.** `QWidget::repaint` of
  one row syncs every dirty widget of the top-level. After a dialog closed,
  the row's 9 k pixels came with 598 k of title bar and functions list, 3 to
  10 ms in half the samples. Time the repaint, and list what else it painted,
  before blaming the code in front of it.
- **Hold a view to what it asks to paint.** A test that counted paint events
  failed when the dock that holds the view updated all it holds. Count at the
  one place where the view asks its viewport to paint, and keep the comparison
  of the window's pixels with the view painted afresh.
- **Look for the session's own commands between an edit and its views.** The
  worker writes the project's files behind an edit, the file watcher answers
  300 ms later with a command, and a command that waits keeps cached answers
  from the reads behind it and a rename from being read ahead. Tests with
  several edits in a row met it one run in four.

## 9. Land it so it stays

- One cause per commit. The message says what repeated, what it cost and how
  identity was shown.
- **After a rebase, read the other side's diff of every file both sides
  changed, clean merge or not.** On 2026-11-12 one side made the demanded-bits
  mask width a variable while the other added a mask of the former fixed width
  to the same file. The two never touched the same lines, and a mask of another
  width is an assertion in a checked build and garbage in a release one
  (`2afaa4d7c8` carries the corrected form). `comm -12` of the two
  `git diff --name-only` lists names the files to read.
- **Correctness batches pass the timing gate too.** A batch that touches shared
  passes runs the alternating whole-file A/B on the regression inputs before it
  is pushed, as a merged PR does (`SKILL.md` § Measuring).
- **`dev` moves under a long verification.** Rebase onto an explicit commit,
  update each moved submodule by name, and verify in proportion to what moved:
  the full grids when a decoder, lifter, pipeline or backend file changed, the
  focused suites otherwise.

## 10. Changes to the Capstone fork

On 2026-11-12 the user told one session: "你对capstone的改动，必须性能上超越iced"
(your changes to Capstone must outperform iced). The sentence was about that
session's changes. Whether it binds every change to the fork is the user's to
say. When this was written nobody had measured the fork against iced from
this skill's side, so the fork's present standing is not claimed here.

- Before changing the fork, or bumping its pointer for a change of your own,
  measure. Build two `compare` binaries, one per Capstone archive, as
  `tools/neverd-bench/decoder-comparison/README.md` describes. Then
  `measure.py --baseline <compare> --candidate <compare> --manifest <corpus>
  --metadata <build-inputs> --out <dir>` times the baseline, the candidate,
  Zydis, XED and iced on every case of the corpus in the scan, format and
  detail modes. Report the candidate against iced and against the baseline
  from the paired samples. If the baseline itself trails iced in a mode, say
  so before changing anything.
- Show field equality separately with `decoder-snapshot`, and run the fork's
  own x86 tests. NeverD's lifter reads the detail contract (opcode bytes,
  operands, access), so the decoder-contract unit binaries and the default
  route of the semantic grid run on the bumped pointer as well.
- An adaptation on NeverD's side of that contract is not a decoder change:
  `1f99eefec3` edits lifter validators only, and the harness links public
  Capstone APIs alone. Check such a change with the decompile timing A/B.
- Practice, not part of the user's sentence: one owner at a time. When
  another session is bumping the pointer, do not edit the fork beside it.

## When the number does not move

- If the statistic you aimed at does not move in a balanced A/B, the change is
  not a performance change, whatever a targeted benchmark says. Land it for
  another stated reason, as the name index was for removing six symbol scans
  from each stub request, or drop it.
- Stop where the remaining levers are approximations. A fast path that cannot
  decide takes the exact path; a speedup that changes an answer is a defect.
