#!/usr/bin/env python3
"""Execute generated MIPS16 source against MD01172 expectations.

The pinned Unicorn predates MIPS16e2. Never execute the e2 input as an oracle:
GNU assembles it; LowIR has independent field tests; generated old-MIPS16 C is
executed at O0/O2 in both byte orders and both caller return modes.
"""
import argparse
import hashlib
import io
from itertools import product
from pathlib import Path
import random
import subprocess
import sys

from elftools.elf.elffile import ELFFile
import roundtrip
from unicorn import UC_HOOK_CODE
from unicorn import mips_const as mips
sys.path.insert(0, str(Path(__file__).resolve().parents[3] / 'scripts'))
from mips_validation_artifacts import MIPSValidationArtifacts

FUNCTIONS = ('andi', 'ori', 'xori', 'lui', 'ext', 'ins', 'ins_zero',
             'movz', 'movz_zero', 'movn', 'movn_zero', 'movtz', 'movtz_zero',
             'movtn', 'movtn_zero', 'lwl', 'lwr', 'swl', 'swr', 'load_pair',
             'load_pair_reverse', 'store_pair', 'store_pair_reverse', 'gp_add', 'gp_lb', 'gp_lbu',
             'gp_lh', 'gp_lhu', 'gp_lw', 'gp_sb', 'gp_sh', 'gp_sw')
INPUTS = [(0, 0), (1, 0xffffffff), (0xffffffff, 1), (0x80000000, 0x7fffffff),
          (0x89abcdef, 0x12345678), (0x80000000, 0), (0x7fffffff, 0x80000000)]
rng = random.Random(0x16e2)
INPUTS += [(rng.getrandbits(32), rng.getrandbits(32)) for _ in range(9)]
INITIAL = bytes((0x81 + i * 3) & 255 for i in range(16))

def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()

def memory_function(name):
    return name in ('lwl','lwr','swl','swr') or '_pair' in name or (name.startswith('gp_') and name != 'gp_add')

def expected(name, inputs, little):
    a, b = inputs
    memory = bytearray(INITIAL)
    if name.startswith('gp_'):
        if name == 'gp_add': value = a - 1
        else:
            op = name[3:]
            width = 4 if op in ('lw','sw') else 2 if op in ('lh','lhu','sh') else 1
            if op.startswith('s'):
                memory[4:4+width] = (b & ((1 << (width*8))-1)).to_bytes(width,'little' if little else 'big')
                value = 7
            else:
                value = int.from_bytes(memory[4:4+width], 'little' if little else 'big', signed=op in ('lb','lh'))
    elif memory_function(name):
        base = 1 + (a & 3)
        if 'pair' in name:
            if name.startswith('store'):
                memory[base:base+4] = b.to_bytes(4, 'little' if little else 'big')
                value = 7
            else:
                value = int.from_bytes(memory[base:base+4], 'little' if little else 'big')
        else:
            address = base + 1
            offset = address & 3
            left, store = name.endswith('l'), name.startswith('s')
            count = offset + 1 if left == little else 4 - offset
            first = address - offset if left == little else address
            value = b
            for i in range(count):
                lane = (4-count if left else 0) + (i if little else count-1-i)
                if store: memory[first+i] = (b >> (8*lane)) & 255
                else: value = (value & ~(255 << (8*lane))) | (memory[first+i] << (8*lane))
            if store: value = 7
    elif name.startswith('mov'):
        take = bool(b) == ('n' in name)
        value = (0 if name.endswith('_zero') else a) if take else 0x1234
    else:
        value = {'andi': a & 0x8001, 'ori': a | 0x8001, 'xori': a ^ 0x8001,
                 'lui': 0x80010000, 'ext': (a >> 3) & 511,
                 'ins': (b & ~(511 << 3)) | ((a & 511) << 3),
                 'ins_zero': b & ~(511 << 3)}[name]
    return value & 0xffffffff, bytes(memory)

def observe(path, name, inputs, little, tag):
    uc, entry = roundtrip.emulator(path, 32, little, 'm16e2_'+name,
                                  cpu_model=mips.UC_CPU_MIPS32_24KF)
    saved = {reg: 0xa5000000 | reg * 0x10101 for reg in (*range(16,24),28,30)}
    for reg in range(1,32):
        uc.reg_write(getattr(mips,f'UC_MIPS_REG_{reg}'), saved.get(reg,0))
    uc.reg_write(mips.UC_MIPS_REG_SP, roundtrip.STACK)
    uc.reg_write(mips.UC_MIPS_REG_RA, roundtrip.STOP | tag)
    uc.reg_write(mips.UC_MIPS_REG_A0, roundtrip.DATA + 4 if name.startswith('gp_') and name != 'gp_add'
                 else roundtrip.DATA + 1 + (inputs[0]&3) if memory_function(name) else inputs[0])
    uc.reg_write(mips.UC_MIPS_REG_A1, inputs[1])
    uc.mem_write(roundtrip.DATA, INITIAL)
    sentinel = (0x6b51 if tag else 0x24030051).to_bytes(2 if tag else 4,
                                                'little' if little else 'big')
    uc.mem_write(roundtrip.STOP, sentinel)
    returned = []
    uc.hook_add(UC_HOOK_CODE, lambda _u,pc,size,_: returned.append(size) if pc == roundtrip.STOP else None)
    uc.emu_start(entry, roundtrip.STOP+len(sentinel), count=100000)
    if returned != [len(sentinel)] or uc.reg_read(mips.UC_MIPS_REG_V1) != 0x51:
        raise RuntimeError('generated function lost the caller ISA tag')
    if uc.reg_read(mips.UC_MIPS_REG_SP) != roundtrip.STACK:
        raise RuntimeError('generated function changed SP')
    for reg,value in saved.items():
        if uc.reg_read(getattr(mips,f'UC_MIPS_REG_{reg}')) & 0xffffffff != value:
            raise RuntimeError(f'generated function changed saved register {reg}')
    return uc.reg_read(mips.UC_MIPS_REG_V0) & 0xffffffff, bytes(uc.mem_read(roundtrip.DATA,16))

