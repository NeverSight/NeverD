#!/usr/bin/env python3
"""Verify array method formals retain fixed nominal results and inherited static lookup."""
from __future__ import annotations
import argparse,copy,hashlib,json,os,shutil,subprocess,tempfile,zipfile
from contextlib import nullcontext
from pathlib import Path
try:
 from scripts.mobile_real_apps_android import dex_header,parse_dexdump
 from scripts.test_mobile_android_annotation_values import compiler_inventory
 from scripts.test_mobile_android_internal import FIXTURES,Verify,choose_jdk,choose_d8
 from scripts.test_mobile_android_monitor_sparse_cleanup import compiler_facts
 from scripts.test_mobile_android_empty_original_bridge_throws import hashes,exception_inventory
except ModuleNotFoundError as error:
 if error.name!="scripts":raise
 from mobile_real_apps_android import dex_header,parse_dexdump
 from test_mobile_android_annotation_values import compiler_inventory
 from test_mobile_android_internal import FIXTURES,Verify,choose_jdk,choose_d8
 from test_mobile_android_monitor_sparse_cleanup import compiler_facts
 from test_mobile_android_empty_original_bridge_throws import hashes,exception_inventory

def main():
 p=argparse.ArgumentParser(description=__doc__)
 for name in ("neverd","android-jar","java-home","compiler-java-home"):p.add_argument("--"+name,type=Path,required=True)
 for name in ("d8","work-dir","kotlin-lib","kotlin-java-home"):p.add_argument("--"+name,type=Path)
 p.add_argument("--dependency-provider",action="store_true");p.add_argument("--shadow-owner",action="store_true");p.add_argument("--projected-result-header",action="store_true");args=p.parse_args()
 if args.projected_result_header and (not args.kotlin_lib or not args.kotlin_java_home):p.error("Projected result headers require --kotlin-lib and --kotlin-java-home")
 if args.work_dir:args.work_dir.mkdir(parents=True,exist_ok=False)
 context=nullcontext(args.work_dir) if args.work_dir else tempfile.TemporaryDirectory(prefix="neverd-array-nominal-result-")
 with context as directory:
  w=Path(directory).resolve();v=Verify(w,choose_jdk(args.java_home),choose_d8(args.d8),1200);cw=w/"compiler";cw.mkdir();compiler=Verify(cw,choose_jdk(args.compiler_java_home),choose_d8(args.d8),1200);fixture=FIXTURES/"array_nominal_result";originals=w/"originals";harness=w/"harness";name="ArrayNominalResultHarness";sha=lambda p:hashlib.sha256(p.read_bytes()).hexdigest()
  if args.shadow_owner:
   authored=w/"owned-fixture";shutil.copytree(fixture/"java",authored/"java");fixture=authored
   owner=fixture/"java/fixture/api/Backend.java";text=owner.read_text()
   if text.count("class Backend {")!=1:raise RuntimeError("Owned static scope anchor changed")
   owner.write_text(text.replace("class Backend {","class Backend<T extends Number> {"))
  sdk=args.android_jar.resolve();sdk_hash=sha(sdk);runtime_deps=[];dependency_hashes={};decl=None
  java_sources=sorted((fixture/"java").rglob("*.java"))
  if args.projected_result_header:
   k=args.kotlin_lib.resolve();runtime_deps=[k/"kotlin-stdlib.jar",k/"annotations-13.0.jar"];dependency_hashes={str(f):sha(f) for f in sorted(k.glob("*.jar"))};decl=w/"kotlin-declarations.jar";seen=set()
   with zipfile.ZipFile(decl,"x") as dest:
    for jar in runtime_deps:
     with zipfile.ZipFile(jar) as source:
      for entry in source.infolist():
       if entry.filename.endswith(".class") and not entry.filename.startswith("META-INF/"):
        if entry.filename in seen or ".." in Path(entry.filename).parts or entry.filename.startswith("/"):raise RuntimeError("Unsafe or duplicate Kotlin declaration entry")
        seen.add(entry.filename);dest.writestr(entry.filename,source.read(entry))
   dependency_hashes[str(decl)]=sha(decl);kotlin_cp=os.pathsep.join(map(str,runtime_deps))
   compiler.run([choose_jdk(args.kotlin_java_home)/"bin/java","-cp",k/"*","org.jetbrains.kotlin.cli.jvm.K2JVMCompiler","-no-stdlib","-no-reflect","-jvm-target","1.8","-Xno-param-assertions","-Xno-call-assertions","-classpath",kotlin_cp,"-d",originals,*sorted((FIXTURES/"array_nominal_result/kotlin").glob("*.kt"))],"kotlinc-original")
   java_originals=w/"original-java";compiler.compile_local([f for f in java_sources if f.name!="Receipt.java"],java_originals,classpath=os.pathsep.join(map(str,[originals,*runtime_deps])),release=8)
   for f in java_originals.rglob("*.class"):
    target=originals/f.relative_to(java_originals);target.parent.mkdir(parents=True,exist_ok=True);shutil.copy2(f,target)
  else:compiler.compile_local(java_sources,originals,release=8)
  observer_sources=[FIXTURES/"harness"/(name+".java")]
  if args.projected_result_header:observer_sources.append(FIXTURES/"harness/ArrayNominalPredicateCaller.java")
  harness.mkdir();compiler.run([compiler.javac,"--release","8","-cp",os.pathsep.join(map(str,[originals,*runtime_deps])),"-d",harness,*observer_sources],"javac-observer")
  original_hashes,harness_hashes=hashes(originals),hashes(harness);(w/"frozen-input-hashes.json").open("x").write(json.dumps({"originals":original_hashes,"harness":harness_hashes},indent=2)+"\n");all_classes,all_methods=compiler_inventory(v,originals)
  if (len(all_classes),len(all_methods))!=((7,16) if args.projected_result_header else (6,13)):raise RuntimeError("Owned array-result definition inventory changed")
  facts=compiler_facts(originals);exceptions=exception_inventory(originals);excluded=all_classes-{"Lfixture/Client;"} if args.dependency_provider else set();classes=all_classes-excluded;methods={m for m in all_methods if m.split("->",1)[0] in classes};provider=w/"owned-provider.jar"
  with zipfile.ZipFile(provider,"x") as jar:
   for f in sorted(originals.rglob("*.class")):
    if f.relative_to(originals).as_posix()!="fixture/Client.class":jar.writestr(f.relative_to(originals).as_posix(),f.read_bytes())
  provider_hash=sha(provider)
  def execute(runtime,label):
   out=[]
   for route in range(3):
    for branch in range(2):
     for mode in range(18):
      text=v.run([v.java,"-cp",os.pathsep.join(map(str,runtime)),name,str(route),str(branch),str(mode),str(args.projected_result_header).lower()],f"{label}-{route}-{branch}-{mode}")
      if text!="attempts=2,array-nominal=matched\n":raise RuntimeError("Frozen array-result observer changed")
      out.append(text)
   if len(out)!=108:raise RuntimeError("Array-result case inventory changed")
   return out
  baseline=execute((originals,harness,*runtime_deps),"baseline");(w/"baseline.json").open("x").write(json.dumps(baseline,indent=2)+"\n")
  client=(fixture/"java/fixture/Client.java").read_text();backend=(fixture/"java/fixture/api/Backend.java").read_text()
  call="Facade.collect(values)";selection="Trace.hit(3, values);\n        Receipt chosen = Trace.result;"
  if client.count(call)!=2 or backend.count(selection)!=1:raise RuntimeError("Array-result mutation anchor changed")
  def helper(source,member):
   if not source.endswith("}\n"):raise RuntimeError("Array-result source tail changed")
   return source[:-2]+member+"\n}\n"
  wrong=[("wrong-overload","Client.java",client.replace(call,"Facade.collect((Object)values)"),0,0),
   ("skipped-provider","Client.java",client.replace(call,"Trace.result"),0,0),
   ("cloned-input","Client.java",client.replace(call,"Facade.collect(values==null?null:values.clone())"),0,0),
   ("wrong-result","Client.java",client.replace("return answer;","return Trace.alternate;"),0,0),
   ("wrapped-failure","Client.java",helper(client.replace(call,"safe(values)"),"private static Receipt safe(Object[] values){try{return Facade.collect(values);}catch(Throwable failure){throw new IllegalStateException(failure);}}"),0,6),
   ("double-provider","Client.java",client.replace("Receipt answer = "+call+";",call+"; Receipt answer = "+call+";"),0,0),
   ("early-caller-effect","Client.java",client.replace("Receipt answer = ","Trace.after = Trace.result; Receipt answer = "),0,0),
   ("lost-declaring-initializer","Backend.java",backend.replace("static { Trace.hit(1, null); }",""),0,0),
   ("lost-array-store","Backend.java",backend.replace("if (Trace.write) values[0] = (T)Trace.replacement;",""),0,4),
   ("early-result-read","Backend.java",backend.replace(selection,"Receipt chosen = Trace.result;\n        Trace.hit(3, values);"),0,14),
   ("repeated-result-read","Backend.java",backend.replace("return chosen;","return Trace.result;"),0,15),
   ("forced-facade-initializer","Client.java",helper(client.replace("Receipt answer = ","warm(); Receipt answer = "),"private static void warm(){try{Class.forName(\"fixture.api.Facade\");}catch(Throwable failure){throw new IllegalStateException(failure);}}"),0,0)]
  receipt_source=(FIXTURES/"array_nominal_result/java/fixture/Receipt.java").read_text()
  if args.projected_result_header:
   receipt_source=receipt_source.replace("class Receipt {","class Receipt implements Api {")
   for label,body in [("wrong-predicate-result","return false;"),
     ("lost-predicate-null-check","return value != null && ((Character)value).charValue() == stamp;"),
     ("lost-predicate-cast","return !(value instanceof Integer) && ((Character)value).charValue() == stamp;"),
     ("wrapped-predicate-failure","try { return ((Character)value).charValue() == stamp; } catch(Throwable failure) { throw new IllegalStateException(failure); }")]:
    wrong.append((label,"Receipt.java",helper(receipt_source,"public boolean contains(char value) { return value == stamp; } public boolean contains(Comparable value) { "+body+" }"),0,0))
  for label,filename,replacement,branch,mode in wrong:
   if replacement=={"Client.java":client,"Backend.java":backend,"Receipt.java":receipt_source}[filename]:raise RuntimeError("Array-result counterexample did not change the program")
   source=w/label/filename;source.parent.mkdir();source.open("x").write(replacement);compiled=w/(label+"-classes");compiler.compile_local([source],compiled,classpath=os.pathsep.join(map(str,[originals,*runtime_deps])),release=8)
   result=subprocess.run([v.java,"-cp",os.pathsep.join(map(str,(compiled,originals,harness,*runtime_deps))),name,"0",str(branch),str(mode),str(args.projected_result_header).lower()],text=True,capture_output=True,timeout=1200,env=v.environment);(w/(label+"-counterexample.log")).open("x").write(result.stdout+result.stderr)
   if result.returncode==0 or "owned nominal array result" not in result.stderr:raise RuntimeError("Observer missed wrong array-result program: "+label)
  cp=["--classpath",sdk]
  if decl:cp += ["--classpath",decl]
  if excluded:cp += ["--classpath",provider]
  dexdir=w/"dex";dexdir.mkdir();paths=[f for f in sorted(originals.rglob("*.class")) if "L"+f.relative_to(originals).as_posix()[:-6]+";" in classes];v.run([*v.d8,"--release","--min-api","24",*cp,"--output",dexdir,*paths],"d8-original");dex=dexdir/"classes.dex";frozen_dex=sha(dex);(w/"baseline-cli-sha256.txt").open("x").write(sha(args.neverd)+"\n");dump=v.run([v.dexdump,"-f","-h","-l","plain",dex],"sdk-inventory");inventory=parse_dexdump(dump,dex.name,dex_header(dex.read_bytes()))
  if {r["descriptor"] for r in inventory["classes"]}!=classes or {r["identity"] for r in inventory["methods"]}!=methods:raise RuntimeError("D8 changed an owned array-result definition")
  (w/"sdk-inventory.json").open("x").write(json.dumps(inventory,indent=2)+"\n");output=w/"recovered";v.run([args.neverd.resolve(),"mobile",dex,*cp,"--timeout","600","-o",output],"recover");coverage=json.loads((output/"report.json").read_text())["android_method_recovery"];rows={r["identity"] for r in coverage["methods"]}
  projected=args.projected_result_header and not excluded;bridge="Lfixture/Receipt;->contains(Ljava/lang/Comparable;)Z";projection=coverage.get("generic_signature_projections",[])
  if rows!=methods or coverage["class_count"]!=len(classes) or coverage["method_count"]!=len(methods) or coverage["unrecovered_method_count"] or coverage.get("projected_method_count",0)!=int(projected) or coverage.get("minimum_java_release",8)!=8:raise RuntimeError("Array-result coverage, scope or original signatures changed")
  if projected:
   if len(projection)!=1 or projection[0].get("declaration")!="Lfixture/Receipt;" or projection[0].get("original_signature")!=facts["Lfixture/Receipt;"]["signature"] or projection[0].get("changes")!=[{"kind":"checked-comparable-predicate-raw-interface","offset":18,"original_type":facts["Lfixture/Receipt;"]["signature"],"scope_origin":bridge,"source_type":"Lfixture/Api;"}]:raise RuntimeError("Nominal result lost its original interface projection receipt")
   projected_rows=[m for m in coverage["methods"] if m["status"]=="source-projected"]
   if len(projected_rows)!=1 or projected_rows[0]["identity"]!=bridge or projected_rows[0].get("projection_kind")!="checked-comparable-predicate-bridge" or projected_rows[0].get("bridge_target")!="Lfixture/Receipt;->contains(C)Z" or projected_rows[0].get("unbox_method")!="Ljava/lang/Character;->charValue()C":raise RuntimeError("Nominal result changed its checked predicate bridge")
  elif projection:raise RuntimeError("Unexpected array-result source projection")
  helpers={r["class"]+"->"+r["name"]+r["prototype"] for r in coverage.get("generated_source_helpers",[])};allowed={o+"->__neverdThrow(Ljava/lang/Throwable;)Ljava/lang/RuntimeException;" for o in classes};rebuilt=w/"rebuilt";deps=[*runtime_deps,*([provider] if excluded else [])];v.compile_local(sorted((output/"sources").rglob("*.java")),rebuilt,classpath=os.pathsep.join(map(str,deps)) if deps else None,release=8);actual_classes,actual_methods=compiler_inventory(v,rebuilt)
  if actual_classes!=classes or set(actual_methods)!=methods|helpers or not helpers<=allowed:raise RuntimeError("Array-result source invented or omitted a definition")
  after=compiler_facts(rebuilt);after_exceptions=exception_inventory(rebuilt);versions={};dropped={};debug={};flags={}
  for owner in classes:
   wanted=copy.deepcopy(facts[owner]);actual=copy.deepcopy(after[owner]);actual["methods"]={m:r for m,r in actual["methods"].items() if m not in helpers}
   if args.projected_result_header:
    for identity,row in [(owner,wanted),*wanted["fields"].items(),*wanted["methods"].items()]:
     for key in ("runtime_invisible_annotations","runtime_invisible_parameter_annotations"):
      value=row.get(key,[]);uses=value if key=="runtime_invisible_annotations" else [a for slot in value for a in slot]
      if uses:
       if any(a["type"] not in {"Lorg/jetbrains/annotations/Nullable;","Lorg/jetbrains/annotations/NotNull;","Lkotlin/jvm/JvmField;"} or a["elements"] for a in uses):raise RuntimeError("Unexpected array-result CLASS annotation")
       dropped.setdefault(identity,{})[key]=value;row[key]=[]
    filename=owner.rsplit("/",1)[1][:-1]+".java";original_file=filename[:-5]+".kt" if owner in {"Lfixture/Api;","Lfixture/Receipt;"} else filename
    if wanted.get("source_file")!=original_file or actual.get("source_file")!=filename:raise RuntimeError("Unexpected array-result source filename")
    debug[owner]={"original":original_file,"rebuilt":filename};wanted["source_file"]=filename
   if projected and owner=="Lfixture/Receipt;":
    if wanted["methods"][bridge]["access"]!=0x1041 or actual["methods"][bridge]["access"]!=1 or actual["signature"] is not None:raise RuntimeError("Unexpected nominal predicate source metadata")
    flags[bridge]={"original":0x1041,"rebuilt":1};wanted["methods"][bridge]["access"]=1;wanted["signature"]=None
   if (wanted["major"],wanted["minor"],actual["major"],actual["minor"])!=(52,0,52,0):raise RuntimeError("Unexpected array-result compiler class version")
   versions[owner]={"original_major":52,"rebuilt_major":52,"minor":0};wanted["major"]=52
   if actual!=wanted:raise RuntimeError("Array-result metadata changed beyond reported helpers: "+owner)
   if any(after_exceptions[m]!=exceptions[m] for m in facts[owner]["methods"]):raise RuntimeError("Array-result Exceptions changed")
  (w/"compiler-class-versions.json").open("x").write(json.dumps(versions,indent=2)+"\n")
  (w/"d8-annotation-differences.json").open("x").write(json.dumps(dropped,indent=2)+"\n");(w/"source-file-differences.json").open("x").write(json.dumps(debug,indent=2)+"\n");(w/"predicate-bridge-flag-differences.json").open("x").write(json.dumps(flags,indent=2)+"\n")
  if execute((rebuilt,harness,*deps),"rebuilt")!=baseline:raise RuntimeError("Array identity, member lookup, result timing or exceptions changed")
  if hashes(originals)!=original_hashes or hashes(harness)!=harness_hashes or sha(dex)!=frozen_dex or sha(provider)!=provider_hash or sha(sdk)!=sdk_hash or any(sha(Path(f))!=h for f,h in dependency_hashes.items()):raise RuntimeError("Frozen array-result input changed")
  summary={"status":"verified","class_count":len(classes),"method_count":len(methods),"all_java_units":len(list((output/"sources").rglob("*.java"))),"minimum_java_release":8,"fresh_jvm_cases":108,"attempts_per_case":2,"wrong_programs":len(wrong),"dependency_provider":args.dependency_provider,"shadow_owner":args.shadow_owner,"projected_result_header":args.projected_result_header};(w/"summary.json").open("x").write(json.dumps(summary,indent=2)+"\n");print(json.dumps(summary,sort_keys=True))
 return 0
if __name__=="__main__":raise SystemExit(main())
