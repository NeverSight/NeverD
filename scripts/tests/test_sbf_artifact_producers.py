from contextlib import redirect_stdout
import io
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys
import time
import tempfile
from types import SimpleNamespace
import unittest
from unittest.mock import patch

from scripts import build_sbf_official_oracle as oracle
from scripts.run_sbf_corpus import qualify, run_manifest
from scripts.sbf_artifacts import ArtifactWorkspace

REPOSITORY = Path(__file__).resolve().parents[2]


@unittest.skipUnless(os.name == 'posix', 'managed SBF products use POSIX leases')
class SbfProducerTests(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name).resolve()
        self.cache = self.root / 'original-assets'
        self.cache.mkdir()
        payload = b'synthetic corpus input'
        (self.cache / 'fixture.so').write_bytes(payload)
        digest = hashlib.sha256(payload).hexdigest()
        self.row = dict(name='fixture', file='fixture.so', size=len(payload),
                        onchain_size=len(payload), sha256=digest,
                        onchain_sha256=digest, loader='loader-v2')
        self.manifest = dict(profile=dict(cluster='mainnet-beta', slot=None),
                             artifacts=[self.row])
        self.worker = self.root / 'worker'
        self.worker.write_text('#!' + sys.executable + '\n' + '''
import json,sys
from pathlib import Path
kind=next(a.split('=',1)[1] for a in sys.argv if a.startswith('--kind='))
for a in sys.argv:
    if a.startswith('--output='): Path(a.split('=',1)[1]).write_text('int f(void) { return 0; }')
print(json.dumps(dict(stage=kind,state='passed')))
''')
        self.worker.chmod(0o755)
        self.compiler = self.root / 'compiler'
        self.compiler.write_text('#!' + sys.executable + '\n' + '''
import sys
from pathlib import Path
if '--version' in sys.argv: print('fixture compiler 1')
else: Path(sys.argv[sys.argv.index('-o')+1]).write_bytes(b'object'*1024)
''')
        self.compiler.chmod(0o755)

    def test_native_objects_are_deleted_even_when_sources_are_retained(self):
        with patch('scripts.run_sbf_corpus.shutil.which', return_value=str(self.compiler)):
            result = qualify(self.row, self.manifest, self.worker, self.cache,
                             self.root / 'out', ['c', 'rust'], 5, 1024**3,
                             retain_sources=True, compile_sources=True)
        for kind in ['c', 'rust']:
            self.assertEqual(result['requests'][kind]['compilation']['state'], 'passed')
            self.assertEqual(result['requests'][kind]['compilation']['object_bytes'], 6144)
        self.assertEqual(list((self.root / 'out').rglob('*.o')), [])
        self.assertTrue((self.root / 'out/fixture/output.c').exists())

    def test_compiler_receipt_failure_also_removes_object(self):
        with patch('scripts.run_sbf_corpus.shutil.which', return_value=str(self.compiler)), \
             patch('scripts.run_sbf_corpus.subprocess.check_output', side_effect=OSError('version failed')):
            with self.assertRaisesRegex(OSError, 'version failed'):
                qualify(self.row, self.manifest, self.worker, self.cache,
                        self.root / 'out', ['c'], 5, 1024**3,
                        retain_sources=True, compile_sources=True)
        self.assertEqual(list((self.root / 'out').rglob('*.o')), [])

    def test_actual_manifest_driver_rotates_bounded_results_and_preserves_reader(self):
        args = SimpleNamespace(manifest=self.root / 'manifest.json', kinds=['c'],
                               timeout=5, memory_bytes=1024**3, jobs=1,
                               retain_sources=True, compile_sources=True,
                               artifact_role='corpus-current', pending_reader='source-audit',
                               build_metadata=None)
        workspace = self.root / 'workspace'
        counts = []
        sizes = []
        with redirect_stdout(io.StringIO()), patch('scripts.run_sbf_corpus.fingerprint', return_value={'fixture': True}), \
             patch('scripts.run_sbf_corpus.shutil.which', return_value=str(self.compiler)):
            for i in range(6):
                output = workspace / f'run-{i}'
                with ArtifactWorkspace(workspace) as artifacts:
                    run_manifest(args, self.manifest, self.worker, self.cache, output, artifacts)
                    self.assertEqual(artifacts.retained_path('source-audit'), output)
                    if i == 0:
                        artifacts.pin('baseline-audit', 'corpus-current')
                    with self.assertRaisesRegex(RuntimeError, 'unfinished artifact reader'):
                        run_manifest(args, self.manifest, self.worker, self.cache,
                                     workspace / 'blocked', artifacts)
                    artifacts.release('source-audit')
                self.assertTrue((workspace / 'run-0/report.json').exists())
                self.assertEqual(list(workspace.rglob('*.o')), [])
                counts.append(len([p for p in workspace.rglob('*') if p.is_file()]))
                sizes.append(sum(p.stat().st_size for p in workspace.rglob('*') if p.is_file()))
            self.assertEqual(len(set(counts[1:])), 1)
            with ArtifactWorkspace(workspace) as artifacts:
                artifacts.release('baseline-audit')
        print('LIFECYCLE_METRIC '+json.dumps(dict(case='manifest-six-rounds',files=counts,bytes=sizes)))
        self.assertFalse((workspace / 'run-0').exists())
        self.assertTrue((workspace / 'run-5/report.json').exists())
        self.assertTrue((self.cache / 'fixture.so').exists())

    def cli_command(self):
        manifest = self.root/'manifest.json'
        row = dict(self.row, asset_id=1,
                   url='https://github.com/example/corpus/releases/download/snapshot/fixture.so')
        manifest.write_text(json.dumps(dict(self.manifest, artifacts=[row], schema=1,
                                           repository='example/corpus', release_tag='snapshot')))
        output = self.root/'workspace/cli'
        metadata = self.root/'build-metadata'
        metadata.mkdir()
        (metadata/'neverd-build-receipt.json').write_text('{"fixture":true}')
        cmd = [sys.executable,str(REPOSITORY/'scripts/run_sbf_corpus.py'),
               '--manifest',str(manifest),'--cache',str(self.cache),'--worker',str(self.worker),
               '--build-metadata',str(metadata),'--output',str(output),'--kinds','c',
               '--retain-sources','--artifact-workspace',str(output.parent),
               '--pending-reader','source-audit','--jobs','1','--timeout','5']
        return cmd, output

    def test_managed_cli_records_metadata_and_protects_pending_audit(self):
        cmd, output = self.cli_command()
        subprocess.run(cmd,check=True,stdout=subprocess.DEVNULL,stderr=subprocess.PIPE)
        report=json.loads((output/'report.json').read_text())
        self.assertEqual(report['summary']['requests']['c']['c'],{'passed':1})
        self.assertEqual(report['identity']['neverd-build-receipt.json'],{'fixture':True})
        second=subprocess.run(cmd,stdout=subprocess.DEVNULL,stderr=subprocess.PIPE,text=True)
        self.assertNotEqual(second.returncode,0)
        self.assertIn('unfinished artifact reader',second.stderr)
        self.assertEqual(report,json.loads((output/'report.json').read_text()))

    def start_slow_cli(self, seconds):
        cmd, output = self.cli_command()
        ready = self.root/'worker-ready'
        self.worker.write_text(self.worker.read_text()+
            f"\nimport os,time\nPath({str(ready)!r}).write_text(str(os.getpid()))\ntime.sleep({seconds})\n")
        process = subprocess.Popen(cmd, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
        deadline = time.monotonic()+5
        while not ready.exists():
            if process.poll() is not None or time.monotonic()>deadline:
                process.kill()
                _, stderr = process.communicate()
                self.fail(stderr.decode())
            time.sleep(0.02)
        return process, output, int(ready.read_text())

    def test_managed_cli_sigterm_cancels_children_and_removes_partial_products(self):
        process, output, worker_pid = self.start_slow_cli(60)
        try:
            process.terminate()
            process.communicate(timeout=5)
        finally:
            if process.poll() is None:
                process.kill()
                process.communicate()
        self.assertFalse(output.exists())
        with self.assertRaises(ProcessLookupError):
            os.kill(worker_pid, 0)
        summary=json.loads((output.parent/'.sbf-artifacts/summaries/corpus-current.json').read_text())
        self.assertEqual(summary['disposition'],'cancelled')
        self.assertIn('summary',summary['metadata'])
        self.assertEqual(json.loads((output.parent/'.sbf-artifacts/state.json').read_text())['entries'],{})

    def test_killed_cli_defers_recovery_until_its_worker_finishes(self):
        process, output, worker_pid = self.start_slow_cli(1.0)
        process.kill()
        process.communicate(timeout=5)
        with self.assertRaisesRegex(RuntimeError,'active producer or reader'):
            with ArtifactWorkspace(output.parent):
                pass
        self.assertTrue(output.exists())
        deadline = time.monotonic()+5
        while True:
            try:
                with ArtifactWorkspace(output.parent):
                    break
            except RuntimeError:
                if time.monotonic()>deadline:
                    raise
                time.sleep(0.02)
        self.assertFalse(output.exists())
        summary=json.loads((output.parent/'.sbf-artifacts/summaries/corpus-current.json').read_text())
        self.assertEqual(summary['disposition'],'interrupted producer recovered')
        self.assertTrue((self.cache/'fixture.so').exists())

    def git(self, *args):
        return subprocess.check_output(['git', '-C', str(self.upstream), *args], text=True).strip()

    def setup_oracle(self):
        self.upstream = self.root / 'sbpf'
        self.upstream.mkdir()
        self.git('init', '-q')
        self.git('config', 'user.name', 'Fixture')
        self.git('config', 'user.email', 'fixture@example.invalid')
        (self.upstream / 'Cargo.toml').write_text('[package]\nname="fixture"\n')
        (self.upstream / 'Cargo.lock').write_text('fixture')
        self.git('add', '.')
        self.git('commit', '-qm', 'fixture')
        self.revision = self.git('rev-parse', 'HEAD')
        self.cargo = self.root / 'cargo'
        self.cargo.write_text('#!' + sys.executable + '\n' + '''
import os,sys
from pathlib import Path
p=Path(sys.argv[sys.argv.index('--target-dir')+1])
p.mkdir(exist_ok=True)
(p/'cache-sentinel').touch(exist_ok=True)
if os.environ.get('FIXTURE_CARGO_FAIL'): sys.exit(17)
b=p/'release/examples/neverd_sbf_official_oracle'
b.parent.mkdir(parents=True,exist_ok=True)
b.write_bytes(b'fixture oracle')
''')
        self.cargo.chmod(0o755)
        self.output = self.root / 'oracle-build/oracle'

    def build_oracle(self, toolchain='fixture'):
        oracle.build_cached_oracle(
            self.upstream, self.output, self.revision, toolchain, str(self.cargo),
            REPOSITORY / 'unittests/sbf/oracle/SBFOfficialOracle.rs',
            REPOSITORY / 'unittests/sbf/SBFOfficialOracleProtocol.def',
            REPOSITORY / 'unittests/sbf/SBFOfficialExecutionFaults.def',
            REPOSITORY / 'unittests/sbf/SBFOfficialExecutionConstants.def',
            REPOSITORY / 'include/neverd/sbf/image/SBFVersions.def')

    def test_oracle_reuses_one_cache_through_success_and_failed_attempts(self):
        self.setup_oracle()
        inventories = []
        sizes = []
        for trial in range(6):
            if trial % 2:
                with patch.dict(os.environ, {'FIXTURE_CARGO_FAIL': '1'}):
                    with self.assertRaises(subprocess.CalledProcessError):
                        self.build_oracle()
            else:
                self.build_oracle()
            self.assertEqual(self.output.read_bytes(), b'fixture oracle')
            cache = self.output.parent / '.sbpf-oracle-build'
            sentinel = cache / 'target/cache-sentinel'
            if trial == 0:
                inode = sentinel.stat().st_ino
            self.assertEqual(sentinel.stat().st_ino, inode)
            self.assertFalse((cache / 'checkout-staging').exists())
            inventories.append(len([p for p in self.output.parent.rglob('*') if p.is_file()]))
            sizes.append(sum(p.stat().st_size for p in self.output.parent.rglob('*') if p.is_file()))
        self.assertEqual(len(set(inventories)), 1)
        print('LIFECYCLE_METRIC '+json.dumps(dict(case='oracle-six-rounds',files=inventories,bytes=sizes)))
        self.assertEqual(self.git('status', '--porcelain'), '')
        self.assertEqual(len(self.git('worktree', 'list', '--porcelain').split('worktree '))-1, 1)
        self.assertEqual(len(json.loads((self.output.parent / '.sbf-artifacts/state.json').read_text())['entries']), 1)

    def test_partial_oracle_checkout_is_removed_before_retry(self):
        self.setup_oracle()
        real_run = oracle.run
        def fail_clone(command, **kwargs):
            if command[:2] == ['git', 'clone']:
                partial = Path(command[-1])
                partial.mkdir()
                (partial/'partial-object').write_bytes(b'x'*8192)
                raise subprocess.CalledProcessError(17, command)
            return real_run(command, **kwargs)
        with patch.object(oracle, 'run', side_effect=fail_clone):
            with self.assertRaises(subprocess.CalledProcessError):
                self.build_oracle()
        cache = self.output.parent/'.sbpf-oracle-build'
        self.assertFalse((cache/'checkout-staging').exists())
        self.assertFalse(self.output.exists())
        self.build_oracle()
        self.assertEqual(self.output.read_bytes(), b'fixture oracle')
        self.assertFalse((cache/'checkout-staging').exists())
        self.assertEqual(self.git('status', '--porcelain'), '')

    def test_failed_oracle_publication_removes_partial_file_and_reuses_cache(self):
        self.setup_oracle()
        def failed_copy(source, destination):
            Path(destination).write_bytes(b'partial publication')
            raise OSError('fixture publication failure')
        with patch.object(oracle.shutil, 'copy2', side_effect=failed_copy):
            with self.assertRaisesRegex(OSError, 'fixture publication failure'):
                self.build_oracle()
        cache = self.output.parent/'.sbpf-oracle-build'
        sentinel = cache/'target/cache-sentinel'
        inode = sentinel.stat().st_ino
        self.assertFalse((self.output.parent/'.oracle.tmp').exists())
        self.assertFalse(self.output.exists())
        self.build_oracle()
        self.assertEqual(sentinel.stat().st_ino, inode)
        self.assertEqual(self.output.read_bytes(), b'fixture oracle')

    def test_oracle_preserves_unrelated_changes_before_cache_invalidation(self):
        self.setup_oracle()
        self.build_oracle()
        cache = self.output.parent / '.sbpf-oracle-build'
        changed = cache / 'upstream/Cargo.toml'
        changed.write_text('user edit')
        subprocess.run(['git', '-C', str(cache / 'upstream'), 'add', 'Cargo.toml'], check=True)
        with self.assertRaisesRegex(RuntimeError, 'unrelated changes'):
            self.build_oracle(toolchain='changed')
        self.assertEqual(changed.read_text(), 'user edit')
        self.assertTrue((cache / 'target/cache-sentinel').exists())
        self.assertEqual(self.output.read_bytes(), b'fixture oracle')


if __name__ == '__main__':
    unittest.main()