def run(args, artifacts):
    fixture = Path(__file__).with_name('mips16e2.S')
    report = {'complete':False, 'passed':[], 'failures':[],
              'reference':'MD01172 rev 1.00; generated-code execution only; no original-e2 engine',
              'fixture_sha256':digest(fixture),'neverd_sha256':digest(args.neverd)}
    artifacts.write('results.json', report)
    resource = subprocess.check_output([args.clang,'-print-resource-dir'],text=True).strip()
    headers = artifacts.work/'includes'; headers.mkdir()
    (headers/'string.h').write_text('#include <stddef.h>\nvoid *memcpy(void *,const void *,size_t);\nvoid *memset(void *,int,size_t);\n')
    command = lambda argv,log: roundtrip.run_command(artifacts,argv,log)
    for little in (False,True):
        profile = 'mipsel' if little else 'mips'
        assemble = [args.assembler,'-mabi=32','-march=mips32r2','-EL' if little else '-EB']
        link = [args.ld,'-m',f"elf32{'l' if little else 'b'}tsmip",'-static']
        original = artifacts.work/(profile+'.elf')
        obj,log = original.with_suffix('.o'),original.with_suffix('.log')
        command([*assemble,"-mmips16e2",*(['--defsym','LITTLE_ENDIAN=1'] if little else []),fixture,'-o',obj],log)
        command([*link,'-Ttext','0x100000','-e','m16e2_andi',obj,'-o',original],log)
        elf = ELFFile(io.BytesIO(original.read_bytes()))
        flags = elf.get_section_by_name('.MIPS.abiflags').data()
        if int.from_bytes(flags[12:16],'little' if little else 'big') != 0x4400:
            raise RuntimeError('GNU input lost explicit e2 ABI evidence')
        original_hash = digest(original)
        artifacts.discard(obj,log)
        for name,backend,opt in product(FUNCTIONS,('highc','llvmc'),('-O0','-O2')):
            function = 'm16e2_'+name
            stem = artifacts.work/f'{profile}-{function}-{backend}-{opt[1:]}'
            source,assembly,obj,rebuilt,log = (stem.with_suffix(x) for x in ('.c','.s','.o','.elf','.log'))
            try:
                command([args.neverd,'decompile','--func',function,*(['--llvm'] if backend=='llvmc' else []),'-o',source,original],log)
                if '__attribute__((mips16,nomicromips))' not in source.read_text():
                    raise RuntimeError('source lost its explicit ISA mode')
                command([args.clang,f'--target={profile}-linux-gnu','-march=mips32r2','-mabi=32',
                         '-mmicromips','-mno-abicalls','-fno-pic','-ffreestanding','-G0','-fno-addrsig',
                         '-fno-strict-aliasing','-fno-inline','-nostdinc','-isystem',resource+'/include',
                         '-I',headers,opt,'-S',source,'-o',assembly],log)
                command([*assemble,assembly,'-o',obj],log)
                command([*link,'-Ttext','0x200000','-e',function,obj,'-o',rebuilt],log)
                observations=[]
                for inputs,tag in product(INPUTS,(0,1)):
                    want = expected(name,inputs,little)
                    got = observe(rebuilt,name,inputs,little,tag)
                    if got != want: raise RuntimeError(f'{stem.name}: {inputs}/{tag}: {got} != {want}')
                    observations.append({'inputs':inputs,'return_tag':tag,'return':got[0],
                                         'memory':got[1].hex(),'saved_state_preserved':True})
                report['passed'].append({'profile':profile,'function':function,'backend':backend,
                    'optimization':opt,'input_sha256':original_hash,'source_sha256':digest(source),
                    'rebuilt_sha256':digest(rebuilt),'observations':observations})
                print('PASS',stem.name,flush=True)
            except Exception as error:
                artifacts.keep_source(source); artifacts.keep_source(assembly)
                report['failures'].append({'case':stem.name,'error':str(error)})
                artifacts.write('results.json',report)
                raise
            artifacts.write('results.json',report)
            artifacts.discard(source,assembly,obj,rebuilt,log)
        artifacts.discard(original)
    report['complete']=True
    artifacts.write('results.json',report)
    print(len(report['passed']),'MIPS16e2 generated-source groups passed',flush=True)

def main():
    parser=argparse.ArgumentParser(description=__doc__)
    for name in ('neverd','clang','assembler','ld','output'):
        parser.add_argument('--'+name,type=Path,required=True)
    args=parser.parse_args()
    with MIPSValidationArtifacts(args.output,'mips16e2-roundtrip',
            (args.neverd,args.clang,args.assembler,args.ld,Path(__file__).with_name('mips16e2.S'))) as artifacts:
        run(args,artifacts)
if __name__=='__main__': main()
