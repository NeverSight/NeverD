from pathlib import Path
import hashlib,json,os,subprocess,sys,xml.etree.ElementTree as ET
R=Path.cwd();E=R/'build-ci/msr-evidence';E.mkdir(parents=True,exist_ok=True)
P=R/'lib/emulation/backends/kvm/KvmMachine.cpp';original=P.read_text();(E/'original-KvmMachine.cpp').write_text(original)
assert subprocess.check_output(['git','rev-parse','HEAD^'],text=True).strip()=='f8db93a1fa50f0872cf4395f3085dbe9c1f52b30'
assert subprocess.check_output(['git','hash-object',str(P)],text=True).strip()=='d207abde1d7fe484187d7a3b580c6e609a19151c'
def sha(p):return hashlib.sha256(p.read_bytes()).hexdigest()
def command(label,args,bound):
 with (E/(label+'.stdout')).open('wb') as out,(E/(label+'.stderr')).open('wb') as err:
  r=subprocess.run(args,stdout=out,stderr=err,timeout=bound)
 (E/(label+'.execution.json')).write_text(json.dumps({'args':args,'status':r.returncode,'bound_seconds':bound,'stdout_sha256':sha(E/(label+'.stdout')),'stderr_sha256':sha(E/(label+'.stderr'))},indent=2));return r.returncode
source=original.replace('#include <cerrno>','#include <cerrno>\n#include <cstdio>')
needle='    const int Slots = ioctl(System, KVM_CHECK_EXTENSION, KVM_CAP_NR_MEMSLOTS);'
source=source.replace(needle,'    std::fprintf(stderr, "MSR_FILTER installed default=%u TLS-bitmap=%u\\n", Filter.flags, unsigned(TLS));\n'+needle)
needle='    auto Capture = [&]() -> llvm::Error {\n'
extra='      if (DirectSupervisor) {\n        kvm_regs Probe{};\n        const int Result = ioctl(CPU, KVM_GET_REGS, &Probe);\n        std::fprintf(stderr, "MSR_CAPTURE exit=%u getregs=%d rip=%llx\\n", Run->exit_reason, Result, (unsigned long long)Probe.rip);\n        if (Run->exit_reason == KVM_EXIT_X86_RDMSR || Run->exit_reason == KVM_EXIT_X86_WRMSR)\n          std::fprintf(stderr, "MSR_EVENT reason=%u index=%x data=%llx\\n", Run->msr.reason, Run->msr.index, (unsigned long long)Run->msr.data);\n      }\n'
assert source.count(needle)==1;source=source.replace(needle,needle+extra)
rows=[]
for variant in ['baseline-logged','filter-before-first-entry']:
 current=source
 if variant=='filter-before-first-entry':
  sig='  llvm::Error enableDirectSupervisor() {\n';start=current.index(sig);body=start+len(sig);end=current.index('    const int Slots =',body);setup=current[body:end];conditional=setup.splitlines()[0]
  helper='  llvm::Error configureDirectMSRFilter() {\n'+setup+'    return llvm::Error::success();\n#else\n    return diagnostic::unavailable(diagnostic::KvmCapabilities,\n                                   BackendAvailability::MissingCapability);\n#endif\n  }\n'
  current=current[:start]+helper+sig+conditional+'\n'+current[end:]
  needle='  M->initializeSynchronizedRegisters();\n';assert current.count(needle)==1
  current=current.replace(needle,'  if (DirectSupervisor)\n    if (auto E = M->configureDirectMSRFilter())\n      return E;\n'+needle)
 P.write_text(current);(E/(variant+'-KvmMachine.cpp')).write_text(current)
 assert command(variant+'-build',['cmake','--build','build-ci','--target','NeverDDirectX64Tests','--parallel','4'],1800)==0
 binary=R/'build-ci/bin/NeverDDirectX64Tests';product=sha(binary);observations=[]
 for iteration in range(20):
  label=variant+'-'+str(iteration);xml=E/(label+'.xml');status=command(label,[str(binary),'--gtest_filter=DirectDriverX64.UnretainedMSRStopsBeforeAccessWithCommittedContext','--gtest_output=xml:'+str(xml)],30)
  cases=list(ET.parse(xml).getroot().iter('testcase'));assert len(cases)==1 and cases[0].find('skipped') is None and status in (0,1)
  failed=cases[0].find('failure') is not None;assert failed==(status==1);observations.append({'iteration':iteration,'status':status,'passed':not failed})
 family=variant+'-family';xml=E/(family+'.xml');status=command(family,[str(binary),'--gtest_filter=DirectDriverX64.*','--gtest_output=xml:'+str(xml)],120);cases=list(ET.parse(xml).getroot().iter('testcase'));assert cases and all(c.find('skipped') is None for c in cases)
 row={'variant':variant,'source_sha256':sha(P),'binary_sha256':product,'repetitions':observations,'family_status':status,'family_count':len(cases),'family_failed':[c.get('name') for c in cases if c.find('failure') is not None]};rows.append(row);(E/(variant+'-receipt.json')).write_text(json.dumps(row,indent=2));print(json.dumps({'variant':variant,'passed':sum(v['passed'] for v in observations),'failed':sum(not v['passed'] for v in observations),'family_failed':row['family_failed']}),flush=True)
(E/'receipt.json').write_text(json.dumps({'evidence_collection_completed':True,'production_fix_claimed':False,'original_source_sha256':hashlib.sha256(original.encode()).hexdigest(),'variants':rows},indent=2))
