#!/usr/bin/env python3
"""Incremental computed evidence retains conditions, failures and new entries."""
from pathlib import Path
import sys
import tempfile

from transport_test import BASE, Client


def ok(client, operation, payload=None):
    response = client.call(operation, payload)
    assert response['status'] == 'ok', response
    return response['payload']


def run(executable):
    with tempfile.TemporaryDirectory(prefix='neverd-computed-') as temporary:
        binary = Path(temporary) / 'computed-strings.bin'
        binary.write_bytes(b'independent conditional observation fixture')
        with Client(executable) as client:
            assert 'computed_strings' in client.hello['capabilities']
            ok(client, 'open', {'path': str(binary), 'analysis': False})
            status = ok(client, 'computed_strings_step')
            assert status['done'] == 1 and not status['complete'], status
            attempted = {status['last_function']}
            query = {'generation': status['generation'], 'limit': 1}
            first = ok(client, 'computed_strings', query)['items'][0]
            assert first['text'] == 'MiX<牌>&', first
            assert first['fixture_prepared_view'] == 'med', first
            assert first['inputs'][0]['value'] == 127
            assert int(first['entry_registers'][0]['value'], 16) == int(BASE, 16) + 0x3000
            # Table reads never advance analysis, even with another filter.
            for _ in range(3):
                page = ok(client, 'computed_strings', {**query, 'filter': 'mix'})
                assert page['scan']['done'] == 1 and page['items'][0] == first
            while not status['complete']:
                status = ok(client, 'computed_strings_step')
                assert status['last_function'] not in attempted, status
                attempted.add(status['last_function'])
            assert status['done'] == status['total'] == 601, status
            assert {int(entry, 16) for entry in attempted} == {int(BASE, 16) + 16 * i for i in range(601)}
            assert status['rows'] == 4 and status['failures'] == 2, status
            assert ok(client, 'computed_strings_step') == status
            rows = []
            for offset in range(4):
                page = ok(client, 'computed_strings', {**query, 'offset': offset})
                rows.extend(page['items'])
            assert [r['status'] for r in rows] == ['conditional', 'failed', 'incomplete', 'conditional'], rows
            assert rows[1]['text'] == 'fixture unsupported instruction', rows[1]
            assert rows[-1]['text'] == 'Tail decoded' and rows[-1]['fixture_calls'] == 601, rows[-1]
            page = ok(client, 'computed_strings', {**query, 'sort': 'address', 'descending': True})
            assert page['items'][0] == rows[-1]
            # Input/function edits retire the derived catalogue, not just a
            # page. An old UI token must never select different evidence.
            ok(client, 'function_delete', {'address': BASE})
            stale = client.call('computed_strings', query)
            assert stale['status'] == 'error' and stale['error']['code'] == 'catalogue_changed', stale
            resumed = ok(client, 'computed_strings_step')
            assert resumed['done'] == 1 and resumed['generation'] != status['generation'], resumed
            assert resumed['total'] == 599 and not resumed['complete'], resumed
            assert resumed['rows'] == resumed['failures'] == 1, resumed
            assert resumed['discovery_error'] == '', resumed
            fresh_query = {'generation': resumed['generation'], 'limit': 1}
            fresh = ok(client, 'computed_strings', fresh_query)
            assert fresh['total'] == 1 and len(fresh['items']) == 1, fresh
            assert fresh['items'][0]['status'] == 'failed', fresh
            assert int(fresh['items'][0]['function_address'], 16) == int(BASE, 16) + 16, fresh
            missing = ok(client, 'computed_strings', {**fresh_query, 'filter': 'mix'})
            assert missing['total'] == 0 and missing['items'] == [], missing
    print('computed string catalogue: conditions, paging, full inventory and invalidation passed')


if __name__ == '__main__':
    run(sys.argv[1])
