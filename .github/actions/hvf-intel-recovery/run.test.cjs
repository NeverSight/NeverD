'use strict';
const assert = require('node:assert/strict');
const test = require('node:test');
const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');
const {once} = require('node:events');
const {spawnSync} = require('node:child_process');
const {setTimeout: delay} = require('node:timers/promises');
const {startCommand, testEnvironment} = require('../hvf-intel-diagnostic/run.cjs');
const {uploadInvocation, readProgress, observeRecovery, commandGroup, finishUpload, LIMIT, MAX_PROGRESS} = require('./run.cjs');
const NAME = 'HvfExecutor.NativeIntelCancellationAndCompletionFailureAllowRetry';
const environment = testEnvironment(process.env);

test('actual action input key selects uploader mode and rejects inherited Node options without mutation', () => {
  const parent = {PATH: '/usr/bin', INPUT_EXPERIMENT: 'recovery-vcpu-recreate'};
  const before = {...parent}, native = testEnvironment(parent);
  for (const mode of ['default', 'jitless']) {
    const invocation = uploadInvocation('/pinned/dist/upload/index.js', {...parent, 'INPUT_UPLOAD-RUNTIME': mode});
    assert.equal(invocation.scope, 'recovery-plan-and-progress-children');
    assert.equal(invocation.node_options_present, false);
    assert.deepEqual(invocation.arguments, [...(mode === 'jitless' ? ['--jitless'] : []),
      '/pinned/dist/upload/index.js']);
    for (const options of ['', '--jitless', '--require=/untrusted.cjs'])
      assert.throws(() => uploadInvocation('/entry.js', {...parent, 'INPUT_UPLOAD-RUNTIME': mode, NODE_OPTIONS: options}),
        /NODE_OPTIONS/);
  }
  assert.throws(() => uploadInvocation('/entry.js', {...parent, 'INPUT_UPLOAD-RUNTIME': '--eval'}), /unknown uploader runtime/);
  assert.equal(uploadInvocation('/entry.js', parent).mode, 'default');
  assert.equal(uploadInvocation('/entry.js', {...parent, INPUT_UPLOAD_RUNTIME: 'jitless'}).mode, 'default');
  assert.deepEqual(parent, before);
  assert.deepEqual(testEnvironment(parent), native);
});

test('actual uploader child receives only its selected runtime flag', t => {
  const directory = fs.mkdtempSync(path.join(os.tmpdir(), 'hvf-uploader-runtime-'));
  t.after(() => fs.rmSync(directory, {recursive: true, force: true}));
  const entry = path.join(directory, 'record.cjs'), parentArgs = [...process.execArgv];
  fs.writeFileSync(entry, 'process.stdout.write(JSON.stringify(process.execArgv));');
  for (const mode of ['default', 'jitless']) {
    const invocation = uploadInvocation(entry, {'INPUT_UPLOAD-RUNTIME': mode});
    const result = spawnSync(invocation.executable, invocation.arguments,
      {env: environment, encoding: 'utf8', timeout: 10000});
    assert.equal(result.status, 0, result.stderr);
    assert.deepEqual(JSON.parse(result.stdout), mode === 'jitless' ? ['--jitless'] : []);
    assert.deepEqual(process.execArgv, parentArgs);
  }
});
const round = n => `Repeating all tests (iteration ${n}) . . .\n[ RUN      ] ${NAME}\n[       OK ] ${NAME} (150 ms)\n[  PASSED  ] 1 test.\n`;
const plan = {native_name: NAME, command: [process.execPath], commit: 'a'.repeat(40), controller_commit: 'b'.repeat(40)};
const collectIdentity = async () => ({verified: true});

function fixture(t) {
  const directory = fs.mkdtempSync(path.join(os.tmpdir(), 'hvf-recovery-'));
  fs.mkdirSync(path.join(directory, 'execution'));
  fs.writeFileSync(path.join(directory, 'execution/output.log'), '');
  t.after(() => fs.rmSync(directory, {recursive: true, force: true}));
  return directory;
}

async function producer(directory, count = 100, options = {}) {
  const {readyDelayMs = 0, ...commandOptions} = options;
  const script = `const fs = require('node:fs');
    const root = ${JSON.stringify(directory)};
    const round = ${round.toString()}; const NAME = ${JSON.stringify(NAME)};
    fs.writeFileSync(root + '/children.json', JSON.stringify({process_groups: [process.pid]}));
    setTimeout(() => process.stdout.write('ready'), ${readyDelayMs}); let n = 0;
    const interval = setInterval(() => {
      fs.appendFileSync(root + '/execution/output.log', round(++n));
      if (n === ${count}) clearInterval(interval);
    }, 5);`;
  const operation = startCommand(process.execPath, ['-e', script], environment,
    {stdio: 'pipe', timeoutMs: 5000, ...commandOptions});
  // A loaded runner can terminate this synthetic child before its handshake.
  // Observe that exit too, so the fixture never leaves a pending test promise.
  await Promise.race([once(operation.child.stdout, 'data'), operation.completion.then(() => {
    throw new Error('synthetic producer exited before readiness');
  })]);
  return operation;
}

