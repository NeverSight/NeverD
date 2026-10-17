import concurrent.futures,hashlib,json,os,pathlib,re,subprocess,time,xml.etree.ElementTree as ET
R=pathlib.Path.cwd();J=R/'build-windows-runtime';E=J/'evidence';E.mkdir(exist_ok=True,parents=True);rows=[]
def run(label,args,timeout=180,env=None):
 start=time.monotonic();status=None
 try:
  p=subprocess.run(list(map(str,args)),capture_output=True,timeout=timeout,env=env);out,err,status=p.stdout,p.stderr,p.returncode
 except subprocess.TimeoutExpired as x:out,err=x.stdout or b'',x.stderr or b'';status='host-timeout'
 (E/(label+'.stdout')).write_bytes(out);(E/(label+'.stderr')).write_bytes(err)
 row={'label':label,'argv':list(map(str,args)),'status':status,'seconds':time.monotonic()-start};rows.append(row);print(json.dumps(row),flush=True)
 return row,out.decode(errors='replace')
patterns=['ExplicitBackends/X64StatusFlags.CarryInstructionsPreserveAllOtherFlagsAndRegisters/unicornPortable', 'ExplicitBackends/X64StatusFlags.SAHFRestoresOnlyDefinedStatusBits/unicornPortable', 'Backends/KernelFileObjectExecution.OriginalDriverQueriesBorrowedFileInformation/unicornLegacyPreferred', 'Backends/KernelFileObjectExecution.OriginalDriverQueriesBorrowedFileInformation/unicornLegacyRebased', 'Backends/KernelFileObjectExecution.OriginalDriverQueriesBufferedFileInformation/unicornLegacyPreferred', 'Backends/KernelFileObjectExecution.OriginalDriverQueriesBufferedFileInformation/unicornLegacyRebased', 'Backends/KernelFileObjectExecution.OriginalBufferedNativeFileTransfers/unicornLegacyPreferred', 'Backends/KernelFileObjectExecution.OriginalBufferedNativeFileTransfers/unicornLegacyRebased', 'Backends/KernelIntegerFormattingExecution.OriginalImportsAndLookupPreserveAllValueBits/unicornLegacyPreferred', 'Backends/KernelIntegerFormattingExecution.OriginalImportsAndLookupPreserveAllValueBits/unicornLegacyRebased', 'Backends/KernelCRTStringExecution.OriginalImportsAndLookupPreserveCheckedWrites/unicornLegacyPreferred', 'Backends/KernelCRTStringExecution.OriginalImportsAndLookupPreserveCheckedWrites/unicornLegacyRebased', 'Backends/KernelFileSystemExecution.OriginalDriverOpensExistingFiles/unicornLegacyPreferred', 'Backends/KernelFileSystemExecution.OriginalDriverOpensExistingFiles/unicornLegacyRebased', 'Native/DriverContextCapture.CompleteImportedAndDynamicRecordsAcrossLifecycle/whp', 'Native/DriverContextCapture.ConcreteCaptureRetainsRecoveryDependency/whp', 'Preemptive/DriverScheduling.BusyGuestMakesProgressWithoutCooperativeYield/unicorn_driver_PeriodicTimer', 'Preemptive/DriverSchedulingPriority.RuntimePriorityControlsDispatchAndPreemption/unicorn_driver_EqualRoundRobin']
try:
 for name in ['NeverDX64IntegerTests','NeverDNativeDriverTests','NeverDSourceDialectTests']:
  run('inventory-'+name,[J/'bin'/(name+'.exe'),'--gtest_list_tests'])
 run('rust-baseline',[J/'bin/NeverDSourceDialectTests.exe','--gtest_filter=SourceDialect.RustViewComputesWhatCComputes:SourceDialect.BitFieldsOfTheProgramsTypesComputeWhatCComputes','--gtest_output=xml:'+str(E/'rust-baseline.xml')],300)
 selection='|'.join(re.escape(n)+'(?:  #.*)?$' for n in patterns)
 for threads in ['1',None]:
  env=dict(os.environ)
  if threads is None:env.pop('NEVERD_THREADS',None)
  else:env['NEVERD_THREADS']=threads
  for parallel in [1,4]:
   label='baseline-threads-'+str(threads)+'-parallel-'+str(parallel)
   run(label,['ctest','--test-dir',J,'--build-config','Release','--no-tests=error','-R',selection,'--parallel',str(parallel),'--output-on-failure','--output-junit',E/(label+'.xml'),'--output-log',E/(label+'.log')],1200,env)
 for i in range(2):
  run('isolated-repeat-'+str(i),['ctest','--test-dir',J,'--build-config','Release','--no-tests=error','-R',selection,'--parallel','1','--output-on-failure','--output-junit',E/('isolated-repeat-'+str(i)+'.xml')],1200,dict(os.environ,NEVERD_THREADS='1'))
finally:
 products={str(p.relative_to(J)):hashlib.sha256(p.read_bytes()).hexdigest() for p in (J/'bin').glob('*.exe')}
 (E/'receipt.json').write_text(json.dumps({'phase':'baseline diagnosis, failures preserved','head':os.environ['GITHUB_SHA'],'llvm_mode':'pinned-prebuilt-r6','source_baseline':'98147dee06715aa0edc27f27133b278bbe0dde7d','cases':patterns,'products':products,'runs':rows},indent=2))
