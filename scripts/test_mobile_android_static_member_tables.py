#!/usr/bin/env python3
"""Verify static-only ordinary members preserve non-static metadata and lazy effects."""
from __future__ import annotations
import argparse,copy,hashlib,json,os,shutil,struct,subprocess,tempfile,zipfile
from contextlib import nullcontext
from pathlib import Path
try:
 from scripts.mobile_real_apps_android import dex_header,parse_dexdump
 from scripts.test_mobile_android_annotation_values import compiler_inventory
 from scripts.test_mobile_android_internal import FIXTURES,Verify,choose_jdk,choose_d8,class_identity
 from scripts.test_mobile_android_shared_upper_bounds import class_records
 from scripts.test_mobile_android_static_defaults import method_code
 from scripts.test_mobile_android_empty_original_bridge_throws import hashes,exception_inventory
except ModuleNotFoundError as error:
 if error.name!="scripts":raise
 from mobile_real_apps_android import dex_header,parse_dexdump
 from test_mobile_android_annotation_values import compiler_inventory
 from test_mobile_android_internal import FIXTURES,Verify,choose_jdk,choose_d8,class_identity
 from test_mobile_android_shared_upper_bounds import class_records
 from test_mobile_android_static_defaults import method_code
 from test_mobile_android_empty_original_bridge_throws import hashes,exception_inventory

def encode_member(root):
 changed=removed=0
 for path in sorted(root.rglob("*.class")):
  before=path.read_bytes();shape=class_records(before);data=bytearray(before)
  for name,start,end in shape["rows"]:
   if name!=b"InnerClasses":continue
   count=int.from_bytes(before[start+6:start+8],"big")
   for at in range(start+8,start+8+8*count,8):
    inner,outer,label,flags=struct.unpack_from(">HHHH",before,at)
    if inner and shape["strings"][shape["classes"][inner]]==b"fixture/Host$Table":
     if flags!=0x19:raise RuntimeError("Owned static member flags changed")
     struct.pack_into(">H",data,at+6,flags&~8);changed+=1
  if path.name=="Host$Table.class":
   pos=shape["pool_end"]+6;pos+=2+2*int.from_bytes(before[pos:pos+2],"big")
   def end_member(at):
    n=int.from_bytes(before[at+6:at+8],"big");at+=8
    for _ in range(n):at+=6+int.from_bytes(before[at+2:at+6],"big")
    return at
   fields=int.from_bytes(before[pos:pos+2],"big");pos+=2
   for _ in range(fields):pos=end_member(pos)
   count_at=pos;count=int.from_bytes(before[pos:pos+2],"big");pos+=2;chunks=[]
   for _ in range(count):
    start=pos;flags,name,desc,attrs=struct.unpack_from(">HHHH",before,start);pos=end_member(pos)
    if shape["strings"][name]==b"<init>":
     if flags!=2 or shape["strings"][desc]!=b"()V":raise RuntimeError("Owned unused constructor changed")
     removed+=1
    else:chunks.append(bytes(data[start:pos]))
   data=data[:count_at]+(count-1).to_bytes(2,"big")+b"".join(chunks)+data[pos:]
   wanted=method_code(before);ctor=wanted.pop(b"<init>()V")
   if not ctor or method_code(bytes(data))!=wanted:raise RuntimeError("Owned member witness changed retained Code")
  elif method_code(before)!=method_code(bytes(data)):raise RuntimeError("Owned member metadata changed Code")
  path.write_bytes(data)
 if changed!=2 or removed!=1:raise RuntimeError("Owned static member witness inventory changed")
 return {"removed_unused_private_constructor":1,"cleared_static_inner_entries":2,"retained_Code_and_other_attributes":"unchanged"}