function observed(operation, directory, upload, options = {}) {
  return observeRecovery(operation, {directory, plan, upload, environment,
    signal: new AbortController().signal, pollMs: 5, stallMs: 40, collectIdentity, ...options});
}

test('native completes unchanged while its sealed initial upload is still pending', async t => {
  const directory = fixture(t), operation = await producer(directory);
  const saved = [];
  const result = await observed(operation, directory, async destination => {
    const before = fs.readFileSync(path.join(destination, 'output-tail.log'));
    saved.push(destination);
    assert.equal(await operation.completion, 0);
    assert.deepEqual(fs.readFileSync(path.join(destination, 'output-tail.log')), before);
  });
  assert.equal(result, 0);
  assert.equal(saved.length, 1); // Intermediate progress is coalesced, never queued.
  assert.equal(fs.readFileSync(path.join(directory, 'execution/output.log'), 'utf8'),
    Array.from({length: 100}, (_, i) => round(i + 1)).join(''));
});

test('completion waits for an already started immutable upload', async t => {
  const directory = fixture(t), operation = await producer(directory, 10);
  let uploaded = false;
  await observed(operation, directory, async () => {
    await operation.completion;
    await delay(30);
    uploaded = true;
  });
  assert.equal(uploaded, true);
});

test('bounded copies carry exact byte ranges and never read through symlinks', t => {
  const directory = fixture(t), file = path.join(directory, 'execution/output.log');
  fs.writeFileSync(file, 'x'.repeat(LIMIT + 100) + '\n' + round(300));
  const captured = readProgress(directory, NAME);
  assert.equal(captured.buffer.length, LIMIT);
  assert.equal(captured.output.first_byte, fs.statSync(file).size - LIMIT);
  assert.equal(captured.output.truncated, true);
  assert.equal(captured.progress.last_completed_iteration, 300);
  fs.unlinkSync(file);
  fs.symlinkSync(path.join(directory, 'unrelated'), file);
  assert.throws(() => readProgress(directory, NAME), /ELOOP/);
});

test('one stalled snapshot preserves new output before the twenty-fifth completion', async t => {
  const directory = fixture(t), operation = await producer(directory, 8);
  // A separate synthetic operation keeps observing after the producer stops writing.
  await operation.completion;
  let finish;
  const controlled = {child: operation.child, completion: new Promise(resolve => { finish = resolve; }),
    cancel: () => finish(1)};
  const reasons = [];
  await observed(controlled, directory, async destination => {
    reasons.push(JSON.parse(fs.readFileSync(path.join(destination, 'metadata.json'))).reason);
    if (reasons.length === 1) {
      fs.appendFileSync(path.join(directory, 'execution/output.log'),
        `Repeating all tests (iteration 9) . . .\n[ RUN      ] ${NAME}\n`);
    } else setTimeout(() => finish(0), 60);
  });
  assert.deepEqual(reasons, ['registered', 'stalled']);
});

test('hard artifact cap bounds even unexpectedly fast or excess native output', async t => {
  const directory = fixture(t);
  fs.writeFileSync(path.join(directory, 'children.json'), JSON.stringify({process_groups: [1234]}));
  let finish, count = 0;
  const operation = {child: {pid: 123}, completion: new Promise(resolve => { finish = resolve; }),
    cancel: () => finish(1)};
  await observed(operation, directory, async () => {
    ++count;
    fs.appendFileSync(path.join(directory, 'execution/output.log'),
      Array.from({length: 25}, (_, i) => round((count - 1) * 25 + i + 1)).join(''));
    if (count === MAX_PROGRESS) setTimeout(() => finish(0), 30);
    if (count > MAX_PROGRESS) finish(1);
  });
  assert.equal(count, MAX_PROGRESS);
});

test('progress and pending uploads do not reset the original command deadline', async t => {
  const directory = fixture(t), operation = await producer(directory, 10000, {timeoutMs: 2000, graceMs: 50});
  let uploaded = false;
  await assert.rejects(observed(operation, directory, async () => {
    await Promise.allSettled([operation.completion]);
    await delay(30);
    uploaded = true;
  }), /deadline/);
  assert.equal(uploaded, true);
  assert.equal(operation.result.termination_reason, 'deadline');
  assert.ok(Date.parse(operation.result.completed_at) - Date.parse(operation.result.started_at) < 3500);
  assert.notEqual(operation.child.exitCode, 0);
});

