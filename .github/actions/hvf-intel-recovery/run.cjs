'use strict';
const fs = require('node:fs');
const path = require('node:path');
const {spawnSync, execFile} = require('node:child_process');
const {setTimeout: delay} = require('node:timers/promises');
const {performance} = require('node:perf_hooks');
const {startCommand, testEnvironment, captureHostState, UPLOAD_REVISION} =
  require('../hvf-intel-diagnostic/run.cjs');
const {verifyIdentity} = require('../hvf-intel-diagnostic/active-sample.cjs');
const {captureRuntime} = require('../hvf-intel-diagnostic/runtime.cjs');

const LIMIT = 1024 * 1024;
const MAX_PROGRESS = 42;
const save = (file, data) => fs.writeFileSync(file, JSON.stringify(data, null, 2) + '\n', {flag: 'wx'});

function uploadInvocation(entryPoint, environment) {
  const mode = environment['INPUT_UPLOAD-RUNTIME'] || 'default';
  if (!['default', 'jitless'].includes(mode)) throw new Error('unknown uploader runtime');
  if (Object.hasOwn(environment, 'NODE_OPTIONS'))
    throw new Error('controlled uploader requires NODE_OPTIONS to be absent');
  return {kind: 'hvf-uploader-runtime-contract',
    scope: 'recovery-plan-and-progress-children', mode,
    executable: process.execPath,
    arguments: [...(mode === 'jitless' ? ['--jitless'] : []), entryPoint],
    node_options_present: false, parent_exec_argv: [...process.execArgv]};
}

function progress(log, name) {
  let current = 0, stage = '', completed = 0, runs = 0, oks = 0;
  for (const line of log.split('\n')) {
    const iteration = /^Repeating all tests \(iteration (\d+)\) \. \. \.$/.exec(line);
    if (iteration) { current = Number(iteration[1]); stage = 'run'; }
    else if (line === `[ RUN      ] ${name}`) { ++runs; stage = stage === 'run' ? 'ok' : ''; }
    else if (line.startsWith(`[       OK ] ${name} (`)) { ++oks; stage = stage === 'ok' ? 'summary' : ''; }
    else if (line === '[  PASSED  ] 1 test.' && stage === 'summary') {
      completed = current;
      stage = '';
    }
  }
  return {last_started_iteration: current, last_completed_iteration: completed,
    native_runs_in_copy: runs, native_ok_in_copy: oks};
}

function readProgress(directory, name) {
  // One bounded, read-only view; no live directory is handed to the uploader.
  const file = path.join(directory, 'execution/output.log');
  const fd = fs.openSync(file, fs.constants.O_RDONLY | fs.constants.O_NOFOLLOW);
  try {
    const stat = fs.fstatSync(fd);
    if (!stat.isFile()) throw new Error('recovery log is not a regular file');
    const first = Math.max(0, stat.size - LIMIT);
    const buffer = Buffer.alloc(Math.min(stat.size, LIMIT));
    const length = fs.readSync(fd, buffer, 0, buffer.length, first);
    return {buffer: buffer.subarray(0, length), output: {size_at_open: stat.size,
      first_byte: first, copied_bytes: length, truncated: first !== 0},
    progress: progress(buffer.subarray(0, length).toString('utf8'), name)};
  } finally { fs.closeSync(fd); }
}

async function identity(pid, pythonPid, binary, environment, signal) {
  return new Promise(resolve => {
    execFile('/bin/ps', ['-ww', '-p', String(pid), '-o', 'pid=,ppid=,pgid=,comm='],
      {env: environment, signal, encoding: 'utf8', timeout: 2000, killSignal: 'SIGKILL', maxBuffer: 4096},
      (error, stdout, stderr) => resolve({native_pid: pid, python_pid: pythonPid,
        expected_binary: binary, observed_at: new Date().toISOString(), stdout, stderr,
        status: error ? (error.code ?? null) : 0,
        verified: !error && verifyIdentity(stdout, pid, pythonPid, binary)}));
  });
}