def main():
 p=argparse.ArgumentParser(description=__doc__)
 for name in ("neverd","android-jar","java-home"):p.add_argument("--"+name,type=Path,required=True)
 for name in ("d8","work-dir"):p.add_argument("--"+name,type=Path)
 p.add_argument("--sdk-provider",action="store_true");args=p.parse_args()
 if args.work_dir:args.work_dir.mkdir(parents=True,exist_ok=False)
 context=nullcontext(args.work_dir) if args.work_dir else tempfile.TemporaryDirectory(prefix="neverd-static-member-tables-")
 with context as directory:
  w=Path(directory).resolve();v=Verify(w,choose_jdk(args.java_home),choose_d8(args.d8),1200);fixture=FIXTURES/"static_member_tables";originals=w/"originals";encoded=w/"encoded";harness=w/"harness";sha=lambda p:hashlib.sha256(p.read_bytes()).hexdigest();sdk=args.android_jar.resolve();sdk_hash=sha(sdk)
  v.compile_local(sorted((fixture/"java").rglob("*.java")),originals,release=8);v.compile_local([FIXTURES/"harness/StaticMemberTablesHarness.java"],harness,classpath=originals,release=8)
  all_classes,original_methods=compiler_inventory(v,originals)
  if (len(all_classes),len(original_methods))!=(4,10):raise RuntimeError("Owned original member table inventory changed")
  frozen={"originals":hashes(originals),"harness":hashes(harness)};shutil.copytree(originals,encoded);witness=encode_member(encoded);(w/"encoding-witness.json").open("x").write(json.dumps(witness,indent=2)+"\n");frozen["encoded"]=hashes(encoded);facts=class_identity.compiler_classes(encoded);exceptions=exception_inventory(encoded);encoded_classes,all_methods=compiler_inventory(v,encoded)
  if encoded_classes!=all_classes or set(original_methods)-set(all_methods)!={"Lfixture/Host$Table;-><init>()V"}:raise RuntimeError("Owned member witness changed another definition")
  def execute(runtime,stage,label):
   out=[]
   for route in range(2):
    for mode in range(11):
     text=v.run([v.java,"-cp",os.pathsep.join(map(str,runtime)),"StaticMemberTablesHarness",str(route),str(mode),str(stage)],f"{label}-{route}-{mode}")
     if text!="attempts=2,static-member-table=matched\n":raise RuntimeError("Owned member table observer changed")
     out.append(text)
   return out
  baseline=execute((originals,harness),0,"original");(w/"baseline.json").open("x").write(json.dumps(baseline,indent=2)+"\n")
  if execute((encoded,harness),1,"encoded")!=baseline:raise RuntimeError("Owned member witness changed lazy initialization")
  texts={p.name:p.read_text() for p in (fixture/"java").rglob("*.java")}
  wrong=[("lost-call","Host.java","int value=Table.read(index);","int value=0;",0,0),
   ("repeated-call","Host.java","int value=Table.read(index);","Table.read(index);int value=Table.read(index);",0,0),
   ("lost-ignored-call","Host.java","Trace.hit(0,null);Table.read(index);Trace.hit(9,Table.mapping);","Trace.hit(0,null);Trace.hit(9,Table.mapping);",1,0),
   ("late-table-read","Host.java","mapping=result;","mapping=(int[])Seed.next;",0,0),
   ("early-value-read","Host.java","Trace.hit(4,mapping);int value=mapping[index];","int value=mapping[index];Trace.hit(4,mapping);",0,0),
   ("late-value-read","Host.java","Trace.hit(5,mapping);return value;","Trace.hit(5,mapping);return mapping[index];",0,0),
   ("lost-initializer-prefix","Host.java","Trace.hit(1,null);",";",0,0),
   ("wrapped-failure","Seed.java","if(fail)throw failure;","if(fail)throw new IllegalStateException(failure);",0,2)]
  for label,filename,anchor,replacement,route,mode in wrong:
   source=w/label;source.mkdir();inputs=[]
   for name,text in texts.items():
    if name==filename:
     if text.count(anchor)!=1:raise RuntimeError("Owned table mutation anchor changed: "+label)
     text=text.replace(anchor,replacement)
    p=source/name;p.open("x").write(text);inputs.append(p)
   compiled=w/(label+"-classes");v.compile_local(inputs,compiled,release=8);encode_member(compiled)
   result=subprocess.run([v.java,"-cp",os.pathsep.join(map(str,(compiled,harness))),"StaticMemberTablesHarness",str(route),str(mode),"1"],text=True,capture_output=True,env=v.environment,timeout=1200);(w/(label+"-counterexample.log")).open("x").write(result.stdout+result.stderr)
   if result.returncode==0 or "owned static member table mismatch" not in result.stderr:raise RuntimeError("Observer missed wrong member table program: "+label)
  excluded={"Lfixture/Seed;","Lfixture/Trace;"} if args.sdk_provider else set();classes=all_classes-excluded;methods={m for m in all_methods if m.split("->",1)[0] in classes};provider=w/"owned-provider.jar"
  with zipfile.ZipFile(provider,"x") as jar:
   for descriptor in sorted(excluded):
    path=descriptor[1:-1]+".class";jar.write(encoded/path,path)
  provider_hash=sha(provider);cp=["--classpath",sdk]
  if excluded:cp += ["--classpath",provider]
  dexdir=w/"dex";dexdir.mkdir();paths=[p for p in sorted(encoded.rglob("*.class")) if "L"+p.relative_to(encoded).as_posix()[:-6]+";" in classes];v.run([*v.d8,"--debug","--min-api","24",*cp,"--output",dexdir,*paths],"d8-original");dex=dexdir/"classes.dex";frozen_dex=sha(dex)
  inventory=parse_dexdump(v.run([v.dexdump,"-f","-h","-l","plain",dex],"sdk-inventory"),dex.name,dex_header(dex.read_bytes()))
  if {r["descriptor"] for r in inventory["classes"]}!=classes or {r["identity"] for r in inventory["methods"]}!=methods:raise RuntimeError("D8 changed an owned member table definition")
  (w/"sdk-inventory.json").open("x").write(json.dumps(inventory,indent=2)+"\n");(w/"frozen-input-hashes.json").open("x").write(json.dumps(frozen,indent=2)+"\n");(w/"baseline-cli-sha256.txt").open("x").write(sha(args.neverd)+"\n");output=w/"recovered";v.run([args.neverd.resolve(),"mobile",dex,*cp,"--timeout","600","-o",output],"recover");coverage=json.loads((output/"report.json").read_text())["android_method_recovery"]
  if {r["identity"] for r in coverage["methods"]}!=methods or coverage["class_count"]!=len(classes) or coverage["method_count"]!=len(methods) or coverage["unrecovered_method_count"] or coverage.get("projected_method_count",0) or coverage.get("generic_signature_projections") or coverage.get("minimum_java_release")!=16:raise RuntimeError("Owned member table coverage changed")
  bindings=coverage.get("class_source_bindings",[])
  if len(bindings)!=1 or bindings[0]["class"]!="Lfixture/Host$Table;" or bindings[0]["binding_kind"]!="static-only-inner-member" or bindings[0]["binary_name_status"]!="preserved" or len(bindings[0]["metadata_differences"])<2:raise RuntimeError("Missing explicit ordinary member source differences")
  helpers={r["class"]+"->"+r["name"]+r["prototype"] for r in coverage.get("generated_source_helpers",[])};expected={c+"->__neverdThrow(Ljava/lang/Throwable;)Ljava/lang/RuntimeException;" for c in classes}|{"Lfixture/Host$Table;-><init>(Lfixture/Host;)V"}
  if helpers!=expected:raise RuntimeError("Wrong original/compiler helper roles for member table")
  rebuilt=w/"rebuilt";deps=[provider] if excluded else [];v.compile_local(sorted((output/"sources").rglob("*.java")),rebuilt,classpath=os.pathsep.join(map(str,deps)) if deps else None,release=16);actual_classes,actual_methods=compiler_inventory(v,rebuilt)
  if actual_classes!=classes or set(actual_methods)!=methods|helpers:raise RuntimeError("Owned table source invented or lost definitions")
  after=class_identity.compiler_classes(rebuilt);after_exceptions=exception_inventory(rebuilt);outer_field=False
  for owner in classes:
   wanted=copy.deepcopy(facts[owner]);actual=copy.deepcopy(after[owner]);actual["methods"]={m:r for m,r in actual["methods"].items() if m not in helpers};actual.pop("sha256",None);wanted.pop("sha256",None)
   if actual.pop("major")!=60 or wanted.pop("major")!=52:raise RuntimeError("Owned member Java release changed")
   actual.pop("size");wanted.pop("size")
   extra=set(actual["fields"])-set(wanted["fields"])
   if extra:
    if owner!="Lfixture/Host$Table;" or extra!={"Lfixture/Host$Table;->this$0:Lfixture/Host;"}:raise RuntimeError("Unexpected compiler member field")
    field=actual["fields"].pop(next(iter(extra)))
    if field["access"]!=0x1010:raise RuntimeError("Wrong compiler outer field flags")
    outer_field=True
   if actual!=wanted:raise RuntimeError("Owned member table metadata changed: "+owner)
   if any(after_exceptions[m]!=exceptions[m] for m in facts[owner]["methods"]):raise RuntimeError("Owned member table Exceptions changed")
  if execute((rebuilt,harness,*deps),2,"rebuilt")!=baseline:raise RuntimeError("Owned table lazy initialization or effects changed")
  if hashes(originals)!=frozen["originals"] or hashes(encoded)!=frozen["encoded"] or hashes(harness)!=frozen["harness"] or sha(dex)!=frozen_dex or sha(provider)!=provider_hash or sha(sdk)!=sdk_hash:raise RuntimeError("Frozen member table inputs changed")
  summary={"status":"verified","class_count":len(classes),"method_count":len(methods),"all_java_units":len(list((output/"sources").rglob("*.java"))),"minimum_java_release":16,"fresh_jvm_cases":22,"attempts_per_case":2,"wrong_programs":len(wrong),"sdk_provider":args.sdk_provider,"compiler_outer_field":outer_field};(w/"summary.json").open("x").write(json.dumps(summary,indent=2)+"\n");print(json.dumps(summary,sort_keys=True))
 return 0
if __name__=="__main__":raise SystemExit(main())
