#!/usr/bin/env python3
"""Owned compiler fixtures: retain finite regions and expose patch residuals.

This small source family is a functional regression, not a held-out accuracy or
performance benchmark. Artifacts record exact inputs, tool versions and flags.
No produced input is executed. GCC profiles are optional when a native x64 Linux
GCC is absent; Clang's x64 ELF and Windows COFF profiles are required.
"""
import argparse
import hashlib
import json
import pathlib
import subprocess
import tempfile


def digest(path):
    return hashlib.sha256(pathlib.Path(path).read_bytes()).hexdigest()


def cyclic_entry_targets(graph):
    """Classify fixture CFGs independently of their reported progress proof."""
    successors = [block['successors'] for block in graph['blocks']]
    reachable = []
    for start in range(len(successors)):
        seen, pending = set(), [start]
        while pending:
            block = pending.pop()
            if block not in seen:
                seen.add(block)
                pending.extend(successors[block])
        reachable.append(seen)
    remaining = set(range(len(successors)))
    assert reachable[graph['entry_block']] == remaining
    entries = []
    while remaining:
        start = min(remaining)
        component = {other for other in remaining
                     if other in reachable[start] and start in reachable[other]}
        remaining -= component
        if len(component) == 1 and start not in successors[start]:
            continue
        targets = {target for source, edges in enumerate(successors)
                   if source not in component for target in edges if target in component}
        if graph['entry_block'] in component:
            targets.add(graph['entry_block'])
        entries.append(targets)
    return entries


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--neverd', required=True)
    parser.add_argument('--clang', required=True)
    parser.add_argument('--gcc')
    parser.add_argument('--fixture', required=True)
    parser.add_argument('--artifacts', required=True)
    args = parser.parse_args()
    output = pathlib.Path(args.artifacts)
    output.mkdir(parents=True, exist_ok=True)
    root = pathlib.Path(tempfile.mkdtemp(prefix='run-', dir=output)).resolve()
    manifest = {'schema': 'neverd.bindiff.compiler-fixtures.v1',
                'source_sha256': digest(args.fixture), 'compilers': {},
                'variants': {}, 'comparisons': [], 'skipped': [], 'status': 'running'}

    def save_manifest():
        (root / 'manifest.json').write_text(json.dumps(manifest, indent=2) + '\n')

    def run(name, command, expected=(0,)):
        result = subprocess.run(command, capture_output=True, text=True, timeout=60)
        (root / (name + '.stdout')).write_text(result.stdout)
        (root / (name + '.stderr')).write_text(result.stderr)
        assert result.returncode in expected, (name, result.returncode, root, result.stderr[-3000:])
        return result

    def compiler(name, path):
        result = run(name + '-version', [path, '--version'])
        manifest['compilers'][name] = {'path': str(pathlib.Path(path).resolve()),
                                       'version': result.stdout.splitlines()[0]}

    compiler('clang', args.clang)
    gcc = None
    if args.gcc:
        compiler('gcc', args.gcc)
        target = run('gcc-target', [args.gcc, '-dumpmachine']).stdout.strip()
        if target.startswith('x86_64') and 'linux' in target and 'clang' not in manifest['compilers']['gcc']['version'].lower():
            gcc = args.gcc
    if not gcc:
        manifest['skipped'].append('GCC profiles: no native x86_64 Linux GCC')

    def build(name, path, flags, source=None):
        source = source or args.fixture
        target = root / (name + ('.obj' if 'coff' in name else '.o'))
        run('build-' + name, [path, '-c', *flags, str(source), '-o', str(target)])
        manifest['variants'][name] = {'path': str(target), 'sha256': digest(target),
                                      'compiler': str(path), 'flags': flags,
                                      'source_sha256': digest(source)}
        return target

    variants = {}
    for level in ('0', '1', '2', '3', 's'):
        variants['clang-elf-O' + level] = build('clang-elf-O' + level, args.clang,
            ['--target=x86_64-unknown-linux-gnu', '-O' + level, '-fno-asynchronous-unwind-tables'])
        if gcc:
            variants['gcc-elf-O' + level] = build('gcc-elf-O' + level, gcc,
                ['-m64', '-O' + level, '-fno-asynchronous-unwind-tables'])
    for level in ('0', '2'):
        variants['clang-coff-O' + level] = build('clang-coff-O' + level, args.clang,
            ['--target=x86_64-pc-windows-msvc', '-O' + level])
    patched = build('clang-elf-patched', args.clang,
                    ['--target=x86_64-unknown-linux-gnu', '-O2', '-fno-asynchronous-unwind-tables', '-DFINAL_ADD=5u'])
    save_manifest()

    def compare(name, a, b, extra=(), require_anchor=True):
        command = [args.neverd, 'diff', '--structural', '-a', str(a), '-b', str(b),
                   '--ignore-names', '--flow-regions', '--json', *extra]
        result = run(name, command, (0, 2))
        document = json.loads(result.stdout)
        assert document['state'] == 'complete', (name, document['stopped_phase'])
        regions = document['flow_regions']
        assert regions['state'] == 'complete' and regions['primary_scope_complete'] and regions['secondary_scope_complete'], (name, regions)
        if require_anchor:
            assert regions['anchor_count'] > 0 and regions['hypothesis_count'] == 1, (name, regions)
        assert regions['semantic_validation'] == 'not_performed'
        assert all(h['open_control_frontier'] for h in regions['hypotheses'])
        assert not document['settings']['use_names']
        for side in ('primary', 'secondary'):
            assert len(document['dataflow'][side]['entries']) == 1
            assert document['dataflow'][side]['entries'][0]['availability'] == 'available'
        manifest['comparisons'].append({'name': name, 'primary_sha256': digest(a),
            'secondary_sha256': digest(b), 'exit': result.returncode,
            'anchors': regions['anchor_count'], 'hypotheses': regions['hypothesis_count'],
            'primary_coverage': [h['primary_coverage'] for h in regions['hypotheses']],
            'secondary_coverage': [h['secondary_coverage'] for h in regions['hypotheses']]})
        save_manifest()
        return document

    baseline = variants['clang-elf-O2']
    for name, target in variants.items():
        if '-elf-' in name:
            compare('same-source-' + name, baseline, target)
    compare('same-source-coff', variants['clang-coff-O2'], variants['clang-coff-O0'])
    saved = root / 'patched.nddiff'
    changed = compare('constant-patch', baseline, patched, ['--save', str(saved)])
    assert any('constants' in pair['changes'] for pair in changed['matches']), changed['matches']
    assert any(pair['similarity'] < 1 for pair in changed['matches'])
    reopened = compare('reopen-patch', baseline, patched, ['--reopen', str(saved)])
    assert reopened == changed
    run('incompatible-abi', [args.neverd, 'diff', '--structural', '-a', str(baseline),
                            '-b', str(variants['clang-coff-O2']), '--flow-regions', '--json'], (1,))
    # The optimized profile uses the ordinary machine LLVM optimizer. Keep the
    # original x64 Med corpus above intact; qualify AArch64 O0/O2 separately.
    profile = ['--dataflow-profile=llvm-ssa-v1']
    for fmt, triple in (('elf', 'aarch64-unknown-linux-gnu'),
                        ('coff', 'aarch64-pc-windows-msvc'),
                        ('macho', 'arm64-apple-macos11')):
        compiled = {}
        for level in ('0', '1', '2', '3', 's'):
            name = f'aarch64-{fmt}-O{level}'
            compiled[level] = build(name, args.clang,
                [f'--target={triple}', '-O' + level, '-fno-asynchronous-unwind-tables'])
        for level, target in compiled.items():
            doc = compare(f'optimized-{fmt}-O2-O{level}', compiled['2'], target, profile)
            assert doc['settings']['dataflow']['profile'] == 'llvm-ssa-v1'
            assert doc['flow_regions']['profile'] == 'llvm-neighborhoods-v1'
        patch = build(f'aarch64-{fmt}-patched', args.clang,
            [f'--target={triple}', '-O2', '-fno-asynchronous-unwind-tables', '-DFINAL_ADD=5u'])
        report = root / f'optimized-{fmt}.nddiff'
        changed = compare(f'optimized-{fmt}-patch', compiled['2'], patch,
                          [*profile, '--save', str(report)])
        assert any('constants' in pair['changes'] for pair in changed['matches'])
        reopened = compare(f'optimized-{fmt}-reopen', compiled['2'], patch,
                           ['--reopen', str(report)])
        assert reopened == changed
        crossed = compare(f'optimized-{fmt}-cross-level-patch', compiled['0'], patch, profile)
        # Native function pairing can abstain across O0/O2. Require the exact
        # changed scalar in both retained SSA graphs instead of inventing a
        # native alignment solely to label the patch.
        for side, value in (('primary', '0x3'), ('secondary', '0x5')):
            graph = crossed['dataflow'][side]['entries'][0]['graph']
            assert any(n['opcode'] == 'add' and any(i[0] == 1 and i[3] == value
                       for i in n['inputs']) for n in graph['nodes']), (fmt, side, graph)
    control = pathlib.Path(args.fixture).with_name('control_regions_fixture.c')
    fragments = [*profile, '--flow-fragments']
    branch_inputs = {}
    for fmt, triple in (('elf', 'aarch64-unknown-linux-gnu'),
                        ('coff', 'aarch64-pc-windows-msvc'),
                        ('macho', 'arm64-apple-macos11')):
        compiled = {}
        for level, value in ((0, 3), (2, 3), (2, 5)):
            name = f'branch-{fmt}-O{level}-{value}'
            compiled[level, value] = build(name, args.clang,
                [f'--target={triple}', f'-O{level}', '-fno-asynchronous-unwind-tables',
                 f'-DFINAL_ADD={value}u'], control)
        a, b, patch = compiled[0, 3], compiled[2, 3], compiled[2, 5]
        branch_inputs[fmt] = a
        report = root / f'fragments-{fmt}.nddiff'
        document = compare(f'fragments-{fmt}', a, b, [*fragments, '--save', str(report)])
        regions = document['flow_regions']
        assert regions['profile'] == 'llvm-fragments-v1'
        assert document['settings']['flow_regions']['fragments'] is True
        assert any(n['opcode'] == 'phi' for n in document['dataflow']['primary']['entries'][0]['graph']['nodes'])
        assert any(n['opcode'] == 'select' for n in document['dataflow']['secondary']['entries'][0]['graph']['nodes'])
        anchor = next(i for i, a in enumerate(regions['anchors']) if a['node_count'] >= 4 and a['frontier_count'])
        boundary = json.loads(run(f'fragments-{fmt}-frontier', [args.neverd, 'diff',
            '--structural', '-a', str(a), '-b', str(b), '--reopen', str(report),
            '--flow-region-anchor', str(anchor), '--flow-region-coordinates', 'frontier',
            '--json'], (0, 2)).stdout)
        assert boundary['view'] == 'frontier' and boundary['boundary_only']
        assert boundary['total'] == regions['anchors'][anchor]['frontier_count']
        assert boundary['rows']
        assert compare(f'fragments-{fmt}-reopen', a, b, ['--reopen', str(report)]) == document
        changed = compare(f'fragments-{fmt}-same-level-patch', b, patch, fragments)
        assert any('constants' in p['changes'] for p in changed['matches'])
        crossed = compare(f'fragments-{fmt}-cross-level-patch', a, patch, fragments, False)
        for side, value in (('primary', '0x3'), ('secondary', '0x5')):
            graph = crossed['dataflow'][side]['entries'][0]['graph']
            assert any(n['opcode'] == 'add' and any(i[0] == 1 and i[3] == value
                       for i in n['inputs']) for n in graph['nodes']), (fmt, side, graph)
    run('fragments-incompatible-abi', [args.neverd, 'diff', '--structural',
        '-a', str(branch_inputs['elf']), '-b', str(branch_inputs['coff']), *fragments, '--json'], (1,))
    # Keep legacy profiles and unresolved loop attribute cells observable.
    for arch, triple in (('x64', 'x86_64-unknown-linux-gnu'),
                         ('aarch64', 'aarch64-unknown-linux-gnu')):
        for family in (1, 2):
            paths = [build(f'control-{arch}-family{family}-O{level}', args.clang,
                     [f'--target={triple}', f'-O{level}', '-fno-asynchronous-unwind-tables',
                      f'-DFAMILY={family}'], control) for level in (0, 2)]
            compare(f'control-{arch}-family{family}', *paths, fragments, False)
    parameter_profile = ['--dataflow-profile=llvm-parameter-ssa-v1']
    deeper_options = root / 'parameter-deeper-options.json'
    deeper_options.write_text(json.dumps({'dataflow': {'profile': 'llvm-parameter-ssa-v1',
                                                       'graph': {'rounds': 16}}}))
    for fmt, triple in (('elf', 'x86_64-unknown-linux-gnu'),
                        ('coff', 'x86_64-pc-windows-msvc'),
                        ('macho', 'x86_64-apple-darwin')):
        def parameter_input(name, level, definitions=()):
            return build(f'parameter-{fmt}-{name}', args.clang,
                         [f'--target={triple}', f'-O{level}',
                          '-fno-asynchronous-unwind-tables', '-DFAMILY=1',
                          *definitions], control)
        a = parameter_input('O0', 0)
        b = parameter_input('O2', 2)
        patch = parameter_input('patch', 2, ['-DFINAL_ADD=5u'])
        branch = parameter_input('branch', 2, ['-DCHANGED_BRANCH'])
        high = parameter_input('high', 2, ['-DHIGH_INPUT'])
        report = root / f'parameter-{fmt}.nddiff'
        document = compare(f'parameter-{fmt}', a, b,
                           [*parameter_profile, '--save', str(report)])
        graphs = [document['dataflow'][side]['entries'][0]['graph']
                  for side in ('primary', 'secondary')]
        assert all(g['schema_version'] == 4 for g in graphs)
        assert any(v['source_bits'] == 64 and v['view_bits'] == 32 and
                   v['elided_truncations'] > 0 for v in graphs[1]['parameter_views'])
        assert len(graphs[0]['nodes']) == len(graphs[1]['nodes'])
        assert any(anchor['phi_block_count'] and anchor['node_count'] >= 4 and
                   min(anchor['primary_origins'], anchor['secondary_origins']) >= 2
                   for anchor in document['flow_regions']['anchors'])
        assert compare(f'parameter-{fmt}-reopen', a, b,
                       ['--reopen', str(report)]) == document
        changed = compare(f'parameter-{fmt}-patch', a, patch, parameter_profile)
        assert any('constants' in pair['changes'] for pair in changed['matches'])
        # The changed final add is not included in a correspondence, while the
        # genuinely shared prefix remains available for review.
        patched_graph = changed['dataflow']['secondary']['entries'][0]['graph']
        final_adds = {i for i, n in enumerate(patched_graph['nodes'])
                      if n['opcode'] == 'add' and
                      any(v[0] == 1 and v[3] == '0x5' for v in n['inputs'])}
        assert final_adds
        assert all(pair[1] not in final_adds for anchor in changed['flow_regions']['anchors']
                   for pair in anchor['nodes'])
        changed_branch = compare(f'parameter-{fmt}-branch', a, branch,
                                 parameter_profile, False)
        branch_graph = changed_branch['dataflow']['secondary']['entries'][0]['graph']
        changed_conditions = {i for i, n in enumerate(branch_graph['nodes'])
                              if n['opcode'] == 'icmp'}
        assert changed_conditions
        assert all(pair[1] not in changed_conditions
                   for anchor in changed_branch['flow_regions']['anchors']
                   for pair in anchor['nodes'])
        assert all(anchor['open_control_frontier'] and anchor['open_data_frontier']
                   for anchor in changed_branch['flow_regions']['anchors'])
        deeper = compare(f'parameter-{fmt}-branch-deeper', a, branch,
                         [*parameter_profile, '--options', str(deeper_options)], False)
        assert deeper['flow_regions']['anchor_count'] == 0
        changed_high = compare(f'parameter-{fmt}-high', a, high,
                               parameter_profile, False)
        high_graph = changed_high['dataflow']['secondary']['entries'][0]['graph']
        assert all(v['elided_truncations'] == 0 for v in high_graph['parameter_views'])
        assert changed_high['flow_regions']['anchor_count'] == 0
    progress_profile = ['--dataflow-profile=llvm-progress-ssa-v1', '--flow-fragments']
    for arch, targets in (
            ('x64', (('elf', 'x86_64-unknown-linux-gnu'),
                     ('coff', 'x86_64-pc-windows-msvc'),
                     ('macho', 'x86_64-apple-darwin'))),
            ('aarch64', (('elf', 'aarch64-unknown-linux-gnu'),
                         ('coff', 'aarch64-pc-windows-msvc'),
                         ('macho', 'arm64-apple-macos11')))):
        for fmt, triple in targets:
            prefix = f'progress-{arch}-{fmt}'

            def loop_input(name, level, definitions=()):
                return build(f'{prefix}-{name}', args.clang,
                             [f'--target={triple}', f'-O{level}',
                              '-fno-asynchronous-unwind-tables', '-DFAMILY=2',
                              *definitions], control)

            a, b = loop_input('O0', 0), loop_input('O2', 2)
            patch = loop_input('patch', 2, ['-DFINAL_ADD=5u'])
            bound = loop_input('bound', 2, ['-DLOOP_MASK=3u'])
            body = loop_input('body', 2, ['-DLOOP_MUL=11u'])
            report = root / f'{prefix}.nddiff'
            document = compare(prefix, a, b,
                               [*progress_profile, '--save', str(report)])
            graphs = [document['dataflow'][side]['entries'][0]['graph']
                      for side in ('primary', 'secondary')]
            assert all(g['schema_version'] == 5 for g in graphs)
            assert graphs[0]['progress']['kind'] == 'counter_loops'
            assert graphs[0]['progress']['loop_count'] == 1
            assert graphs[1]['progress']['kind'] == 'acyclic'
            assert 'willreturn' not in graphs[0]['progress']['source_contract']
            assert 'willreturn' in graphs[0]['progress']['observed_contract']
            regions = document['flow_regions']
            assert regions['profile'] == 'llvm-progress-fragments-v1'
            assert regions['semantic_validation'] == 'not_performed'
            assert any(anchor['node_count'] >= 4 and anchor['frontier_count']
                       for anchor in regions['anchors'])
            assert all(anchor['open_control_frontier'] for anchor in regions['anchors'])
            assert compare(prefix + '-reopen', a, b,
                           ['--reopen', str(report)]) == document
            changed = compare(prefix + '-patch', a, patch, progress_profile, False)
            patched = changed['dataflow']['secondary']['entries'][0]['graph']
            final_adds = {i for i, n in enumerate(patched['nodes'])
                          if n['opcode'] == 'add' and
                          any(v[0] == 1 and v[3] == '0x5' for v in n['inputs'])}
            assert final_adds
            assert all(pair[1] not in final_adds for anchor in changed['flow_regions']['anchors']
                       for pair in anchor['nodes'])
            changed_bound = compare(prefix + '-bound', a, bound, progress_profile, False)
            smaller = changed_bound['dataflow']['secondary']['entries'][0]['graph']
            assert any(n['opcode'] == 'and' and any(v[0] == 1 and v[3] == '0x3'
                       for v in n['inputs']) for n in smaller['nodes'])
            assert len(smaller['nodes']) != len(graphs[1]['nodes'])
            assert all(anchor['open_control_frontier']
                       for anchor in changed_bound['flow_regions']['anchors'])
            changed_body = compare(prefix + '-body', a, body, progress_profile, False)
            altered = changed_body['dataflow']['secondary']['entries'][0]['graph']
            multiplications = {i for i, n in enumerate(altered['nodes'])
                               if n['opcode'] == 'mul' and
                               any(v[0] == 1 and v[3] == '0xb' for v in n['inputs'])}
            assert multiplications
            assert all(pair[1] not in multiplications
                       for anchor in changed_body['flow_regions']['anchors']
                       for pair in anchor['nodes'])
    for family, version, shape, loop_count in ((3, 2, 'multiblock', 1),
                                               (4, 3, 'nested', 2),
                                               (5, 4, 'selected-updates', 1),
                                               (6, 4, 'common-update', 1)):
        progress_profile = [f'--dataflow-profile=llvm-progress-ssa-v{version}', '--flow-fragments']
        for arch, targets in (
                ('x64', (('elf', 'x86_64-unknown-linux-gnu'),
                         ('coff', 'x86_64-pc-windows-msvc'),
                         ('macho', 'x86_64-apple-darwin'))),
                ('aarch64', (('elf', 'aarch64-unknown-linux-gnu'),
                             ('coff', 'aarch64-pc-windows-msvc'),
                             ('macho', 'arm64-apple-macos11')))):
            for fmt, triple in targets:
                prefix = f'{shape}-progress-{arch}-{fmt}'

                def loop_input(name, level, definitions=()):
                    return build(f'{prefix}-{name}', args.clang,
                                 [f'--target={triple}', f'-O{level}',
                                  '-fno-asynchronous-unwind-tables', f'-DFAMILY={family}',
                                  *definitions],
                                 control.with_name('branch_progress_fixture.c')
                                 if family >= 5 else control)

                a, b = loop_input('O0', 0), loop_input('O2', 2)
                if family == 6:
                    a, b = b, a
                patch_level = 0 if family == 5 else 2
                patch = loop_input('patch', patch_level, ['-DFINAL_ADD=5u'])
                bound = loop_input('bound', patch_level, ['-DLOOP_MASK=3u'])
                body = loop_input('body', patch_level, ['-DLOOP_MUL=11u'])
                paired = a if version == 4 else b
                report = root / f'{prefix}.nddiff'
                document = compare(prefix, a, paired,
                                   [*progress_profile, '--save', str(report)])
                graphs = [document['dataflow'][side]['entries'][0]['graph']
                          for side in ('primary', 'secondary')]
                assert all(g['schema_version'] == 4 + version for g in graphs)
                assert graphs[0]['progress']['kind'] == 'counter_loops'
                assert graphs[0]['progress']['loop_count'] == loop_count
                if version == 4:
                    cross_report = root / f'{prefix}-cross-level.nddiff'
                    crossed = compare(prefix + '-cross-level', a, b,
                                      [*progress_profile, '--save', str(cross_report)], False)
                    other_graph = crossed['dataflow']['secondary']['entries'][0]['graph']
                    other = other_graph['progress']
                    multiple_entries = False
                    nowrap_decrement = False
                    if family == 5:
                        entries = cyclic_entry_targets(other_graph)
                        assert len(entries) == 1 and entries[0], (prefix, entries)
                        multiple_entries = len(entries[0]) > 1
                        nodes = other_graph['nodes']
                        decrements = {i for i, node in enumerate(nodes)
                            if node['opcode'] in ('add', 'sub') and node['result_bytes'] == 4
                            and any(v[0] == 1 and v[3] == (
                                '0xffffffff' if node['opcode'] == 'add' else '0x1')
                                for v in node['inputs'])
                            and any(v[0] == 0 and nodes[v[1]]['phi'] for v in node['inputs'])}
                        assert decrements, prefix
                        common_wrap_flags = 3  # LLVM nuw/nsw bits in the wire contract.
                        for update in decrements:
                            flags = int(nodes[update]['semantics'].rsplit(';flags=', 1)[1])
                            assert 0 <= flags <= 3, (prefix, flags)
                            common_wrap_flags &= flags
                        nowrap_decrement = bool(common_wrap_flags)
                    if family == 5 and not multiple_entries and nowrap_decrement:
                        # A retained single-entry bounded decrement must prove
                        # progress, whether it has a common or selected update.
                        assert other['kind'] == 'counter_loops', (prefix, other)
                        assert other['loop_count'] == 1 and other['backedge_count'] == 1, (prefix, other)
                        assert other['guard_count'] == len(other['loops']) > 0, (prefix, other)
                        assert {w['update'] for w in other['loops']} == decrements, prefix
                        assert 'willreturn' not in other['source_contract'], (prefix, other)
                        assert 'willreturn' in other['observed_contract'], (prefix, other)
                    else:
                        # Tail duplication can give the selected-update O2
                        # component multiple entries. v4-v6 require one entry;
                        # the source's bounded loop is not a retained proof.
                        # A missing nowrap flag also requires v4 to abstain.
                        # Common-update O0 also still lacks a v4 premise.
                        assert other['kind'] == 'unproven', (prefix, other)
                        assert not other['loops'], (prefix, other)
                        assert other['source_contract'] == other['observed_contract'], (prefix, other)
                        assert crossed['flow_regions']['anchor_count'] == 0, prefix
                    assert crossed['dataflow']['primary']['entries'][0]['graph'] == graphs[0]
                    assert compare(prefix + '-cross-level-reopen', a, b,
                                   ['--reopen', str(cross_report)], False) == crossed
                    proof = graphs[0]['progress']
                    assert proof['backedge_count'] == 1 and proof['guard_count'] == 2
                    assert len({loop['latch'] for loop in proof['loops']}) == 1
                    assert len({loop['block'] for loop in proof['loops']}) == 2
                else:
                    assert graphs[1]['progress']['kind'] in ('acyclic', 'counter_loops')
                if version == 2:
                    assert all(loop['header'] != loop['block']
                               for loop in graphs[0]['progress']['loops'])
                elif version == 3:
                    proofs = graphs[0]['progress']['loops']
                    assert len({loop['header'] for loop in proofs}) == 2
                    assert len({loop['block'] for loop in proofs}) == 2
                legacy = compare(prefix + '-legacy', a, paired,
                                 [f'--dataflow-profile=llvm-progress-ssa-v{version - 1}', '--flow-fragments'], False)
                old = legacy['dataflow']['primary']['entries'][0]['graph']['progress']
                assert old['kind'] == 'unproven' and old['source_contract'] == old['observed_contract']
                assert 'willreturn' not in graphs[0]['progress']['source_contract']
                assert 'willreturn' in graphs[0]['progress']['observed_contract']
                regions = document['flow_regions']
                assert regions['profile'] == f'llvm-progress-fragments-v{version}'
                assert regions['semantic_validation'] == 'not_performed'
                assert any(anchor['node_count'] >= 4 and anchor['frontier_count']
                           for anchor in regions['anchors'])
                assert all(anchor['open_control_frontier'] for anchor in regions['anchors'])
                assert compare(prefix + '-reopen', a, paired,
                               ['--reopen', str(report)]) == document
                changed = compare(prefix + '-patch', a, patch, progress_profile, version == 4)
                patched = changed['dataflow']['secondary']['entries'][0]['graph']
                final_adds = {i for i, n in enumerate(patched['nodes'])
                              if n['opcode'] == 'add' and
                              any(v[0] == 1 and v[3] == '0x5' for v in n['inputs'])}
                assert final_adds
                assert all(pair[1] not in final_adds for anchor in changed['flow_regions']['anchors']
                           for pair in anchor['nodes'])
                changed_bound = compare(prefix + '-bound', a, bound, progress_profile, version == 4)
                smaller = changed_bound['dataflow']['secondary']['entries'][0]['graph']
                assert any(n['opcode'] == 'and' and any(v[0] == 1 and v[3] == '0x3'
                           for v in n['inputs']) for n in smaller['nodes'])
                assert smaller['nodes'] != graphs[1]['nodes']
                assert all(anchor['open_control_frontier']
                           for anchor in changed_bound['flow_regions']['anchors'])
                changed_body = compare(prefix + '-body', a, body, progress_profile, version == 4)
                altered = changed_body['dataflow']['secondary']['entries'][0]['graph']
                multiplications = {i for i, n in enumerate(altered['nodes'])
                                   if n['opcode'] == 'mul' and
                                   any(v[0] == 1 and v[3] == '0xb' for v in n['inputs'])}
                assert multiplications
                assert all(pair[1] not in multiplications
                           for anchor in changed_body['flow_regions']['anchors']
                           for pair in anchor['nodes'])
                if version == 4:
                    modular = compare(prefix + '-modular', a, b,
                        ['--dataflow-profile=llvm-progress-ssa-v5', '--flow-fragments'],
                        not multiple_entries)
                    graph = modular['dataflow']['secondary']['entries'][0]['graph']
                    proof = graph['progress']
                    if family == 5 and (multiple_entries or nowrap_decrement):
                        # The modular profile must retain v4's exact graph and
                        # decision, including every existing nowrap witness or
                        # the explicit refusal of a multiple-entry component.
                        previous = crossed['dataflow']['secondary']['entries'][0]['graph']
                        assert graph['nodes'] == previous['nodes'], prefix
                        expected = {**previous['progress'], 'loops': [
                            {**witness, 'proof': 'nowrap'}
                            for witness in previous['progress']['loops']]}
                        assert proof == expected, (prefix, proof, expected)
                        if multiple_entries:
                            assert modular['flow_regions']['anchor_count'] == 0
                    else:
                        assert proof['kind'] == 'counter_loops'
                        assert proof['loop_count'] == 1 and proof['guard_count'] == 2
                        assert {w['proof'] for w in proof['loops']} <= {'nowrap', 'modular'}
                        if family == 5:
                            assert proof['backedge_count'] == 1
                            assert {w['proof'] for w in proof['loops']} == {'modular'}
                            assert {w['update'] for w in proof['loops']} == decrements
                            counters = {v[1] for update in decrements
                                for v in other_graph['nodes'][update]['inputs'] if v[0] == 0}
                            guards = {i for i, node in enumerate(other_graph['nodes'])
                                if node['opcode'] == 'icmp' and ';predicate=32' in node['semantics']
                                and any(v[0] == 1 and v[3] == '0x0' for v in node['inputs'])
                                and any(v[0] == 0 and v[1] in counters for v in node['inputs'])}
                            assert {w['compare'] for w in proof['loops']} == guards

    modular_profile = ['--dataflow-profile=llvm-progress-ssa-v5', '--flow-fragments']
    for arch, targets in (
            ('x64', (('elf', 'x86_64-unknown-linux-gnu'),
                     ('coff', 'x86_64-pc-windows-msvc'),
                     ('macho', 'x86_64-apple-darwin'))),
            ('aarch64', (('elf', 'aarch64-unknown-linux-gnu'),
                         ('coff', 'aarch64-pc-windows-msvc'),
                         ('macho', 'arm64-apple-macos11')))):
        for fmt, triple in targets:
            for shape, step, exit_width in (('odd', 3, 1), ('even', 2, 2)):
                prefix = f'modular-{shape}-{arch}-{fmt}'

                def modular_input(name, level, changes=None):
                    definitions = {'COUNT_STEP': step, 'EXIT_WIDTH': exit_width,
                                   'LOOP_MUL': 7, 'FINAL_ADD': 3}
                    definitions.update(changes or {})
                    return build(f'{prefix}-{name}', args.clang,
                        [f'--target={triple}', f'-O{level}', '-fno-asynchronous-unwind-tables',
                         *[f'-D{k}={v}u' for k, v in definitions.items()]],
                        control.with_name('modular_progress_fixture.c'))

                a, b = modular_input('O0', 0), modular_input('O2', 2)
                saved = root / f'{prefix}.nddiff'
                document = compare(prefix, a, a, [*modular_profile, '--save', str(saved)])
                graph = document['dataflow']['primary']['entries'][0]['graph']
                proof = graph['progress']
                assert graph['schema_version'] == 9 and proof['kind'] == 'counter_loops'
                assert proof['loop_count'] == proof['backedge_count'] == proof['guard_count'] == 1
                # The ordinary optimizer can recover a monotone contract for
                # the O0 even-step form; v5 must prefer that existing proof.
                assert {w['proof'] for w in proof['loops']} == {
                    'nowrap' if shape == 'even' else 'modular'}
                assert document['flow_regions']['profile'] == 'llvm-progress-fragments-v5'
                assert compare(prefix + '-reopen', a, a, ['--reopen', str(saved)]) == document
                old = compare(prefix + '-legacy', a, a,
                    ['--dataflow-profile=llvm-progress-ssa-v4', '--flow-fragments'], False)
                old_proof = old['dataflow']['primary']['entries'][0]['graph']['progress']
                if shape == 'odd':
                    assert old_proof['kind'] == 'unproven' and not old_proof['loops']
                else:
                    assert old_proof['kind'] == 'counter_loops'
                qualified = arch == 'aarch64' or shape == 'even'
                cross = compare(prefix + '-cross-level', a, b, modular_profile, qualified)
                optimized_graph = cross['dataflow']['secondary']['entries'][0]['graph']
                optimized = optimized_graph['progress']
                literal_guards = set()
                if not qualified:
                    # Some compiler versions keep the direct 32-bit +3/zero
                    # guard. Classify that retained graph independently of the
                    # reported proof; the odd step visits every counter value.
                    nodes = optimized_graph['nodes']
                    literal_updates = {i for i, node in enumerate(nodes)
                        if node['opcode'] == 'add' and node['result_bytes'] == 4
                        and any(v[0] == 1 and v[3] == '0x3' for v in node['inputs'])
                        and any(v[0] == 0 and nodes[v[1]]['phi']
                                for v in node['inputs'])}
                    literal_guards = {i for i, node in enumerate(nodes)
                        if node['opcode'] == 'icmp' and ';predicate=32' in node['semantics']
                        and any(v[0] == 1 and v[2] == 4 and v[3] == '0x0'
                                for v in node['inputs'])
                        and any(v[0] == 0 and v[1] in literal_updates
                                for v in node['inputs'])}
                    qualified = bool(literal_guards)
                if qualified:
                    assert optimized['kind'] == 'counter_loops'
                    assert {w['proof'] for w in optimized['loops']} == {'modular'}
                    assert any(anchor['node_count'] >= 4 for anchor in cross['flow_regions']['anchors'])
                    assert cross['flow_regions']['hypothesis_count'] == 1
                    if literal_guards:
                        assert {w['compare'] for w in optimized['loops']} == literal_guards
                        assert all(w['update'] in literal_updates for w in optimized['loops'])
                else:
                    # A nonliteral x64 bound still retains the original contract.
                    assert optimized['kind'] == 'unproven'
                    assert optimized['source_contract'] == optimized['observed_contract']
                    assert cross['flow_regions']['anchor_count'] == 0
                for name, changes, opcode, constant in (
                        ('patch', {'FINAL_ADD': 5}, 'add', '0x5'),
                        ('body', {'LOOP_MUL': 11}, 'mul', '0xb')):
                    other = modular_input(name, 0, changes)
                    changed = compare(prefix + '-' + name, a, other, modular_profile)
                    changed_graph = changed['dataflow']['secondary']['entries'][0]['graph']
                    changed_nodes = {i for i, n in enumerate(changed_graph['nodes'])
                        if n['opcode'] == opcode and any(v[0] == 1 and v[3] == constant for v in n['inputs'])}
                    assert changed_nodes
                    assert all(pair[1] not in changed_nodes
                        for anchor in changed['flow_regions']['anchors'] for pair in anchor['nodes'])
                other = modular_input('step', 0, {'COUNT_STEP': 5 if step == 3 else 6})
                changed = compare(prefix + '-step', a, other, modular_profile)
                new_graph = changed['dataflow']['secondary']['entries'][0]['graph']
                updates = {w['update'] for w in new_graph['progress']['loops']}
                assert updates and new_graph['progress']['kind'] == 'counter_loops'
                assert all(pair[1] not in updates for anchor in changed['flow_regions']['anchors']
                           for pair in anchor['nodes'])
                other = modular_input('bound', 0, {'EXIT_WIDTH': exit_width + 1})
                changed = compare(prefix + '-bound', a, other, modular_profile)
                changed_graph = changed['dataflow']['secondary']['entries'][0]['graph']
                assert changed_graph['progress']['kind'] == 'counter_loops'
                assert changed_graph['nodes'] != graph['nodes']
                # Even delta with a single exit misses every odd seed. Both
                # real optimization levels must retain the original contract.
                for level in (0, 2):
                    other = modular_input(f'infinite-O{level}', level,
                                           {'COUNT_STEP': 2, 'EXIT_WIDTH': 1})
                    negative = compare(prefix + f'-infinite-O{level}', a, other, modular_profile, False)
                    unproved = negative['dataflow']['secondary']['entries'][0]['graph']['progress']
                    assert unproved['kind'] == 'unproven' and not unproved['loops']
                    assert unproved['source_contract'] == unproved['observed_contract']
                    assert negative['flow_regions']['anchor_count'] == 0
    parameter_bound_profile = ['--dataflow-profile=llvm-progress-ssa-v6', '--flow-fragments']
    for arch, targets in (
            ('x64', (('elf', 'x86_64-unknown-linux-gnu'),
                     ('coff', 'x86_64-pc-windows-msvc'),
                     ('macho', 'x86_64-apple-darwin'))),
            ('aarch64', (('elf', 'aarch64-unknown-linux-gnu'),
                         ('coff', 'aarch64-pc-windows-msvc'),
                         ('macho', 'arm64-apple-macos11')))):
        for fmt, triple in targets:
            prefix = f'parameter-bound-{arch}-{fmt}'

            def parameter_bound_input(name, level, changes=None):
                definitions = {'COUNT_STEP': 3, 'EXIT_OFFSET': 0,
                               'LOOP_MUL': 7, 'FINAL_ADD': 3}
                definitions.update(changes or {})
                return build(f'{prefix}-{name}', args.clang,
                    [f'--target={triple}', f'-O{level}', '-fno-asynchronous-unwind-tables',
                     *[f'-D{k}={v}u' for k, v in definitions.items()]],
                    control.with_name('parameter_bound_progress_fixture.c'))

            a, b = parameter_bound_input('O0', 0), parameter_bound_input('O2', 2)
            saved = root / f'{prefix}.nddiff'
            document = compare(prefix, a, a, [*parameter_bound_profile, '--save', str(saved)])
            graph = document['dataflow']['primary']['entries'][0]['graph']
            proof = graph['progress']
            assert graph['schema_version'] == 10 and proof['kind'] == 'counter_loops'
            assert proof['loop_count'] == proof['backedge_count'] == proof['guard_count'] == 1
            assert {w['proof'] for w in proof['loops']} == {'parameter_modular'}
            assert all(not p['attributes'] for p in graph['parameter_views'])
            assert document['flow_regions']['profile'] == 'llvm-progress-fragments-v6'
            assert compare(prefix + '-reopen', a, a, ['--reopen', str(saved)]) == document
            old = compare(prefix + '-legacy', a, a,
                          ['--dataflow-profile=llvm-progress-ssa-v5', '--flow-fragments'], False)
            old_proof = old['dataflow']['primary']['entries'][0]['graph']['progress']
            assert old_proof['kind'] == 'unproven' and not old_proof['loops']
            assert old_proof['source_contract'] == old_proof['observed_contract']
            # Finite local fragments can still match in an unproved loop.
            # Their open control boundaries do not establish whole-loop progress.
            assert all(anchor['open_control_frontier'] for anchor in old['flow_regions']['anchors'])
            crossed = compare(prefix + '-cross-level', a, b, parameter_bound_profile)
            optimized = crossed['dataflow']['secondary']['entries'][0]['graph']['progress']
            assert optimized['kind'] == 'counter_loops'
            # AArch64 code generation moves the parameter into the initial
            # counter and leaves a literal exit; retain the preferred v5 basis.
            assert {w['proof'] for w in optimized['loops']} == {
                'modular' if arch == 'aarch64' else 'parameter_modular'}
            for name, changes, opcode, constant in (
                    ('patch', {'FINAL_ADD': 5}, 'add', '0x5'),
                    ('body', {'LOOP_MUL': 11}, 'mul', '0xb')):
                other = parameter_bound_input(name, 0, changes)
                changed = compare(prefix + '-' + name, a, other, parameter_bound_profile)
                changed_graph = changed['dataflow']['secondary']['entries'][0]['graph']
                changed_nodes = {i for i, n in enumerate(changed_graph['nodes'])
                    if n['opcode'] == opcode and any(v[0] == 1 and v[3] == constant for v in n['inputs'])}
                assert changed_nodes
                assert all(pair[1] not in changed_nodes
                    for anchor in changed['flow_regions']['anchors'] for pair in anchor['nodes'])
            other = parameter_bound_input('step', 0, {'COUNT_STEP': 5})
            changed = compare(prefix + '-step', a, other, parameter_bound_profile)
            new_graph = changed['dataflow']['secondary']['entries'][0]['graph']
            updates = {w['update'] for w in new_graph['progress']['loops']}
            assert updates and new_graph['progress']['kind'] == 'counter_loops'
            assert all(pair[1] not in updates for anchor in changed['flow_regions']['anchors']
                       for pair in anchor['nodes'])
            other = parameter_bound_input('bound', 0, {'EXIT_OFFSET': 1})
            changed = compare(prefix + '-bound', a, other, parameter_bound_profile)
            changed_graph = changed['dataflow']['secondary']['entries'][0]['graph']
            assert changed_graph['progress']['kind'] == 'counter_loops'
            assert changed_graph['nodes'] != graph['nodes']
            for level in (0, 2):
                other = parameter_bound_input(f'infinite-O{level}', level, {'COUNT_STEP': 2})
                negative = compare(prefix + f'-infinite-O{level}', a, other, parameter_bound_profile, False)
                unproved = negative['dataflow']['secondary']['entries'][0]['graph']['progress']
                assert unproved['kind'] == 'unproven' and not unproved['loops']
                assert unproved['source_contract'] == unproved['observed_contract']
                assert negative['flow_regions']['anchor_count'] == 0
    invariant_bound_profile = ['--dataflow-profile=llvm-progress-ssa-v7', '--flow-fragments']
    for arch, targets in (
            ('x64', (('elf', 'x86_64-unknown-linux-gnu'),
                     ('coff', 'x86_64-pc-windows-msvc'),
                     ('macho', 'x86_64-apple-darwin'))),
            ('aarch64', (('elf', 'aarch64-unknown-linux-gnu'),
                         ('coff', 'aarch64-pc-windows-msvc'),
                         ('macho', 'arm64-apple-macos11')))):
        for fmt, triple in targets:
            prefix = f'invariant-bound-{arch}-{fmt}'

            def invariant_bound_input(name, level, changes=None):
                definitions = {'COUNT_STEP': 3, 'EXIT_OFFSET': 0,
                               'BOUND_MASK': 255, 'LOOP_MUL': 7, 'FINAL_ADD': 3}
                definitions.update(changes or {})
                return build(f'{prefix}-{name}', args.clang,
                    [f'--target={triple}', f'-O{level}', '-fno-asynchronous-unwind-tables',
                     *[f'-D{k}={v}u' for k, v in definitions.items()]],
                    control.with_name('invariant_bound_progress_fixture.c'))

            a, b = invariant_bound_input('O0', 0), invariant_bound_input('O2', 2)
            saved = root / f'{prefix}.nddiff'
            document = compare(prefix, a, a, [*invariant_bound_profile, '--save', str(saved)])
            graph = document['dataflow']['primary']['entries'][0]['graph']
            proof = graph['progress']
            assert graph['schema_version'] == 11 and proof['kind'] == 'counter_loops'
            assert proof['loop_count'] == proof['backedge_count'] == proof['guard_count'] == 1
            assert {w['proof'] for w in proof['loops']} == {'invariant_modular'}
            assert all(not p['attributes'] for p in graph['parameter_views'])
            assert document['flow_regions']['profile'] == 'llvm-progress-fragments-v7'
            assert compare(prefix + '-reopen', a, a, ['--reopen', str(saved)]) == document
            old = compare(prefix + '-legacy', a, a,
                          ['--dataflow-profile=llvm-progress-ssa-v6', '--flow-fragments'], False)
            old_proof = old['dataflow']['primary']['entries'][0]['graph']['progress']
            assert old_proof['kind'] == 'unproven' and not old_proof['loops']
            assert old_proof['source_contract'] == old_proof['observed_contract']
            # Finite local fragments can still match in an unproved loop.
            # Their open control boundaries do not establish whole-loop progress.
            assert all(anchor['open_control_frontier'] for anchor in old['flow_regions']['anchors'])
            crossed = compare(prefix + '-cross-level', a, b, invariant_bound_profile)
            optimized = crossed['dataflow']['secondary']['entries'][0]['graph']['progress']
            assert optimized['kind'] == 'counter_loops'
            # Both optimized targets use a literal exit. Keep the preferred
            # existing proof instead of relabelling it as expression-relative.
            assert {w['proof'] for w in optimized['loops']} == {'modular'}
            for name, changes, opcode, constant in (
                    ('patch', {'FINAL_ADD': 5}, 'add', '0x5'),
                    ('body', {'LOOP_MUL': 11}, 'mul', '0xb'),
                    ('mask', {'BOUND_MASK': 127}, 'and', '0x7f')):
                other = invariant_bound_input(name, 0, changes)
                changed = compare(prefix + '-' + name, a, other, invariant_bound_profile)
                changed_graph = changed['dataflow']['secondary']['entries'][0]['graph']
                changed_nodes = {i for i, n in enumerate(changed_graph['nodes'])
                    if n['opcode'] == opcode and any(v[0] == 1 and v[3] == constant for v in n['inputs'])}
                assert changed_nodes
                assert all(pair[1] not in changed_nodes
                    for anchor in changed['flow_regions']['anchors'] for pair in anchor['nodes'])
            other = invariant_bound_input('step', 0, {'COUNT_STEP': 5})
            changed = compare(prefix + '-step', a, other, invariant_bound_profile)
            new_graph = changed['dataflow']['secondary']['entries'][0]['graph']
            updates = {w['update'] for w in new_graph['progress']['loops']}
            assert updates and new_graph['progress']['kind'] == 'counter_loops'
            assert all(pair[1] not in updates for anchor in changed['flow_regions']['anchors']
                       for pair in anchor['nodes'])
            other = invariant_bound_input('bound', 0, {'EXIT_OFFSET': 1})
            changed = compare(prefix + '-bound', a, other, invariant_bound_profile)
            changed_graph = changed['dataflow']['secondary']['entries'][0]['graph']
            assert changed_graph['progress']['kind'] == 'counter_loops'
            assert changed_graph['nodes'] != graph['nodes']
            for level in (0, 2):
                other = invariant_bound_input(f'infinite-O{level}', level, {'COUNT_STEP': 2})
                negative = compare(prefix + f'-infinite-O{level}', a, other, invariant_bound_profile, False)
                unproved = negative['dataflow']['secondary']['entries'][0]['graph']['progress']
                assert unproved['kind'] == 'unproven' and not unproved['loops']
                assert unproved['source_contract'] == unproved['observed_contract']
                assert negative['flow_regions']['anchor_count'] == 0
    manifest['status'] = 'passed'
    save_manifest()
    print(f"{len(manifest['comparisons'])} compiler/patch fixture comparisons; artifacts: {root}")
    for skipped in manifest['skipped']:
        print('SKIP:', skipped)


if __name__ == '__main__':
    main()
