import hashlib,json,os,pathlib,re,subprocess,time,xml.etree.ElementTree as ET
R=pathlib.Path.cwd();J=R/'build-windows-runtime';E=J/'evidence';E.mkdir(exist_ok=True,parents=True);rows=[];good=False
from scripts.audit_ci_test_results import parse_junit
patterns=[f'Backends/{suite}.{method}/whp{contract}{base}' for suite,method in [('KernelFileObjectExecution','OriginalBufferedNativeFileTransfers'),('KernelIntegerFormattingExecution','OriginalImportsAndLookupPreserveAllValueBits')] for contract in ['Legacy','Checked'] for base in ['Preferred','Rebased']]+[f'Transports/DarwinProcess.CreatedRegularAttributesKeepIndependentProvidersAndObjectState/{p}_whp' for p in ['MacOSX64','SimulatorX64']]

def canonical(n):return n.split('  #',1)[0]
def run(label,args,bound=1200):
 args=list(map(str,args));start=time.monotonic();p=subprocess.Popen(args,stdout=subprocess.PIPE,stderr=subprocess.PIPE,creationflags=subprocess.CREATE_NEW_PROCESS_GROUP);expired=False
 try:out,err=p.communicate(timeout=bound)
 except subprocess.TimeoutExpired:
  expired=True;subprocess.run(['taskkill','/PID',str(p.pid),'/T','/F'],capture_output=True,timeout=30);out,err=p.communicate(timeout=30)
 (E/(label+'.stdout')).write_bytes(out);(E/(label+'.stderr')).write_bytes(err);row={'label':label,'argv':args,'status':p.returncode,'outer_timeout':expired,'root_reaped':p.poll() is not None,'seconds':time.monotonic()-start};rows.append(row);print(json.dumps(row),flush=True);assert p.returncode==0 and not expired,(label,out[-3000:],err[-3000:]);return out.decode(errors='replace')
def audit_ctest(label,expected,repeats,text):
 outcomes=parse_junit(ET.parse(E/(label+'.xml')).getroot());names=[canonical(x.test.name) for x in outcomes];assert len(names)==len(set(names)) and set(names)==set(expected)
 for x in outcomes:assert x.outcome=='passed' and re.search(r'\[\s+PASSED\s+\]\s+1 test\.',x.output) and '[  SKIPPED ]' not in x.output,(label,x.test.name,x.outcome,x.output[-1200:])
 observed=[canonical(m.group(1).strip()) for m in re.finditer(r'Test\s+#\d+:\s+(.+?)\s+\.+\s+Passed',text)];assert len(observed)==len(expected)*repeats and all(observed.count(n)==repeats for n in expected),(label,len(observed),observed)
 rows[-1].update(actual_repeated_cases=len(observed),actual_names=sorted(expected),xml_sha256=hashlib.sha256((E/(label+'.xml')).read_bytes()).hexdigest())
def listing(text):
 result=set();suite=''
 for line in text.splitlines():
  if line and not line[0].isspace() and line.split(' #',1)[0].rstrip().endswith('.'):suite=line.split(' #',1)[0].strip()
  elif line.startswith('  ') and suite:result.add(suite+line.strip().split(' #',1)[0].strip())
 return result
try:
 inventory=json.loads(run('inventory',['ctest','--test-dir',J,'--show-only=json-v1'],180));selected={canonical(x['name']):x for x in inventory['tests'] if canonical(x['name']) in patterns};assert set(selected)==set(patterns)
 for n,x in selected.items():
  props={p['name']:p['value'] for p in x.get('properties',[])}
  if 'KernelFileObject' in n:assert props['TIMEOUT']==120
  elif 'KernelIntegerFormatting' in n:assert props['TIMEOUT']==150
 for label,parallel,repeats in [('isolated-original-grid',1,3),('parallel-original-grid',4,5)]:
  expression='^(?:'+'|'.join(re.escape(x) for x in patterns)+')(  #.*)?$';text=run(label,['ctest','--test-dir',J,'--no-tests=error','-R',expression,'--parallel',str(parallel),'--repeat','until-fail:'+str(repeats),'--output-on-failure','--output-junit',E/(label+'.xml'),'--output-log',E/(label+'.ctest.log')],2400);audit_ctest(label,set(patterns),repeats,text)
 binary=J/'bin/NeverDSourceDialectTests.exe';text=run('source-dialect-inventory',[binary,'--gtest_list_tests'],120);excluded={'SourceDialect.CorpusReadsBackAsTheSameC','SourceDialect.OneFile'};expected=listing(text)-excluded;assert expected and 'SourceDialect.BitFieldsOfTheProgramsTypesComputeWhatCComputes' in expected
 label='native-source-dialect';run(label,[binary,'--gtest_filter=*-'+':'.join(sorted(excluded)),'--gtest_output=xml:'+str(E/(label+'.xml'))],600);cases=list(ET.parse(E/(label+'.xml')).getroot().iter('testcase'));actual={x.attrib['classname']+'.'+x.attrib['name'] for x in cases};assert len(cases)==len(expected) and actual==expected and all(x.find('failure') is None and x.find('skipped') is None and x.attrib.get('status')=='run' and x.attrib.get('result')=='completed' for x in cases);rows[-1].update(actual_passed=sorted(actual),excluded_input_cases=sorted(excluded))
 good=True
finally:
 products={p.name:hashlib.sha256(p.read_bytes()).hexdigest() for p in (J/'bin').glob('*.exe')};fixtures={str(p.relative_to(J)):hashlib.sha256(p.read_bytes()).hexdigest() for folder in [J/'unittests/emulation/fixtures',J/'unittests/emulation/darwin-fixtures'] if folder.exists() for p in folder.rglob('*') if p.is_file()};(E/'receipt.json').write_text(json.dumps({'accepted':good,'head':os.environ['GITHUB_SHA'],'feature_candidate':'d53b8ad082d0413105f8096c0c3b2ec1554b765f','llvm_mode':'Release/prebuilt-r6; source LLVM remains separate','public_guest_defaults_unchanged':True,'products':products,'fixtures':fixtures,'original_cases':patterns,'runs':rows},indent=2))
if not good:raise SystemExit(1)
