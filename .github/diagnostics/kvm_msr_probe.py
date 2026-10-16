from pathlib import Path
import hashlib,json,subprocess,traceback,xml.etree.ElementTree as ET
R=Path.cwd();E=R/'build-ci/msr-evidence';E.mkdir(parents=True,exist_ok=True);P=R/'lib/emulation/backends/kvm/KvmMachine.cpp'
CANDIDATE='de62c184391e705aadae4f630dbf8a6a63e748d1';EXPECTED_BLOB='c6366b44c04de25e60346f9270dfe76a91196acc';PROBE_HEAD=subprocess.check_output(['git','rev-parse','HEAD'],text=True).strip();assert subprocess.check_output(['git','rev-parse','HEAD^'],text=True).strip()==CANDIDATE
candidate=P.read_bytes();assert subprocess.check_output(['git','hash-object',str(P)],text=True).strip()==EXPECTED_BLOB
original=subprocess.check_output(['git','show','f8db93a1fa50f0872cf4395f3085dbe9c1f52b30:lib/emulation/backends/kvm/KvmMachine.cpp']);(E/'original.cpp').write_bytes(original);(E/'production.cpp').write_bytes(candidate)
assert subprocess.check_output(['git','diff','--name-only',CANDIDATE,'HEAD'],text=True).splitlines()==['.github/diagnostics/kvm_msr_probe.py','.github/workflows/kvm-direct-msr-probe.yml']
def sha(p):return hashlib.sha256(p.read_bytes()).hexdigest()
rows=[];owners=[];accepted=False;error=None

def command(label,args,bound):
 timed=False;status=None
 with (E/(label+'.stdout')).open('wb') as out,(E/(label+'.stderr')).open('wb') as err:
  try:status=subprocess.run(args,stdout=out,stderr=err,timeout=bound).returncode
  except subprocess.TimeoutExpired:timed=True
 rec={'args':args,'status':status,'bound_seconds':bound,'timed_out':timed,'stdout_sha256':sha(E/(label+'.stdout')),'stderr_sha256':sha(E/(label+'.stderr'))};(E/(label+'.execution.json')).write_text(json.dumps(rec,indent=2));assert not timed,label;return status

def listed(text):
 values=[];suite=''
 for raw in text.splitlines():
  line=raw.split(' #',1)[0].rstrip()
  if line and not line.startswith(' ') and line.endswith('.'):suite=line[:-1]
  elif suite and line.startswith('  '):values.append(suite+'.'+line.strip())
 return values
try:
 for variant,data,count in [('original-unlogged',original,20),('production-unlogged',candidate,100)]:
  P.write_bytes(data);assert command(variant+'-build',['cmake','--build','build-ci','--target','NeverDDirectX64Tests','--parallel','4'],1800)==0
  binary=R/'build-ci/bin/NeverDDirectX64Tests';product=sha(binary);observations=[]
  for iteration in range(count):
   label=variant+'-'+str(iteration);xml=E/(label+'.xml');status=command(label,[str(binary),'--gtest_filter=DirectDriverX64.UnretainedMSRStopsBeforeAccessWithCommittedContext','--gtest_output=xml:'+str(xml)],30);cases=list(ET.parse(xml).getroot().iter('testcase'));assert len(cases)==1 and cases[0].find('skipped') is None and status in (0,1)
   failed=cases[0].find('failure') is not None;assert failed==(status==1);observations.append({'iteration':iteration,'status':status,'passed':not failed})
  row={'variant':variant,'source_sha256':sha(P),'binary_sha256':product,'repetitions':observations};rows.append(row);(E/(variant+'-receipt.json')).write_text(json.dumps(row,indent=2));print(json.dumps({'variant':variant,'passed':sum(v['passed'] for v in observations),'failed':sum(not v['passed'] for v in observations)}),flush=True)
 assert P.read_bytes()==candidate and subprocess.check_output(['git','hash-object',str(P)],text=True).strip()==EXPECTED_BLOB
 targets=['NeverDDirectX64Tests','NeverDRunControlTests','NeverDCPUEmulationTests','NeverDKvmRunTests','NeverDKvmStateTransferTests','NeverDKvmCancellationTests'];assert command('owners-build',['cmake','--build','build-ci','--target',*targets,'--parallel','4'],1800)==0
 for name in targets:
  binary=R/'build-ci/bin'/name;inventory=subprocess.check_output([str(binary),'--gtest_list_tests'],text=True);(E/(name+'-inventory.txt')).write_text(inventory);expected=listed(inventory);assert expected
  xml=E/(name+'.xml');status=command(name,[str(binary),'--gtest_output=xml:'+str(xml)],300);cases=list(ET.parse(xml).getroot().iter('testcase'));names=[c.get('classname')+'.'+c.get('name') for c in cases];assert len(names)==len(set(names))==len(expected) and set(names)==set(expected),name
  failed=[c for c in cases if c.find('failure') is not None];skipped=[c for c in cases if c.find('skipped') is not None];required=[c for c in cases if c.get('classname')=='DirectDriverX64' or c.get('name','').endswith('/kvm')]
  assert status==0 and not failed,name
  if name=='NeverDDirectX64Tests':assert len([c for c in required if c.get('classname')=='DirectDriverX64'])==20 and all(c.find('skipped') is None for c in required)
  if name in ['NeverDRunControlTests','NeverDKvmRunTests','NeverDKvmStateTransferTests','NeverDKvmCancellationTests']:assert not skipped,name
  owners.append({'binary':name,'binary_sha256':sha(binary),'expected':expected,'passed':len(cases)-len(skipped),'skipped':[c.get('classname')+'.'+c.get('name') for c in skipped],'native_required_passed':len(required)});print(json.dumps({'owner':name,'passed':len(cases)-len(skipped),'skipped':len(skipped),'required_native':len(required)}),flush=True)
 assert len(rows)==2 and any(not v['passed'] for v in rows[0]['repetitions']) and all(v['passed'] for v in rows[1]['repetitions'])
 assert not subprocess.check_output(['git','status','--porcelain','--untracked-files=all'],text=True)
 accepted=True
except BaseException as ex:error=repr(ex);traceback.print_exc()
finally:
 (E/'receipt.json').write_text(json.dumps({'accepted':accepted,'candidate':CANDIDATE,'candidate_blob':EXPECTED_BLOB,'probe_head':PROBE_HEAD,'uninstrumented_sources':True,'variants':rows,'owners':owners,'error':error,'full_ci_success_claimed':False},indent=2));print(json.dumps({'accepted':accepted,'error':error}),flush=True)
 if not accepted:raise SystemExit(1)
