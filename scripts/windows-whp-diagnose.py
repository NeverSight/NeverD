import hashlib,json,os,pathlib,platform,re,shutil,subprocess,time,xml.etree.ElementTree as ET
R=pathlib.Path.cwd();J=R/'build-windows-runtime';E=J/'evidence';E.mkdir(exist_ok=True,parents=True);rows=[]
drivers=[f'Backends/{suite}.{method}/whp{contract}{base}' for suite,method in [('KernelFileObjectExecution','OriginalBufferedNativeFileTransfers'),('KernelIntegerFormattingExecution','OriginalImportsAndLookupPreserveAllValueBits')] for contract in ['Legacy','Checked'] for base in ['Preferred','Rebased']]
darwin=[f'Transports/DarwinProcess.CreatedRegularAttributesKeepIndependentProvidersAndObjectState/{p}_whp' for p in ['MacOSX64','SimulatorX64']]
patterns=drivers+darwin

def run(label,args,timeout=300):
 args=list(map(str,args));start=time.monotonic();p=subprocess.Popen(args,stdout=subprocess.PIPE,stderr=subprocess.PIPE,creationflags=subprocess.CREATE_NEW_PROCESS_GROUP);expired=False
 try:out,err=p.communicate(timeout=timeout)
 except subprocess.TimeoutExpired:
  expired=True;subprocess.run(['taskkill','/PID',str(p.pid),'/T','/F'],capture_output=True,timeout=30);out,err=p.communicate(timeout=30)
 (E/(label+'.stdout')).write_bytes(out);(E/(label+'.stderr')).write_bytes(err)
 row={'label':label,'argv':args,'status':p.returncode,'outer_timeout':expired,'root_reaped':p.poll() is not None,'seconds':time.monotonic()-start,'stdout_sha256':hashlib.sha256(out).hexdigest(),'stderr_sha256':hashlib.sha256(err).hexdigest()};rows.append(row)
 xml=E/(label+'.xml')
 if xml.exists():
  try:row['testcases']=[{'name':x.attrib.get('classname','')+'.'+x.attrib.get('name',''),'status':x.attrib.get('status'),'result':x.attrib.get('result'),'skipped':x.find('skipped') is not None,'failed':x.find('failure') is not None,'error':x.find('error') is not None} for x in ET.parse(xml).getroot().iter('testcase')]
  except ET.ParseError as ex:row['xml_error']=str(ex)
 for n in ['LastTest.log','LastTestsFailed.log']:
  f=J/'Testing/Temporary'/n
  if f.exists():shutil.copy2(f,E/(label+'-'+n))
 (E/'progress.json').write_text(json.dumps(rows,indent=2));print(json.dumps({k:v for k,v in row.items() if k!='testcases'}),flush=True)
 return out.decode(errors='replace')
try:
 (E/'host.json').write_text(json.dumps({'head':os.environ['GITHUB_SHA'],'feature_candidate':'49e7c2582','llvm_mode':'pinned-prebuilt-r6; original source LLVM is separate','platform':platform.platform(),'machine':platform.machine(),'cpu_count':os.cpu_count(),'NEVERD_THREADS':os.environ.get('NEVERD_THREADS'),'original_ctest_bound_seconds':30,'public_guest_limits_unchanged':True},indent=2))
 run('inventory',['ctest','--test-dir',J,'--show-only=json-v1'],120)
 for binary in ['NeverDNativeDriverTests','NeverDDarwinProcessTests','NeverDSourceDialectTests']:
  run('inventory-'+binary,[J/'bin'/(binary+'.exe'),'--gtest_list_tests'],120)
 # Preserve the original 30-second CTest contract, one isolated case first.
 for index,case in enumerate([drivers[0],drivers[4],darwin[0]]):
  label='isolated-ctest-'+str(index);run(label,['ctest','--test-dir',J,'--no-tests=error','-R','^'+re.escape(case)+'(  #.*)?$','--parallel','1','--output-on-failure','--output-junit',E/(label+'.xml')],180)
 # A direct child allows each original compound method to complete and records
 # per-guest timings, without changing any guest limit or CTest registration.
 for index,case in enumerate([drivers[0],drivers[2],drivers[4],drivers[6],*darwin]):
  binary='NeverDDarwinProcessTests' if case in darwin else 'NeverDNativeDriverTests';label='direct-completion-'+str(index);run(label,[J/'bin'/(binary+'.exe'),'--gtest_filter='+case,'--gtest_output=xml:'+str(E/(label+'.xml'))],240)
 label='parallel-original-grid';run(label,['ctest','--test-dir',J,'--no-tests=error','-R','^(?:'+'|'.join(re.escape(x) for x in patterns)+')(  #.*)?$','--parallel','4','--repeat','until-fail:5','--output-on-failure','--output-junit',E/(label+'.xml')],1800)
 label='native-source-dialect';run(label,[J/'bin/NeverDSourceDialectTests.exe','--gtest_filter=*-SourceDialect.CorpusReadsBackAsTheSameC:SourceDialect.OneFile','--gtest_output=xml:'+str(E/(label+'.xml'))],600)
finally:
 products={p.name:hashlib.sha256(p.read_bytes()).hexdigest() for p in (J/'bin').glob('*.exe')};fixtures={str(p.relative_to(J)):hashlib.sha256(p.read_bytes()).hexdigest() for d in [J/'unittests/emulation/fixtures',J/'unittests/emulation/darwin-fixtures'] if d.exists() for p in d.rglob('*') if p.is_file()}
 (E/'receipt.json').write_text(json.dumps({'phase':'bounded native Windows diagnosis; failures remain evidence','head':os.environ['GITHUB_SHA'],'feature_candidate':'49e7c2582','products':products,'fixtures':fixtures,'original_cases':patterns,'runs':rows,'source_llvm_success_not_claimed':True},indent=2))
