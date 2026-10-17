import hashlib,json,os,pathlib,re,subprocess,time,xml.etree.ElementTree as ET
R=pathlib.Path.cwd();J=R/'build-windows-runtime';E=J/'evidence';E.mkdir(exist_ok=True,parents=True);rows=[];good=False
patterns=['ExplicitBackends/X64StatusFlags.CarryInstructionsPreserveAllOtherFlagsAndRegisters/unicornPortable', 'ExplicitBackends/X64StatusFlags.SAHFRestoresOnlyDefinedStatusBits/unicornPortable', 'Backends/KernelFileObjectExecution.OriginalDriverQueriesBorrowedFileInformation/unicornLegacyPreferred', 'Backends/KernelFileObjectExecution.OriginalDriverQueriesBorrowedFileInformation/unicornLegacyRebased', 'Backends/KernelFileObjectExecution.OriginalDriverQueriesBufferedFileInformation/unicornLegacyPreferred', 'Backends/KernelFileObjectExecution.OriginalDriverQueriesBufferedFileInformation/unicornLegacyRebased', 'Backends/KernelFileObjectExecution.OriginalBufferedNativeFileTransfers/unicornLegacyPreferred', 'Backends/KernelFileObjectExecution.OriginalBufferedNativeFileTransfers/unicornLegacyRebased', 'Backends/KernelIntegerFormattingExecution.OriginalImportsAndLookupPreserveAllValueBits/unicornLegacyPreferred', 'Backends/KernelIntegerFormattingExecution.OriginalImportsAndLookupPreserveAllValueBits/unicornLegacyRebased', 'Backends/KernelCRTStringExecution.OriginalImportsAndLookupPreserveCheckedWrites/unicornLegacyPreferred', 'Backends/KernelCRTStringExecution.OriginalImportsAndLookupPreserveCheckedWrites/unicornLegacyRebased', 'Backends/KernelFileSystemExecution.OriginalDriverOpensExistingFiles/unicornLegacyPreferred', 'Backends/KernelFileSystemExecution.OriginalDriverOpensExistingFiles/unicornLegacyRebased', 'Native/DriverContextCapture.CompleteImportedAndDynamicRecordsAcrossLifecycle/whp', 'Native/DriverContextCapture.ConcreteCaptureRetainsRecoveryDependency/whp', 'Preemptive/DriverScheduling.BusyGuestMakesProgressWithoutCooperativeYield/unicorn_driver_PeriodicTimer', 'Preemptive/DriverSchedulingPriority.RuntimePriorityControlsDispatchAndPreemption/unicorn_driver_EqualRoundRobin']

def run(label,args,timeout=1200,env=None,expected=None,require_actual=None):
 start=time.monotonic();args=list(map(str,args));p=subprocess.Popen(args,stdout=subprocess.PIPE,stderr=subprocess.PIPE,env=env,creationflags=subprocess.CREATE_NEW_PROCESS_GROUP);expired=False
 try:out,err=p.communicate(timeout=timeout)
 except subprocess.TimeoutExpired:
  expired=True;subprocess.run(['taskkill','/PID',str(p.pid),'/T','/F'],capture_output=True,timeout=30);out,err=p.communicate(timeout=30)
 row={'label':label,'argv':args,'status':p.returncode,'outer_timeout':expired,'root_reaped':p.poll() is not None,'seconds':time.monotonic()-start};rows.append(row)
 (E/(label+'.stdout')).write_bytes(out);(E/(label+'.stderr')).write_bytes(err);print(json.dumps(row),flush=True)
 assert not expired and p.returncode==0,(label,p.returncode,out[-3000:],err[-3000:])
 if expected is not None:
  xml=E/(label+'.xml');cs=list(ET.parse(xml).getroot().iter('testcase'));assert cs
  ctest=any('ctest'==pathlib.Path(a).stem for a in args[:1]);seen={}
  for c in cs:
   n=c.attrib['name'].split('  #',1)[0] if ctest else c.attrib['classname']+'.'+c.attrib['name']
   assert n not in seen,n;seen[n]=c
  if isinstance(expected,set):assert set(seen)==expected,(label,set(seen)^expected)
  else:assert len(cs)==expected,(label,len(cs),expected)
  assert all(c.find('failure') is None for c in cs),label
  skipped={n for n,c in seen.items() if c.find('skipped') is not None}
  if require_actual is not None:assert not skipped&require_actual,(label,skipped&require_actual)
  if ctest and require_actual is not None:
   for n in require_actual:
    text=seen[n].findtext('system-out') or '';assert re.search(r'\[\s+PASSED\s+\]\s+1 test\.',text) and not re.search(r'\[\s+SKIPPED\s+\]',text),(label,n,text[-1200:])
  if not ctest:assert all(c.attrib.get('status')=='run' and c.attrib.get('result')=='completed' for c in cs if c.find('skipped') is None)
  row.update(observed=len(cs),passed=len(cs)-len(skipped),skipped=sorted(skipped),xml_sha256=hashlib.sha256(xml.read_bytes()).hexdigest())
 return out.decode(errors='replace')

