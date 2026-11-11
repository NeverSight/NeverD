#!/usr/bin/env python3
"""Verify fresh closed callbacks retain constructor inputs and superclass overloads."""
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
 context=nullcontext(args.work_dir) if args.work_dir else tempfile.TemporaryDirectory(prefix="neverd-closed-callback-super-")
 with context as directory:
  w=Path(directory).resolve();v=Verify(w,choose_jdk(args.java_home),choose_d8(args.d8),1200);cw=w/"compiler";cw.mkdir();compiler=Verify(cw,choose_jdk(args.compiler_java_home),choose_d8(args.d8),1200);fixture=FIXTURES/"closed_callback_super";originals=w/"originals";harness=w/"harness";name="ClosedCallbackSuperHarness";sha=lambda p:hashlib.sha256(p.read_bytes()).hexdigest()
  sdk=args.android_jar.resolve();sdk_hash=sha(sdk);compiler.compile_local(sorted((fixture/"java").rglob("*.java")),originals,release=8)
  compiler.compile_local([FIXTURES/"harness"/(n+".java") for n in ("ClosedCallbackSuperCaller",name)],harness,classpath=originals,release=8)
  original_hashes,harness_hashes=hashes(originals),hashes(harness);(w/"frozen-input-hashes.json").open("x").write(json.dumps({"originals":original_hashes,"harness":harness_hashes},indent=2)+"\n");all_classes,all_methods=compiler_inventory(v,originals)
  if (len(all_classes),len(all_methods))!=(6,14):raise RuntimeError("Owned closed-callback-super definition inventory changed")
  facts=compiler_facts(originals);header=facts["Lfixture/Core$Callback;"]
  if header["signature"]!="Ljava/lang/Object;Ljava/util/function/Supplier<Ljava/lang/String;>;":raise RuntimeError("Owned complete callback interface changed")
  exceptions=exception_inventory(originals);excluded={"Lfixture/Base;"} if args.dependency_provider else set();classes=all_classes-excluded;methods={m for m in all_methods if m.split("->",1)[0] in classes};provider=w/"owned-provider.jar"
  with zipfile.ZipFile(provider,"x") as jar:jar.writestr("fixture/Base.class",(originals/"fixture/Base.class").read_bytes())
  provider_hash=sha(provider)
  def execute(runtime,label):
   out=[]
   for route in range(4):
    for mode in range(10):
     text=v.run([v.java,"-cp",os.pathsep.join(map(str,runtime)),name,str(route),str(mode)],f"{label}-{route}-{mode}")
     expected=f"route={route},mode={mode},attempt=[01],trace=[0-9]+"
     if len(text.splitlines())!=2 or not all(re.fullmatch(expected,line) for line in text.splitlines()):raise RuntimeError("Frozen closed-callback-super observer changed")
     out.append(text)
   return out
  baseline=execute((originals,harness),"baseline");(w/"baseline.json").open("x").write(json.dumps(baseline,indent=2)+"\n")
  texts={f.name:f.read_text() for f in sorted((fixture/"java").rglob("*.java"))}
  call="super(new Callback(context,text));"
  wrong=[
   ("lost-allocation","Core.java",call,"super((Supplier<String>)null);",0),
   ("repeat-allocation","Core.java",call,call+"new Callback(context,text);",0),
   ("wrong-context","Core.java",call,"super(new Callback(new Context(),text));",0),
   ("wrong-text","Core.java",call,"super(new Callback(context,null));",0),
   ("wrong-overload","Core.java",call,"super((Object)new Callback(context,text));",0),
   ("lost-factory-identity","Base.java","this.factory=factory;","this.factory=null;",0),
   ("lost-callback","Base.java","value=factory.get();","value=null;",0),
   ("late-snapshot","Core.java","snapshot=context.reply;","snapshot=Trace.late;",0),
   ("changed-marker","Core.java","marker=text;","marker=new StringBuilder();",0),
   ("wrapped-failure","Trace.java","throw failure;","throw new IllegalStateException(failure);",5),
   ("cloned-result","Core.java","return snapshot;","return new String(snapshot);",0),
   ("late-result-source","Core.java","return snapshot;","return context.reply;",0),
   ("lost-constructor-effect","Core.java","Trace.hit(4);",";",0),
   ("lost-parent-effect","Base.java","Trace.hit(5);",";",0),
   ("lost-callback-effect","Core.java","Trace.hit(6);",";",0),
   ("wrong-partial-state","Base.java","Trace.parent=this;","Trace.parent=null;",5)]
  for label,filename,anchor,replacement,mode in wrong:
   text=texts[filename]
   if text.count(anchor)!=1:raise RuntimeError("Closed callback mutation anchor changed: "+label)
   source=w/label/filename;source.parent.mkdir();source.open("x").write(text.replace(anchor,replacement));compiled=w/(label+"-classes");compiler.compile_local([source],compiled,classpath=originals,release=8)
   result=subprocess.run([v.java,"-cp",os.pathsep.join(map(str,(compiled,originals,harness))),name,"0",str(mode)],text=True,capture_output=True,timeout=1200,env=v.environment);(w/(label+"-counterexample.log")).open("x").write(result.stdout+result.stderr)
   if result.returncode==0 or "owned closed callback super mismatch" not in result.stderr:raise RuntimeError("Observer missed wrong closed callback program: "+label)
  cp=["--classpath",sdk]
  if excluded:cp+=["--classpath",provider]
  dexdir=w/"dex";dexdir.mkdir();paths=[f for f in sorted(originals.rglob("*.class")) if "L"+f.relative_to(originals).as_posix()[:-6]+";" in classes];v.run([*v.d8,"--release","--min-api","24",*cp,"--output",dexdir,*paths],"d8-original");dex=dexdir/"classes.dex";frozen_dex=sha(dex);(w/"baseline-cli-sha256.txt").open("x").write(sha(args.neverd)+"\n");dump=v.run([v.dexdump,"-f","-h","-l","plain",dex],"sdk-inventory");inventory=parse_dexdump(dump,dex.name,dex_header(dex.read_bytes()))
  if {r["descriptor"] for r in inventory["classes"]}!=classes or {r["identity"] for r in inventory["methods"]}!=methods:raise RuntimeError("D8 changed an owned closed-callback-super definition")
  (w/"sdk-inventory.json").open("x").write(json.dumps(inventory,indent=2)+"\n");disassembly=v.run([v.dexdump,"-d",dex],"constructor-disassembly");witnessed=False
  for block in re.finditer(r"(?ms)^\s+#\d+\s+:\s+\(in ([^)]+)\)\s*\n(.*?)(?=^\s+#\d+\s+:\s+\(in |^Class #|\Z)",disassembly):
   if block[1]!="Lfixture/Core$Child;":continue
   patterns=(r"new-instance[^\n]*Lfixture/Core\$Callback;",r"invoke-direct[^\n]*Lfixture/Core\$Callback;.<init>:\(Lfixture/Context;Ljava/lang/CharSequence;\)V",r"invoke-direct[^\n]*Lfixture/Base;.<init>:\(Ljava/util/function/Supplier;\)V")
   matches=[re.search(pattern,block[2]) for pattern in patterns]
   if all(matches) and [m.start() for m in matches]==sorted(m.start() for m in matches):witnessed=True
  if not witnessed:raise RuntimeError("D8 changed the owned allocation/initializer/super witness")
  output=w/"recovered";v.run([args.neverd.resolve(),"mobile",dex,*cp,"--timeout","600","-o",output],"recover");coverage=json.loads((output/"report.json").read_text())["android_method_recovery"];rows={r["identity"] for r in coverage["methods"]}
  if rows!=methods or coverage["class_count"]!=len(classes) or coverage["method_count"]!=len(methods) or coverage["unrecovered_method_count"] or coverage.get("projected_method_count",0)!=1 or coverage.get("generic_signature_projections") or coverage.get("minimum_java_release",8)!=25:raise RuntimeError("Closed callback coverage or original signatures changed")
  bridge="Lfixture/Core$Callback;->get()Ljava/lang/Object;";projected=[r for r in coverage["methods"] if r["status"]!="recovered"]
  if len(projected)!=1 or projected[0]["identity"]!=bridge or projected[0]["status"]!="source-projected" or projected[0].get("projection_kind")!="generic-interface-bridge" or projected[0].get("bridge_target")!="Lfixture/Core$Callback;->get()Ljava/lang/String;" or projected[0].get("bridge_dispatch")!="Ljava/util/function/Supplier;->get()Ljava/lang/Object;":raise RuntimeError("Unexpected projected closed callback entry")
  helpers={r["class"]+"->"+r["name"]+r["prototype"] for r in coverage.get("generated_source_helpers",[])};allowed={o+"->__neverdThrow(Ljava/lang/Throwable;)Ljava/lang/RuntimeException;" for o in classes};rebuilt=w/"rebuilt";deps=[provider] if excluded else [];v.compile_local(sorted((output/"sources").rglob("*.java")),rebuilt,classpath=os.pathsep.join(map(str,deps)) if deps else None,release=25);actual_classes,actual_methods=compiler_inventory(v,rebuilt)
  if actual_classes!=classes or set(actual_methods)!=methods|helpers or not helpers<=allowed:raise RuntimeError("Closed callback source invented or omitted a definition")
  after=compiler_facts(rebuilt);after_exceptions=exception_inventory(rebuilt)
  for owner in classes:
   wanted=copy.deepcopy(facts[owner]);actual=copy.deepcopy(after[owner]);actual["methods"]={m:r for m,r in actual["methods"].items() if m not in helpers}
   if wanted.get("major")!=52 or actual.get("major")!=69:raise RuntimeError("Unexpected original or required-release compiler version")
   actual["major"]=wanted["major"]
   if actual!=wanted:raise RuntimeError("Closed callback metadata changed beyond reported helpers and required Java release: "+owner)
   if any(after_exceptions[m]!=exceptions[m] for m in facts[owner]["methods"]):raise RuntimeError("Closed callback Exceptions changed")
  if execute((rebuilt,harness,*deps),"rebuilt")!=baseline:raise RuntimeError("Closed callback inputs, construction order, callback result or failures changed")
  if hashes(originals)!=original_hashes or hashes(harness)!=harness_hashes or sha(dex)!=frozen_dex or sha(provider)!=provider_hash or sha(sdk)!=sdk_hash:raise RuntimeError("Frozen closed-callback-super input changed")
  summary={"status":"verified","class_count":len(classes),"method_count":len(methods),"all_java_units":len(list((output/"sources").rglob("*.java"))),"minimum_java_release":25,"compiler_bridge_verified":1,"fresh_jvm_cases":40,"attempts_per_case":2,"wrong_programs":len(wrong),"dependency_provider":args.dependency_provider};(w/"summary.json").open("x").write(json.dumps(summary,indent=2)+"\n");print(json.dumps(summary,sort_keys=True))
 return 0
if __name__=="__main__":raise SystemExit(main())