async function observeRecovery(operation, {directory, plan, upload, environment, signal,
  pollMs = 250, stallMs = 5000, collectIdentity = identity}) {
  let running = true;
  const stopped = new AbortController();
  const completion = operation.completion.finally(() => { running = false; stopped.abort(); });
  const abort = () => { stopped.abort(); operation.cancel(); };
  signal.addEventListener('abort', abort, {once: true});
  if (signal.aborted) abort();
  const observation = (async () => {
    let sequence = 0, uploadedCompleted = 0, savedBytes = -1, lastCompleted = 0;
    let lastAdvance = performance.now(), stalled = false, registry;
    while (running && !signal.aborted && sequence < MAX_PROGRESS) {
      let state;
      try {
        registry = JSON.parse(fs.readFileSync(path.join(directory, 'children.json'), 'utf8'));
        if (!Array.isArray(registry.process_groups) || registry.process_groups.length !== 1 ||
            !Number.isSafeInteger(registry.process_groups[0]) || registry.process_groups[0] <= 1) {
          throw new Error('recovery native identity is not unique');
        }
        state = readProgress(directory, plan.native_name);
      } catch (error) {
        // Registration is written once immediately after spawn; it can be
        // observed between creation and close. Other IO failures remain fatal.
        if (error.code !== 'ENOENT' && !(error instanceof SyntaxError)) throw error;
      }
      if (state) {
        const completed = state.progress.last_completed_iteration;
        if (completed > lastCompleted) { lastCompleted = completed; lastAdvance = performance.now(); }
        const reason = sequence === 0 ? 'registered' :
          completed >= uploadedCompleted + 25 ? 'progress' :
          !stalled && state.output.size_at_open > savedBytes && performance.now() - lastAdvance >= stallMs ? 'stalled' : null;
        if (reason) {
          const destination = path.join(directory, `progress-${String(sequence).padStart(3, '0')}`);
          fs.mkdirSync(destination);
          fs.writeFileSync(path.join(destination, 'output-tail.log'), state.buffer, {flag: 'wx'});
          save(path.join(destination, 'children.json'), registry);
          const nativeIdentity = await collectIdentity(registry.process_groups[0], operation.child.pid,
            plan.command[0], environment, signal);
          save(path.join(destination, 'metadata.json'), {
            kind: 'instrumented-partial-hvf-recovery-progress', complete_inventory: false,
            commit: plan.commit, controller_commit: plan.controller_commit,
            captured_at: new Date().toISOString(), reason, sequence,
            output: state.output, progress: state.progress, identity: nativeIdentity,
            execution_running_at_seal: running, cancelled: signal.aborted,
          });
          // Completion/cancellation cannot begin a new upload. An upload that
          // already started is awaited while the original native timer runs.
          if (!running || signal.aborted) break;
          await upload(destination, sequence);
          ++sequence;
          uploadedCompleted = completed;
          savedBytes = state.output.size_at_open;
          if (reason === 'stalled') stalled = true;
          continue;
        }
      }
      await delay(pollMs, undefined, {signal: stopped.signal});
    }
  })().catch(error => {
    if (error.name === 'AbortError' && !running && !signal.aborted) return;
    operation.cancel('progress-evidence-failure');
    throw error;
  });
  try {
    const [execution, observed] = await Promise.allSettled([completion, observation]);
    if (observed.status === 'rejected') throw observed.reason;
    if (execution.status === 'rejected') throw execution.reason;
    if (signal.aborted) throw new Error('recovery diagnosis interrupted');
    return execution.value;
  } finally { signal.removeEventListener('abort', abort); }
}

function commandGroup() {
  const operations = new Set();
  const abort = new AbortController();
  return {
    signal: abort.signal,
    cancel() {
      abort.abort();
      for (const operation of operations) operation.cancel();
    },
    start(binary, args, environment, options) {
      if (abort.signal.aborted) throw new Error('recovery diagnosis interrupted');
      const operation = startCommand(binary, args, environment, options);
      operations.add(operation);
      // Attach both handlers without creating an unhandled rejected promise.
      operation.completion.then(() => operations.delete(operation), () => operations.delete(operation));
      return operation;
    },
  };
}

async function finishUpload(operation, record, suffix) {
  try {
    const status = await operation.completion;
    if (status !== 0) {
      throw new Error(`recovery ${suffix} upload failed: exit=${status}; ` +
        `signal=${operation.result.signal}; reason=${operation.result.termination_reason}`);
    }
  } finally {
    // A signalled uploader has a null exit status. Preserve its identity and
    // outcome before cancelling the guest, rather than misattributing the
    // missing native result to a hypervisor failure.
    save(record, {...operation.result, pid: operation.child.pid ?? null, parent_pid: process.pid,
      executable: operation.child.spawnfile, arguments: operation.child.spawnargs.slice(1)});
  }
}

