#!/usr/bin/env python3
"""Embedded relocation evidence remains distinct under xref lookup and paging."""
from pathlib import Path
import sys
import tempfile

from transport_test import BASE, Client


def ok(client, operation, payload):
    response = client.call(operation, payload)
    assert response['status'] == 'ok', response
    return response['payload']


def run(server):
    base = int(BASE, 16)
    text = base + 0x3100
    source = base + 0x3010
    with tempfile.TemporaryDirectory(prefix='neverd-embedded-refs-') as directory:
        fixture = Path(directory) / 'embedded-refs.bin'
        fixture.write_bytes(b'independent contained-object reference fixture')
        with Client(server) as client:
            ok(client, 'open', {'path': str(fixture), 'analysis': False})
            rows = ok(client, 'xrefs', {'address': hex(text), 'limit': 100})['items']
            embedded = [r for r in rows if r.get('provenance') == 'embedded_elf_relocation']
            assert len(embedded) == 2, rows
            prefix = next(r for r in embedded if int(r['from'], 16) == source)
            assert int(prefix['to'], 16) == text - 1, prefix
            assert int(prefix['string_address'], 16) == text, prefix
            assert prefix['type'] == 'e' and prefix['embedded_object'] == '0x123'
            assert not prefix['function'] and 'function_address' not in prefix
            assert 'embedded ELF' in prefix['text']
            assert 'text starts at ' in prefix['text']
            exact = ok(client, 'xrefs', {'address': hex(text - 1)})['items']
            assert len(exact) == 1 and exact[0]['to'] == prefix['to'], exact
            outgoing = ok(client, 'xrefs', {'address': hex(source), 'direction': 'from'})
            assert outgoing['total'] == 1 and len(outgoing['items']) == 1, outgoing
            assert outgoing['items'][0]['to'] == prefix['to'], outgoing
            # Lookup of the visible text aliases the prefix; it never invents
            # a second source relocation or changes the true target.
            paged = []
            offset = 0
            while True:
                page = ok(client, 'xrefs', {'address': hex(text), 'offset': offset, 'limit': 1})
                paged.extend(page['items'])
                if page['complete']:
                    break
                offset = page['next_offset']
            assert paged == rows, (paged, rows)
            table = ok(client, 'string_references', {'limit': 100})['items']
            found = [r for r in table if int(r['address'], 16) in (source, source + 4)]
            assert len(found) == 2, table
            assert all('embedded ELF' in r['disasm'] and not r['function'] for r in found), found
            ok(client, 'patch_apply', {'address': hex(base + 1), 'kind': 'bytes', 'bytes': 'a5'})
            changed = ok(client, 'xrefs', {'address': hex(text), 'limit': 100})['items']
            assert all(r.get('provenance') != 'embedded_elf_relocation' for r in changed), changed
            changed = ok(client, 'string_references', {'limit': 100})['items']
            assert all(int(r['address'], 16) not in (source, source + 4) for r in changed), changed
            ok(client, 'patch_revert', {'address': hex(base + 1), 'ranges': [{'address': hex(base + 1), 'size': 1}]})
            restored = ok(client, 'xrefs', {'address': hex(text), 'limit': 100})['items']
            assert restored == rows, (restored, rows)
        collision = Path(directory) / 'embedded-alias-collision.bin'
        collision.write_bytes(b'independent overlapping relocation fixture')
        with Client(server) as client:
            ok(client, 'open', {'path': str(collision), 'analysis': False})
            rows = ok(client, 'xrefs', {'address': hex(text), 'limit': 100})['items']
            embedded = [r for r in rows if r.get('provenance') == 'embedded_elf_relocation']
            assert len(embedded) == 1, embedded
            assert int(embedded[0]['to'], 16) == text, embedded
            assert 'text_prefix_target' not in embedded[0], embedded
            outgoing = ok(client, 'xrefs', {'address': hex(source), 'direction': 'from'})['items']
            assert {int(r['to'], 16) for r in outgoing} == {text - 1, text}, outgoing
    print('embedded refs: exact target, text-prefix alias, both directions and pages passed')


if __name__ == '__main__':
    run(sys.argv[1])