test('a producer deadline before readiness rejects instead of stranding the test', async t => {
  await assert.rejects(producer(fixture(t), 10000, {readyDelayMs: 1000, timeoutMs: 100, graceMs: 50}), /deadline/);
});

test('failed upload cancels and retires the still-running producer', async t => {
  const directory = fixture(t), operation = await producer(directory, 10000);
  await assert.rejects(observed(operation, directory, async () => { throw new Error('upload unavailable'); }),
    /upload unavailable/);
  assert.equal(operation.result.termination_reason, 'progress-evidence-failure');
  assert.ok(operation.result.completed_at);
  assert.equal(operation.child.signalCode, 'SIGTERM');
});

test('uploader exit failures and fatal signals retain distinct process evidence', async t => {
  const directory = fixture(t);
  for (const [suffix, script, exit, signal] of [
    ['success', 'process.exit(0)', 0, null],
    ['exit', 'process.exit(7)', 7, null],
    ['signal', "process.kill(process.pid, 'SIGTERM')", null, 'SIGTERM'],
  ]) {
    const operation = startCommand(process.execPath, ['-e', script], environment, {stdio: 'pipe'});
    const record = path.join(directory, `${suffix}.json`);
    const finished = finishUpload(operation, record, suffix);
    if (suffix === 'success') await finished;
    else await assert.rejects(finished, new RegExp(`exit=${exit}; signal=${signal}`));
    const data = JSON.parse(fs.readFileSync(record));
    assert.equal(data.pid, operation.child.pid);
    assert.equal(data.executable, process.execPath);
    assert.deepEqual(data.arguments, ['-e', script]);
    assert.equal(data.exit_status, exit);
    assert.equal(data.signal, signal);
    assert.equal(data.termination_reason, null);
    assert.ok(data.completed_at);
  }
});

test('uploader deadline is preserved even when command completion rejects', async t => {
  const directory = fixture(t), record = path.join(directory, 'timeout.json');
  const operation = startCommand(process.execPath, ['-e', 'setInterval(() => {}, 1000)'],
    environment, {stdio: 'pipe', timeoutMs: 100, graceMs: 50});
  await assert.rejects(finishUpload(operation, record, 'timeout'), /deadline/);
  const data = JSON.parse(fs.readFileSync(record));
  assert.equal(data.termination_reason, 'deadline');
  assert.deepEqual(data.arguments, ['-e', 'setInterval(() => {}, 1000)']);
  assert.equal(data.pid, operation.child.pid);
  assert.ok(data.completed_at);
});

test('external cancellation retires both uploader and the Python-owned native process group', async t => {
  const directory = fixture(t);
  fs.rmSync(path.join(directory, 'execution'), {recursive: true});
  const group = commandGroup();
  const python = `import os, sys\nfrom pathlib import Path\nfrom scripts.diagnose_hvf_methods import NativeChildren\nfrom scripts.run_native_cpu_methods import execute\np=Path(sys.argv[1])\nwith NativeChildren(p):\n execute([sys.executable,'-c','import time; time.sleep(60)'],str(p),os.environ,60,p/'execution')\n`;
  const native = group.start('python3', ['-c', python, directory], environment,
    {stdio: 'pipe', timeoutMs: 10000});
  const upload = group.start(process.execPath, ['-e',
    "process.on('SIGTERM', () => {}); process.stdout.write('ready'); setInterval(() => {}, 1000)"],
  environment, {stdio: 'pipe', timeoutMs: 10000, graceMs: 50});
  const results = Promise.allSettled([native.completion, upload.completion]);
  try {
    await once(upload.child.stdout, 'data');
    for (let i = 0; i < 500 && !fs.existsSync(path.join(directory, 'children.json')); ++i) await delay(10);
    assert.ok(fs.existsSync(path.join(directory, 'children.json')));
    group.cancel();
    const states = await results;
    assert.ok(states.every(result => result.status === 'rejected'));
    assert.equal(upload.result.signal, 'SIGKILL');
    const retirement = JSON.parse(fs.readFileSync(path.join(directory, 'retirement.json')));
    assert.equal(retirement.length, 1);
    assert.equal(retirement[0].retired, true);
    assert.throws(() => process.kill(-retirement[0].pid, 0), /ESRCH/);
    assert.throws(() => group.start('python3', [], environment), /interrupted/);
  } finally { group.cancel(); await results; }
});
