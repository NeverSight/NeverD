#!/usr/bin/env python3
"""Verify conditional interface boolean initialization preserves ordered runtime effects."""
from __future__ import annotations
import argparse,copy,hashlib,json,os,re,subprocess,tempfile,zipfile
from contextlib import nullcontext
from pathlib import Path
try:
 from scripts.mobile_real_apps_android import dex_header,parse_dexdump
 from scripts.test_mobile_android_annotation_values import compiler_inventory
 from scripts.test_mobile_android_internal import FIXTURES,Verify,choose_jdk,choose_d8
 from scripts.test_mobile_android_visibility_generic_bridge import compiler_facts
 from scripts.test_mobile_android_empty_original_bridge_throws import hashes,exception_inventory
except ModuleNotFoundError as error:
 if error.name!="scripts":raise
 from mobile_real_apps_android import dex_header,parse_dexdump
 from test_mobile_android_annotation_values import compiler_inventory
 from test_mobile_android_internal import FIXTURES,Verify,choose_jdk,choose_d8
 from test_mobile_android_visibility_generic_bridge import compiler_facts
 from test_mobile_android_empty_original_bridge_throws import hashes,exception_inventory

def main():
 p=argparse.ArgumentParser(description=__doc__)
 for name in ("neverd","android-jar","java-home","compiler-java-home"):p.add_argument("--"+name,type=Path,required=True)
 for name in ("d8","work-dir"):p.add_argument("--"+name,type=Path)
 p.add_argument("--dependency-provider",action="store_true");args=p.parse_args()
 if args.work_dir:args.work_dir.mkdir(parents=True,exist_ok=False)
 context=nullcontext(args.work_dir) if args.work_dir else tempfile.TemporaryDirectory(prefix="neverd-interface-boolean-initializer-")
 with context as directory:
  w=Path(directory).resolve();v=Verify(w,choose_jdk(args.java_home),choose_d8(args.d8),1200);cw=w/"compiler";cw.mkdir();compiler=Verify(cw,choose_jdk(args.compiler_java_home),choose_d8(args.d8),1200);fixture=FIXTURES/"interface_boolean_initializer";originals=w/"originals";harness=w/"harness";name="fixture.InterfaceBooleanInitializerHarness";sha=lambda p:hashlib.sha256(p.read_bytes()).hexdigest()
  sdk=args.android_jar.resolve();sdk_hash=sha(sdk);compiler.compile_local(sorted((fixture/"java").rglob("*.java")),originals,release=8)
  compiler.compile_local([FIXTURES/"harness"/(n+".java") for n in ("InterfaceBooleanInitializerCaller","InterfaceBooleanInitializerHarness")],harness,classpath=originals,release=8)
  original_hashes,harness_hashes=hashes(originals),hashes(harness);(w/"frozen-input-hashes.json").open("x").write(json.dumps({"originals":original_hashes,"harness":harness_hashes},indent=2)+"\n");all_classes,all_methods=compiler_inventory(v,originals)
  if (len(all_classes),len(all_methods))!=(3,7):raise RuntimeError("Owned interface-boolean-initializer definition inventory changed")
  facts=compiler_facts(originals);exceptions=exception_inventory(originals);excluded=all_classes-{"Lfixture/Policy;"} if args.dependency_provider else set();classes=all_classes-excluded;methods={m for m in all_methods if m.split("->",1)[0] in classes};provider=w/"owned-provider.jar"
  with zipfile.ZipFile(provider,"x") as jar:
   for owner in sorted(all_classes-{"Lfixture/Policy;"}):jar.writestr(owner[1:-1]+".class",(originals/(owner[1:-1]+".class")).read_bytes())
  provider_hash=sha(provider)
  def execute(runtime,label):
   out=[]
   for route in range(4):
    for mode in range(10):
     text=v.run([v.java,"-cp",os.pathsep.join(map(str,runtime)),name,str(route),str(mode)],f"{label}-{route}-{mode}")
     expected=f"route={route},mode={mode},attempt=[01],trace=[0-9]+"
     if len(text.splitlines())!=2 or not all(re.fullmatch(expected,line) for line in text.splitlines()):raise RuntimeError("Frozen interface-boolean-initializer observer changed")
     out.append(text)
   return out
  baseline=execute((originals,harness),"baseline");(w/"baseline.json").open("x").write(json.dumps(baseline,indent=2)+"\n")
  texts={f.name:f.read_text() for f in sorted((fixture/"java").rglob("*.java"))};call="Source.left()>=Source.right()"
  wrong=[
   ("constant-false","Policy.java",call,"false",0),
   ("constant-true","Policy.java",call,"true",5),
   ("reversed-relation","Policy.java",call,"Source.left()<Source.right()",0),
   ("strict-relation","Policy.java",call,"Source.left()>Source.right()",6),
   ("lost-right-call","Policy.java",call,"Source.left()>=27",0),
   ("repeated-left","Policy.java",call,"Source.left()+Source.left()>=Source.right()",0),
   ("repeated-right","Policy.java",call,"Source.left()>=Source.right()+Source.right()",0),
   ("reordered-producers","Policy.java",call,"Source.right()<=Source.left()",0),
   ("late-field-read","Policy.java",call,"(Source.left()>=Source.right())&&(Trace.left>=Trace.right)",0),
   ("overflow-comparison","Policy.java",call,"Source.left()-Source.right()>=0",8),
   ("wrong-default-observation","Source.java","Trace.defaultSeen=Policy.enabled;","Trace.defaultSeen=true;",0),
   ("wrapped-failure","Trace.java","throw failure;","throw new IllegalStateException(failure);",2),
   ("lost-right-mutation","Source.java","Trace.left=Trace.late;",";",0),
   ("lost-left-effect","Source.java","Trace.hit(2);",";",0),
   ("lost-right-effect","Source.java","Trace.hit(3);",";",0),
   ("lost-class-initialization","Source.java","static { Trace.hit(1); }","static { }",0)]
  for label,filename,anchor,replacement,mode in wrong:
   text=texts[filename]
   if text.count(anchor)!=1:raise RuntimeError("Interface boolean mutation anchor changed: "+label)
   source=w/label/filename;source.parent.mkdir();source.open("x").write(text.replace(anchor,replacement));compiled=w/(label+"-classes");compiler.compile_local([source],compiled,classpath=originals,release=8)
   result=subprocess.run([v.java,"-cp",os.pathsep.join(map(str,(compiled,originals,harness))),name,"0",str(mode)],text=True,capture_output=True,timeout=1200,env=v.environment);(w/(label+"-counterexample.log")).open("x").write(result.stdout+result.stderr)
   if result.returncode==0 or "owned interface boolean initializer mismatch" not in result.stderr:raise RuntimeError("Observer missed wrong interface boolean program: "+label)
  cp=["--classpath",sdk]
  if excluded:cp+=["--classpath",provider]
  dexdir=w/"dex";dexdir.mkdir();paths=[f for f in sorted(originals.rglob("*.class")) if "L"+f.relative_to(originals).as_posix()[:-6]+";" in classes];v.run([*v.d8,"--release","--min-api","24",*cp,"--output",dexdir,*paths],"d8-original");dex=dexdir/"classes.dex";frozen_dex=sha(dex);(w/"baseline-cli-sha256.txt").open("x").write(sha(args.neverd)+"\n");dump=v.run([v.dexdump,"-f","-h","-l","plain",dex],"sdk-inventory");inventory=parse_dexdump(dump,dex.name,dex_header(dex.read_bytes()))
  if {r["descriptor"] for r in inventory["classes"]}!=classes or {r["identity"] for r in inventory["methods"]}!=methods:raise RuntimeError("D8 changed an owned interface-boolean-initializer definition")
  (w/"sdk-inventory.json").open("x").write(json.dumps(inventory,indent=2)+"\n");disassembly=v.run([v.dexdump,"-d",dex],"initializer-disassembly");witnessed=False
  for block in re.finditer(r"(?ms)^\s+#\d+\s+:\s+\(in ([^)]+)\)\s*\n(.*?)(?=^\s+#\d+\s+:\s+\(in |^Class #|\Z)",disassembly):
   if block[1]!="Lfixture/Policy;":continue
   ops=re.findall(r"\|[0-9a-fA-F]+:\s+(\S+)([^\n]*)",block[2]);names=[op for op,unused in ops]
   if len(ops)==10 and names[:4]==["invoke-static","move-result","invoke-static","move-result"] and names[4].startswith("if-") and names[5:]==["const/4","goto","const/4","sput-boolean","return-void"]:witnessed=True
  if not witnessed:raise RuntimeError("D8 changed the owned boolean branch/store witness")
  output=w/"recovered";v.run([args.neverd.resolve(),"mobile",dex,*cp,"--timeout","600","-o",output],"recover");coverage=json.loads((output/"report.json").read_text())["android_method_recovery"];rows={r["identity"] for r in coverage["methods"]}
  if rows!=methods or coverage["class_count"]!=len(classes) or coverage["method_count"]!=len(methods) or coverage["unrecovered_method_count"] or coverage.get("projected_method_count",0) or coverage.get("generic_signature_projections") or coverage.get("class_source_bindings") or coverage.get("minimum_java_release",8)!=8:raise RuntimeError("Interface boolean coverage or original identities changed")
  helpers={r["class"]+"->"+r["name"]+r["prototype"] for r in coverage.get("generated_source_helpers",[])};allowed={o+"->__neverdThrow(Ljava/lang/Throwable;)Ljava/lang/RuntimeException;" for o in classes};rebuilt=w/"rebuilt";deps=[provider] if excluded else [];v.compile_local(sorted((output/"sources").rglob("*.java")),rebuilt,classpath=os.pathsep.join(map(str,deps)) if deps else None,release=8);actual_classes,actual_methods=compiler_inventory(v,rebuilt)
  if actual_classes!=classes or set(actual_methods)!=methods|helpers or not helpers<=allowed:raise RuntimeError("Interface boolean source invented or omitted a definition")
  after=compiler_facts(rebuilt);after_exceptions=exception_inventory(rebuilt)
  for owner in classes:
   wanted=copy.deepcopy(facts[owner]);actual=copy.deepcopy(after[owner]);actual["methods"]={m:r for m,r in actual["methods"].items() if m not in helpers}
   if actual!=wanted:raise RuntimeError("Interface boolean compiler metadata or ConstantValue changed: "+owner)
   if any(after_exceptions[m]!=exceptions[m] for m in facts[owner]["methods"]):raise RuntimeError("Interface boolean Exceptions changed")
  if execute((rebuilt,harness,*deps),"rebuilt")!=baseline:raise RuntimeError("Interface boolean evaluation order, snapshots, initialization or failure identity changed")
  if hashes(originals)!=original_hashes or hashes(harness)!=harness_hashes or sha(dex)!=frozen_dex or sha(provider)!=provider_hash or sha(sdk)!=sdk_hash:raise RuntimeError("Frozen interface-boolean-initializer input changed")
  summary={"status":"verified","class_count":len(classes),"method_count":len(methods),"all_java_units":len(list((output/"sources").rglob("*.java"))),"minimum_java_release":8,"fresh_jvm_cases":40,"attempts_per_case":2,"wrong_programs":len(wrong),"dependency_provider":args.dependency_provider};(w/"summary.json").open("x").write(json.dumps(summary,indent=2)+"\n");print(json.dumps(summary,sort_keys=True))
 return 0
if __name__=="__main__":raise SystemExit(main())
