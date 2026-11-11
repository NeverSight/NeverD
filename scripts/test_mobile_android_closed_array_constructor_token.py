#!/usr/bin/env python3
"""Verify descriptor-only closed anonymous constructors forward erased carriers exactly."""
from __future__ import annotations
import copy,argparse,hashlib,json,os,re,subprocess,tempfile,zipfile
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
 context=nullcontext(args.work_dir) if args.work_dir else tempfile.TemporaryDirectory(prefix="neverd-closed-array-constructor-token-")
 with context as directory:
  w=Path(directory).resolve();v=Verify(w,choose_jdk(args.java_home),choose_d8(args.d8),1200);cw=w/"compiler";cw.mkdir();compiler=Verify(cw,choose_jdk(args.compiler_java_home),choose_d8(args.d8),1200);fixture=FIXTURES/"closed_array_constructor_token";originals=w/"originals";harness=w/"harness";name="ClosedArrayConstructorTokenHarness";sha=lambda p:hashlib.sha256(p.read_bytes()).hexdigest()
  sdk=args.android_jar.resolve();sdk_hash=sha(sdk);compiler.compile_local(sorted((fixture/"java").rglob("*.java")),originals,release=8)
  compiler.compile_local([FIXTURES/"harness"/(n+".java") for n in ("ClosedArrayConstructorTokenCaller",name)],harness,classpath=originals,release=8)
  original_hashes,harness_hashes=hashes(originals),hashes(harness);(w/"frozen-input-hashes.json").open("x").write(json.dumps({"originals":original_hashes,"harness":harness_hashes},indent=2)+"\n");all_classes,all_methods=compiler_inventory(v,originals)
  if (len(all_classes),len(all_methods))!=(4,7):raise RuntimeError("Owned closed-array-constructor-token definition inventory changed")
  owner="Lfixture/Core$1;";ctor=owner+"-><init>(ILjava/lang/Class;I)V";facts=compiler_facts(originals)
  if facts[owner]["signature"]!="Lfixture/Base<[F>;" or facts[owner]["fields"] or facts[owner]["methods"][ctor]["signature"] is not None:raise RuntimeError("Owned descriptor-only closed anonymous declaration changed")
  exceptions=exception_inventory(originals);excluded={"Lfixture/Base;"} if args.dependency_provider else set();classes=all_classes-excluded;methods={m for m in all_methods if m.split("->",1)[0] in classes};provider=w/"owned-provider.jar"
  with zipfile.ZipFile(provider,"x") as jar:jar.writestr("fixture/Base.class",(originals/"fixture/Base.class").read_bytes())
  provider_hash=sha(provider)
  def execute(runtime,label):
   out=[]
   for route in range(4):
    for mode in range(13):
     text=v.run([v.java,"-cp",os.pathsep.join(map(str,runtime)),name,str(route),str(mode)],f"{label}-{route}-{mode}")
     expected=f"route={route},mode={mode},attempt=[01],trace=[0-9]+"
     if len(text.splitlines())!=2 or not all(re.fullmatch(expected,line) for line in text.splitlines()):raise RuntimeError("Frozen closed-array-constructor-token observer changed")
     out.append(text)
   return out
  baseline=execute((originals,harness),"baseline");(w/"baseline.json").open("x").write(json.dumps(baseline,indent=2)+"\n")
  texts={f.name:f.read_text() for f in sorted((fixture/"java").rglob("*.java"))};call="new Base<float[]>(left,(Class<float[]>)token,right) {}"
  wrong=[
   ("wrong-left","Core.java",call,"new Base<float[]>(0,(Class<float[]>)token,right) {}",0),
   ("wrong-right","Core.java",call,"new Base<float[]>(left,(Class<float[]>)token,0) {}",0),
   ("swapped-scalars","Core.java",call,"new Base<float[]>(right,(Class<float[]>)token,left) {}",0),
   ("wrong-token","Core.java",call,"new Base<float[]>(left,(Class)Integer.class,right) {}",0),
   ("lost-token-store","Base.java","this.token=token;","this.token=null;",0),
   ("wrong-padding","Base.java","this.padding=0;","this.padding=1;",0),
   ("wrong-parent-left","Base.java","this.left=left;","this.left=0;",0),
   ("lost-parent-effect","Base.java","Trace.hit(2);",";",0),
   ("lost-constructor-suffix","Base.java","Trace.hit(3);",";",0),
   ("repeated-constructor-suffix","Base.java","Trace.hit(3);","Trace.hit(3);Trace.hit(3);",0),
   ("wrapped-failure","Trace.java","throw failure;","throw new IllegalStateException(failure);",2),
   ("lost-partial-state","Base.java","Trace.parent=this;","Trace.parent=null;",2),
   ("new-null-token-check","Core.java","Base<float[]> result=","if(token==null)throw new NullPointerException();Base<float[]> result=",5),
   ("new-element-check","Core.java","Base<float[]> result=","if(token!=float[].class)throw new ClassCastException();Base<float[]> result=",6),
   ("lost-caller-effect","Core.java","Trace.hit(4);",";",0),
   ("wrong-result-identity","Core.java","return result;","return null;",0)]
  for label,filename,anchor,replacement,mode in wrong:
   text=texts[filename]
   if text.count(anchor)!=1:raise RuntimeError("Closed local constructor mutation anchor changed: "+label)
   source=w/label/filename;source.parent.mkdir();source.open("x").write(text.replace(anchor,replacement));compiled=w/(label+"-classes");compiler.compile_local([source],compiled,classpath=originals,release=8)
   result=subprocess.run([v.java,"-cp",os.pathsep.join(map(str,(compiled,originals,harness))),name,"0",str(mode)],text=True,capture_output=True,timeout=1200,env=v.environment);(w/(label+"-counterexample.log")).open("x").write(result.stdout+result.stderr)
   if result.returncode==0 or "owned closed array constructor mismatch" not in result.stderr:raise RuntimeError("Observer missed wrong closed local constructor program: "+label)
  cp=["--classpath",sdk]
  if excluded:cp+=["--classpath",provider]
  dexdir=w/"dex";dexdir.mkdir();paths=[f for f in sorted(originals.rglob("*.class")) if "L"+f.relative_to(originals).as_posix()[:-6]+";" in classes];v.run([*v.d8,"--release","--min-api","24",*cp,"--output",dexdir,*paths],"d8-original");dex=dexdir/"classes.dex";frozen_dex=sha(dex);(w/"baseline-cli-sha256.txt").open("x").write(sha(args.neverd)+"\n");dump=v.run([v.dexdump,"-f","-h","-l","plain",dex],"sdk-inventory");inventory=parse_dexdump(dump,dex.name,dex_header(dex.read_bytes()))
  if {r["descriptor"] for r in inventory["classes"]}!=classes or {r["identity"] for r in inventory["methods"]}!=methods:raise RuntimeError("D8 changed an owned closed-array-constructor-token definition")
  (w/"sdk-inventory.json").open("x").write(json.dumps(inventory,indent=2)+"\n");disassembly=v.run([v.dexdump,"-d",dex],"constructor-disassembly");witnessed=False
  for block in re.finditer(r"(?ms)^\s+#\d+\s+:\s+\(in ([^)]+)\)\s*\n(.*?)(?=^\s+#\d+\s+:\s+\(in |^Class #|\Z)",disassembly):
   if block[1]!=owner:continue
   ops=re.findall(r"\|[0-9a-fA-F]+:\s+(\S+)([^\n]*)",block[2])
   if len(ops)==2 and ops[0][0]=="invoke-direct" and "Lfixture/Base;.<init>:(ILjava/lang/Class;I)V" in ops[0][1] and ops[1][0]=="return-void":witnessed=True
  if not witnessed:raise RuntimeError("D8 changed the exact anonymous constructor forwarder")
  output=w/"recovered";v.run([args.neverd.resolve(),"mobile",dex,*cp,"--timeout","600","-o",output],"recover");coverage=json.loads((output/"report.json").read_text())["android_method_recovery"];rows={r["identity"] for r in coverage["methods"]}
  if rows!=methods or coverage["class_count"]!=len(classes) or coverage["method_count"]!=len(methods) or coverage["unrecovered_method_count"] or coverage.get("projected_method_count",0)!=2 or coverage.get("generic_signature_projections") or coverage.get("minimum_java_release",8)!=8:raise RuntimeError("Closed local constructor coverage or original signatures changed")
  projected=[r for r in coverage["methods"] if r["status"]!="recovered"];bindings=coverage.get("class_source_bindings",[])
  affected={ctor,"Lfixture/Core;->make(ILjava/lang/Class;I)Lfixture/Base;"}
  if {r["identity"] for r in projected}!=affected or any(r.get("projection_kind")!="anonymous-to-named-local" or r["status"]!="source-projected" for r in projected) or len(bindings)!=1 or bindings[0]["class"]!=owner or bindings[0]["binding_kind"]!="anonymous-to-named-local" or bindings[0]["binary_name_status"]!="changed" or not bindings[0].get("metadata_differences"):raise RuntimeError("Unexpected closed anonymous source binding")
  rebuilt=w/"rebuilt";deps=[provider] if excluded else [];v.compile_local(sorted((output/"sources").rglob("*.java")),rebuilt,classpath=os.pathsep.join(map(str,deps)) if deps else None,release=8);after=compiler_facts(rebuilt);after_exceptions=exception_inventory(rebuilt)
  candidates=[n for n,r in after.items() if r["inner_class"] and r["inner_class"]["name"]==bindings[0]["source_name"] and r["enclosing_method"]==facts[owner]["enclosing_method"]]
  if len(candidates)!=1:raise RuntimeError("Rebuilt anonymous constructor lacks its unique reported binding")
  renamed=candidates[0]
  def rename(value):
   if isinstance(value,str):return value.replace(owner,renamed)
   if isinstance(value,dict):return {rename(k):rename(v) for k,v in value.items()}
   if isinstance(value,list):return [rename(v) for v in value]
   return value
  helpers={rename(r["class"]+"->"+r["name"]+r["prototype"]) for r in coverage.get("generated_source_helpers",[])};allowed={rename(o)+"->__neverdThrow(Ljava/lang/Throwable;)Ljava/lang/RuntimeException;" for o in classes};actual_classes,actual_methods=compiler_inventory(v,rebuilt)
  if actual_classes!={rename(o) for o in classes} or set(actual_methods)!={rename(m) for m in methods}|helpers or not helpers<=allowed:raise RuntimeError("Closed local source invented or omitted a definition")
  for cls in classes:
   wanted=rename(copy.deepcopy(facts[cls]));actual=copy.deepcopy(after[rename(cls)]);actual["methods"]={m:r for m,r in actual["methods"].items() if m not in helpers}
   if cls==owner:wanted["inner_class"]["name"]=bindings[0]["source_name"]
   if actual!=wanted:raise RuntimeError("Closed local metadata changed beyond its reported anonymous class name: "+cls)
   if any(after_exceptions[rename(m)]!=exceptions[m] for m in facts[cls]["methods"]):raise RuntimeError("Closed local Exceptions changed")
  if execute((rebuilt,harness,*deps),"rebuilt")!=baseline:raise RuntimeError("Closed local argument identities, constructor order or failures changed")
  if hashes(originals)!=original_hashes or hashes(harness)!=harness_hashes or sha(dex)!=frozen_dex or sha(provider)!=provider_hash or sha(sdk)!=sdk_hash:raise RuntimeError("Frozen closed-array-constructor-token input changed")
  summary={"status":"verified","class_count":len(classes),"method_count":len(methods),"all_java_units":len(list((output/"sources").rglob("*.java"))),"minimum_java_release":8,"reported_anonymous_bindings":1,"fresh_jvm_cases":52,"attempts_per_case":2,"wrong_programs":len(wrong),"dependency_provider":args.dependency_provider};(w/"summary.json").open("x").write(json.dumps(summary,indent=2)+"\n");print(json.dumps(summary,sort_keys=True))
 return 0
if __name__=="__main__":raise SystemExit(main())