async function main() {
  const group = commandGroup();
  const interrupt = () => group.cancel();
  process.on('SIGINT', interrupt);
  process.on('SIGTERM', interrupt);
  const input = name => {
    const value = process.env[`INPUT_${name.toUpperCase()}`];
    if (!value) throw new Error(`missing action input: ${name}`);
    return value;
  };
  const source = path.resolve(input('source')), evidence = path.resolve(input('evidence'));
  const uploader = path.resolve(input('upload-action'));
  const invocation = uploadInvocation(path.join(uploader, 'dist/upload/index.js'), process.env);
  const helper = path.resolve(__dirname, '../../../scripts/diagnose_hvf_recovery.py');
  const environment = testEnvironment(process.env);
  const revision = spawnSync('git', ['-C', uploader, 'rev-parse', 'HEAD'],
    {env: environment, encoding: 'utf8', timeout: 10000});
  if (revision.status !== 0 || revision.stdout.trim() !== UPLOAD_REVISION) {
    throw new Error('artifact uploader is not the pinned official revision');
  }
  const attempt = process.env.GITHUB_RUN_ATTEMPT, repetitions = input('repetitions');
  if (!/^\d+$/.test(attempt || '') || !['100', '1000'].includes(repetitions)) throw new Error('invalid recovery inputs');
  const experiment = process.env.INPUT_EXPERIMENT || 'recovery';
  if (!['recovery', 'lifecycle', 'instruction', 'instruction-reuse', 'instruction-vcpu-recreate', 'instruction-vm-recreate', 'instruction-owner-recreate', 'owner-failure-controls', 'recovery-reuse', 'recovery-vcpu-recreate', 'recovery-vm-recreate', 'finite-deadline'].includes(experiment)) {
    throw new Error('unknown Intel experiment');
  }
  const common = ['--source', source, '--evidence', evidence];
  const preparation = group.start('python3', [helper, 'prepare', ...common,
    '--build', path.resolve(input('build')), '--repetitions', repetitions, '--experiment', experiment], environment);
  if (await preparation.completion !== 0) throw new Error('recovery preparation failed');
  const plan = JSON.parse(fs.readFileSync(path.join(evidence, 'plan.json'), 'utf8'));
  save(path.join(evidence, 'observer-runtime.json'), captureRuntime());
  save(path.join(evidence, 'uploader-runtime.json'), invocation);
  save(path.join(evidence, 'host-start.json'), await captureHostState(evidence, environment, group.signal));
  const uploads = path.join(evidence, 'uploads');
  fs.mkdirSync(uploads);
  const upload = async (directory, suffix) => {
    const uploadEnvironment = {...process.env,
      INPUT_NAME: `hvf-intel-recovery-attempt-${attempt}-${suffix}`, INPUT_PATH: directory,
      'INPUT_IF-NO-FILES-FOUND': 'error', 'INPUT_RETENTION-DAYS': '7',
      'INPUT_COMPRESSION-LEVEL': '6', INPUT_OVERWRITE: 'false',
      'INPUT_INCLUDE-HIDDEN-FILES': 'false', INPUT_ARCHIVE: 'true'};
    const operation = group.start(invocation.executable, invocation.arguments,
      uploadEnvironment, {timeoutMs: 120000});
    await finishUpload(operation, path.join(uploads, `${suffix}.json`), suffix);
  };
  // Seal the pre-execution plan separately; its directory never becomes live.
  const prepared = path.join(evidence, 'prepared');
  fs.mkdirSync(prepared);
  for (const name of ['plan.json', 'inventory.json', 'host-start.json', 'observer-runtime.json', 'uploader-runtime.json']) {
    fs.copyFileSync(path.join(evidence, name), path.join(prepared, name), fs.constants.COPYFILE_EXCL);
  }
  await upload(prepared, 'plan');
  const operation = group.start('python3', [helper, 'execute', ...common], environment,
    {timeoutMs: (plan.timeout_seconds + 30) * 1000});
  try {
    const status = await observeRecovery(operation, {directory: evidence, plan, environment,
      signal: group.signal, upload: (directory, sequence) => upload(directory, `progress-${String(sequence).padStart(3, '0')}`)});
    if (status !== 0) throw new Error(`native recovery failed: ${status}`);
  } finally {
    save(path.join(evidence, 'controller-status.json'), operation.result);
    save(path.join(evidence, 'host-finish.json'), await captureHostState(evidence, environment));
  }
  if (group.signal.aborted) throw new Error('recovery diagnosis interrupted during final collection');
}

module.exports = {uploadInvocation, progress, readProgress, observeRecovery, commandGroup, finishUpload, LIMIT, MAX_PROGRESS};
if (require.main === module) main().catch(error => { console.error(error.message); process.exitCode = 1; });
