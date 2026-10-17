from pathlib import Path
import hashlib,json,os,re,signal,subprocess,time,xml.etree.ElementTree as ET
J=Path.cwd()/'build-linux-boundaries';E=J/'evidence';rows=[];good=False

def run(label,args,bound=600):
 args=list(map(str,args));start=time.monotonic();p=subprocess.Popen(args,stdout=subprocess.PIPE,stderr=subprocess.PIPE,start_new_session=True);expired=False
 try:out,err=p.communicate(timeout=bound)
 except subprocess.TimeoutExpired:
  expired=True;os.killpg(p.pid,signal.SIGTERM)
  try:out,err=p.communicate(timeout=3)
  except subprocess.TimeoutExpired:os.killpg(p.pid,signal.SIGKILL);out,err=p.communicate(timeout=10)
 (E/(label+'.stdout')).write_bytes(out);(E/(label+'.stderr')).write_bytes(err);row={'label':label,'argv':args,'status':p.returncode,'outer_timeout':expired,'root_reaped':p.poll() is not None,'seconds':time.monotonic()-start};rows.append(row);print(json.dumps(row),flush=True);return row,out.decode(errors='replace')

def listing(text):
 suite='';names=set()
 for line in text.splitlines():
  if line and not line[0].isspace() and line.split(' #')[0].rstrip().endswith('.'):
   suite=line.split(' #')[0].strip()
  elif line.startswith('  ') and suite:
   item=line.strip().split(' #')[0].strip()
   if item:names.add(suite+item)
 return names
try:
 for binary in ['NeverDX87CallGraphTests','NeverDARM32ObjectImmediateTests']:
  row,text=run('inventory-'+binary,[J/'bin'/binary,'--gtest_list_tests'],60);assert row['status']==0 and not row['outer_timeout'];expected=listing(text);assert expected
  label='native-'+binary;row,text=run(label,[J/'bin'/binary,'--gtest_output=xml:'+str(E/(label+'.xml'))],900)
  cases=list(ET.parse(E/(label+'.xml')).getroot().iter('testcase'));actual={x.attrib['classname']+'.'+x.attrib['name'] for x in cases};passed={x.attrib['classname']+'.'+x.attrib['name'] for x in cases if x.find('failure') is None and x.find('skipped') is None and x.attrib.get('status')=='run' and x.attrib.get('result')=='completed'};row.update(expected=sorted(expected),observed=sorted(actual),actual_passed=sorted(passed));assert row['status']==0 and not row['outer_timeout'] and len(cases)==len(expected) and actual==expected and passed==expected
 good=True
finally:
 products={p.name:hashlib.sha256(p.read_bytes()).hexdigest() for p in [J/'bin/NeverDX87CallGraphTests',J/'bin/NeverDARM32ObjectImmediateTests',J/'bin/neverd',J/'unittests/lift/test_x87_fpu32.o'] if p.is_file()};(E/'receipt.json').write_text(json.dumps({'accepted':good,'head':os.environ['GITHUB_SHA'],'feature_candidate':'49e7c2582','llvm_mode':'prebuilt-r6; source LLVM qualification remains separate','products':products,'observations':rows},indent=2))
