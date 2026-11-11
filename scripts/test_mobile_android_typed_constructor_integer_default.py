#!/usr/bin/env python3
"""Verify integer defaults retain complete same-owner constructor signatures and input identities."""
from __future__ import annotations
import argparse,copy,hashlib,json,os,re,shutil,subprocess,tempfile
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
 p.add_argument("--package-owner",action="store_true");args=p.parse_args()
 if args.work_dir:args.work_dir.mkdir(parents=True,exist_ok=False)
 context=nullcontext(args.work_dir) if args.work_dir else tempfile.TemporaryDirectory(prefix="neverd-typed-constructor-integer-default-")
 with context as directory:
  w=Path(directory).resolve();v=Verify(w,choose_jdk(args.java_home),choose_d8(args.d8),1200);cw=w/"compiler";cw.mkdir();compiler=Verify(cw,choose_jdk(args.compiler_java_home),choose_d8(args.d8),1200);fixture=FIXTURES/"typed_constructor_integer_default";originals=w/"originals";harness=w/"harness";name="fixture.TypedConstructorIntegerDefaultHarness";sha=lambda p:hashlib.sha256(p.read_bytes()).hexdigest()
  source_root=fixture/"java"
  if args.package_owner:
   source_root=w/"package-source";shutil.copytree(fixture/"java",source_root);source=source_root/"fixture/Base.java";text=source.read_text()
   if text.count("public class Base")!=1 or text.count("public Base(")!=2:raise RuntimeError("Owned package constructor declaration changed")
   source.write_text(text.replace("public class Base","class Base").replace("public Base(","Base("))
  sdk=args.android_jar.resolve();sdk_hash=sha(sdk);compiler.compile_local(sorted(source_root.rglob("*.java")),originals,release=8)
  compiler.compile_local([FIXTURES/"harness"/(n+".java") for n in ("TypedConstructorIntegerDefaultCaller","TypedConstructorIntegerDefaultHarness")],harness,classpath=originals,release=8)
  original_hashes,harness_hashes=hashes(originals),hashes(harness);(w/"frozen-input-hashes.json").open("x").write(json.dumps({"originals":original_hashes,"harness":harness_hashes},indent=2)+"\n");classes,methods=compiler_inventory(v,originals);methods=set(methods)
  if (len(classes),len(methods))!=(2,5):raise RuntimeError("Owned typed-constructor-integer-default definition inventory changed")
  facts=compiler_facts(originals);owner="Lfixture/Base;";caller=owner+"-><init>(ILjava/lang/Class;I)V";target=owner+"-><init>(ILjava/lang/Class;II)V"
  if facts[owner]["methods"][caller]["signature"]!="(ILjava/lang/Class<TT;>;I)V" or facts[owner]["methods"][target]["signature"]!="(ILjava/lang/Class<TT;>;II)V":raise RuntimeError("Owned constructor signatures lost their complete class-scoped carrier")
  exceptions=exception_inventory(originals)
  def execute(runtime,label):
   out=[]
   for route in range(4):
    for mode in range(12):
     text=v.run([v.java,"-cp",os.pathsep.join(map(str,runtime)),name,str(route),str(mode)],f"{label}-{route}-{mode}")
     expected=f"route={route},mode={mode},attempt=[01],trace=[0-9]+"
     if len(text.splitlines())!=2 or not all(re.fullmatch(expected,line) for line in text.splitlines()):raise RuntimeError("Frozen typed-constructor-integer-default observer changed")
     out.append(text)
   return out
  baseline=execute((originals,harness),"baseline");(w/"baseline.json").open("x").write(json.dumps(baseline,indent=2)+"\n")
  texts={f.name:f.read_text() for f in sorted(source_root.rglob("*.java"))};call="this(left,token,0,right);"
  wrong=[
   ("wrong-default","Base.java",call,"this(left,token,1,right);",0),
   ("wrong-left","Base.java",call,"this(0,token,0,right);",0),
   ("wrong-right","Base.java",call,"this(left,token,0,0);",0),
   ("wrong-token","Base.java",call,"this(left,null,0,right);",0),
   ("lost-constructor-suffix","Base.java","Trace.hit(3);",";",0),
   ("repeated-constructor-suffix","Base.java","Trace.hit(3);","Trace.hit(3);Trace.hit(3);",0),
   ("lost-target-effect","Base.java","Trace.hit(2);",";",0),
   ("lost-direct-padding","Base.java","this.padding=padding;","this.padding=0;",9),
   ("swapped-default-position","Base.java",call,"this(left,token,right,0);",0),
   ("wrong-token-store","Base.java","this.token=token;","this.token=(Class<T>)(Class)String.class;",6),
   ("wrapped-failure","Trace.java","throw failure;","throw new IllegalStateException(failure);",2),
   ("lost-partial-state","Base.java","Trace.parent=this;","Trace.parent=null;",2),
   ("new-null-token-check","Base.java","this.left=left;","if(token==null)throw new NullPointerException();this.left=left;",5),
   ("new-element-check","Base.java","this.left=left;","if((Object)token!=String.class)throw new ClassCastException();this.left=left;",6),
   ("early-target-failure","Base.java","this.left=left;","Trace.hit(2);this.left=left;",2),
   ("late-parent-registration","Base.java","Trace.parent=this;\n        Trace.hit(2);","Trace.hit(2);\n        Trace.parent=this;",2)]
  for label,filename,anchor,replacement,mode in wrong:
   text=texts[filename]
   if text.count(anchor)!=1:raise RuntimeError("Typed integer constructor mutation anchor changed: "+label)
   source=w/label/filename;source.parent.mkdir();source.open("x").write(text.replace(anchor,replacement));compiled=w/(label+"-classes");compiler.compile_local([source],compiled,classpath=originals,release=8)
   result=subprocess.run([v.java,"-cp",os.pathsep.join(map(str,(compiled,originals,harness))),name,"0",str(mode)],text=True,capture_output=True,timeout=1200,env=v.environment);(w/(label+"-counterexample.log")).open("x").write(result.stdout+result.stderr)
   if result.returncode==0 or "owned typed constructor integer default mismatch" not in result.stderr:raise RuntimeError("Observer missed wrong typed integer constructor program: "+label)
  cp=["--classpath",sdk];dexdir=w/"dex";dexdir.mkdir();v.run([*v.d8,"--release","--min-api","24",*cp,"--output",dexdir,*sorted(originals.rglob("*.class"))],"d8-original");dex=dexdir/"classes.dex";frozen_dex=sha(dex);(w/"baseline-cli-sha256.txt").open("x").write(sha(args.neverd)+"\n");dump=v.run([v.dexdump,"-f","-h","-l","plain",dex],"sdk-inventory");inventory=parse_dexdump(dump,dex.name,dex_header(dex.read_bytes()))
  if {r["descriptor"] for r in inventory["classes"]}!=classes or {r["identity"] for r in inventory["methods"]}!=methods:raise RuntimeError("D8 changed an owned typed-constructor-integer-default definition")
  (w/"sdk-inventory.json").open("x").write(json.dumps(inventory,indent=2)+"\n");disassembly=v.run([v.dexdump,"-d",dex],"constructor-disassembly");witnessed=False
  for block in re.finditer(r"(?ms)^\s+#\d+\s+:\s+\(in ([^)]+)\)\s*\n(.*?)(?=^\s+#\d+\s+:\s+\(in |^Class #|\Z)",disassembly):
   if block[1]!=owner:continue
   proto=re.search(r"(?m)^\s+type\s+: '(.*?)'",block[2])
   if not proto or proto[1]!="(ILjava/lang/Class;I)V":continue
   ops=re.findall(r"\|[0-9a-fA-F]+:\s+(\S+)([^\n]*)",block[2])
   if len(ops)==5 and ops[0][0]=="const/4" and "#int 0" in ops[0][1] and ops[1][0]=="invoke-direct" and "Lfixture/Base;.<init>:(ILjava/lang/Class;II)V" in ops[1][1]:witnessed=True
  if not witnessed:raise RuntimeError("D8 changed the exact typed constructor integer-default delegation")
  output=w/"recovered";v.run([args.neverd.resolve(),"mobile",dex,*cp,"--timeout","600","-o",output],"recover");coverage=json.loads((output/"report.json").read_text())["android_method_recovery"];rows={r["identity"] for r in coverage["methods"]}
  if rows!=methods or coverage["class_count"]!=len(classes) or coverage["method_count"]!=len(methods) or coverage["unrecovered_method_count"] or coverage.get("projected_method_count",0) or coverage.get("generic_signature_projections") or coverage.get("class_source_bindings") or coverage.get("minimum_java_release",8)!=8:raise RuntimeError("Typed integer constructor coverage or original signatures changed")
  helpers={r["class"]+"->"+r["name"]+r["prototype"] for r in coverage.get("generated_source_helpers",[])};allowed={o+"->__neverdThrow(Ljava/lang/Throwable;)Ljava/lang/RuntimeException;" for o in classes};rebuilt=w/"rebuilt";v.compile_local(sorted((output/"sources").rglob("*.java")),rebuilt,release=8);actual_classes,actual_methods=compiler_inventory(v,rebuilt)
  if actual_classes!=classes or set(actual_methods)!=methods|helpers or not helpers<=allowed:raise RuntimeError("Typed integer constructor source invented or omitted a definition")
  after=compiler_facts(rebuilt);after_exceptions=exception_inventory(rebuilt)
  for cls in classes:
   wanted=copy.deepcopy(facts[cls]);actual=copy.deepcopy(after[cls]);actual["methods"]={m:r for m,r in actual["methods"].items() if m not in helpers}
   if actual!=wanted:raise RuntimeError("Typed integer constructor compiler metadata changed: "+cls)
   if any(after_exceptions[m]!=exceptions[m] for m in facts[cls]["methods"]):raise RuntimeError("Typed integer constructor Exceptions changed")
  if execute((rebuilt,harness),"rebuilt")!=baseline:raise RuntimeError("Typed integer constructor input identities, delegation, target behavior or failures changed")
  if hashes(originals)!=original_hashes or hashes(harness)!=harness_hashes or sha(dex)!=frozen_dex or sha(sdk)!=sdk_hash:raise RuntimeError("Frozen typed-constructor-integer-default input changed")
  summary={"status":"verified","class_count":len(classes),"method_count":len(methods),"all_java_units":len(list((output/"sources").rglob("*.java"))),"minimum_java_release":8,"fresh_jvm_cases":48,"attempts_per_case":2,"wrong_programs":len(wrong),"package_owner":args.package_owner};(w/"summary.json").open("x").write(json.dumps(summary,indent=2)+"\n");print(json.dumps(summary,sort_keys=True))
 return 0
if __name__=="__main__":raise SystemExit(main())