def listing(text):
 names=set();suite=''
 for raw in text.splitlines():
  line=raw.split(' #',1)[0].rstrip()
  if line and not line.startswith(' ') and line.endswith('.'):suite=line[:-1]
  elif suite and line.startswith('  '):names.add(suite+'.'+line.strip())
 return names
try:
 inv=json.loads(run('ctest-inventory',['ctest','--test-dir',J,'--build-config','Release','--show-only=json-v1']))
 (E/'inventory.json').write_text(json.dumps(inv,indent=2));allctest={t['name'].split('  #',1)[0] for t in inv['tests']};assert set(patterns)<=allctest,set(patterns)-allctest
 for name in ['NeverDX64IntegerTests','NeverDNativeDriverTests','NeverDSourceDialectTests','NeverDCPUEmulationTests','NeverDRunControlTests','NeverDInstructionFetchTests']:
  run('inventory-'+name,[J/'bin'/(name+'.exe'),'--gtest_list_tests'])
 for i in range(3):
  label='deadline-regression-'+str(i);n='ExecutionExit.InstructionObserverCannotAdmitEffectsAfterDeadline';run(label,[J/'bin/NeverDCPUEmulationTests.exe','--gtest_filter='+n,'--gtest_output=xml:'+str(E/(label+'.xml'))],180,expected={n},require_actual={n})
  label='rust-linker-'+str(i);ns={'SourceDialect.RustViewComputesWhatCComputes','SourceDialect.BitFieldsOfTheProgramsTypesComputeWhatCComputes'};run(label,[J/'bin/NeverDSourceDialectTests.exe','--gtest_filter='+':'.join(sorted(ns)),'--gtest_output=xml:'+str(E/(label+'.xml'))],300,expected=ns,require_actual=ns)
 selection='|'.join(re.escape(n)+'(  #.*)?$' for n in patterns)
 for threads in ['1',None]:
  env=dict(os.environ)
  if threads is None:env.pop('NEVERD_THREADS',None)
  else:env['NEVERD_THREADS']=threads
  for parallel in [1,4]:
   label='fixed-threads-'+str(threads)+'-parallel-'+str(parallel);run(label,['ctest','--test-dir',J,'--build-config','Release','--no-tests=error','-R',selection,'--parallel',str(parallel),'--output-on-failure','--output-junit',E/(label+'.xml'),'--output-log',E/(label+'.log')],1200,env,expected=set(patterns),require_actual=set(patterns))
 for i in range(2):
  label='fixed-isolated-repeat-'+str(i);run(label,['ctest','--test-dir',J,'--build-config','Release','--no-tests=error','-R',selection,'--parallel','1','--output-on-failure','--output-junit',E/(label+'.xml')],1200,dict(os.environ,NEVERD_THREADS='1'),expected=set(patterns),require_actual=set(patterns))
 label='fixed-parallel-stress-eight';out=run(label,['ctest','--test-dir',J,'--build-config','Release','--no-tests=error','-R',selection,'--parallel','4','--repeat','until-fail:8','--verbose','--output-junit',E/(label+'.xml'),'--output-log',E/(label+'.log')],1200,dict(os.environ,NEVERD_THREADS='1'),expected=set(patterns),require_actual=set(patterns))
 observed=[n.strip() for n in re.findall(r'\[ RUN\s+\] ([^\r\n]+)',out)]
 assert len(observed)==8*len(patterns) and all(observed.count(n)==8 for n in patterns),(label,len(observed))
 rows[-1]['actual_repeated_cases']=len(observed)
 stems={n.rsplit('/',1)[0] for n in patterns};siblings={n for n in allctest if n.rsplit('/',1)[0] in stems};label='affected-methods-all-backends';selection='|'.join(re.escape(n)+'/[^ ]+(  #.*)?$' for n in sorted(stems));required={n for n in siblings if '/unicorn' in n or '/whp' in n};run(label,['ctest','--test-dir',J,'--build-config','Release','--no-tests=error','-R',selection,'--parallel','4','--output-on-failure','--output-junit',E/(label+'.xml')],1200,dict(os.environ,NEVERD_THREADS='1'),expected=siblings,require_actual=required)
 for label,name in [('complete-cpu','NeverDCPUEmulationTests'),('complete-run-control','NeverDRunControlTests'),('complete-instruction-fetch','NeverDInstructionFetchTests'),('complete-source-dialect','NeverDSourceDialectTests')]:
  # Corpus inputs belong to the full workflow; the two explicit local-input
  # cases are kept out of this focused source-dialect execution.
  inv=listing((E/('inventory-'+name+'.stdout')).read_text(errors='replace'))
  excluded={'SourceDialect.CorpusReadsBackAsTheSameC','SourceDialect.OneFile'} if name=='NeverDSourceDialectTests' else set();expected=inv-excluded;filt='*'
  if excluded:filt='*-' + ':'.join(sorted(excluded))
  allowed_skips={n for n in expected if name=='NeverDCPUEmulationTests' and n.startswith('Checked/AArch64CPU.') and n.endswith('/1')}
  if name=='NeverDInstructionFetchTests':
   mandatory={n for n in expected if n.rsplit('/',1)[-1].startswith(('Unicorn','WhpX64'))}
   assert mandatory and all(any('.'+method+'/' in n for n in mandatory) for method in ['HostOperandWriteRefreshesAnInstructionAtThePageTail','HostWriteAfterStopRefreshesAnUnexecutedInstruction','RepeatedStepsRetainExactLoopAndStopState'])
   allowed_skips=expected-mandatory
  run(label,[J/'bin'/(name+'.exe'),'--gtest_filter='+filt,'--gtest_output=xml:'+str(E/(label+'.xml'))],600,expected=expected,require_actual=expected-allowed_skips)
 good=True
finally:
 products={str(p.relative_to(J)):hashlib.sha256(p.read_bytes()).hexdigest() for p in (J/'bin').glob('*.exe')};fixtures={p.name:hashlib.sha256(p.read_bytes()).hexdigest() for p in (J/'unittests/emulation/fixtures').glob('driver_context_capture*')}
 (E/'receipt.json').write_text(json.dumps({'accepted':good,'head':os.environ['GITHUB_SHA'],'llvm_mode':'pinned-prebuilt-r6','feature_candidate':'16a8a4d2c9c9235c051b9b019c9baace429015b9','feature_commit':'34a42e30fbe57a89a5ab9f8da22805810ee1d526','incoming_dev':'1e5268e1f51b35527aab8d39d5fe2c6fefab026a','original_timeout_cases':patterns,'products':products,'context_fixtures':fixtures,'runs':rows,'source_llvm_validation_separate':True},indent=2))
