#!/usr/bin/env python3
"""Verify literal superclass bridges retain exact float results and casts."""
from __future__ import annotations
import argparse,copy,hashlib,json,os,subprocess,tempfile,zipfile
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
 context=nullcontext(args.work_dir) if args.work_dir else tempfile.TemporaryDirectory(prefix="neverd-primitive-parameter-bridge-")
 with context as directory:
  w=Path(directory).resolve();v=Verify(w,choose_jdk(args.java_home),choose_d8(args.d8),1200);cw=w/"compiler";cw.mkdir();compiler=Verify(cw,choose_jdk(args.compiler_java_home),choose_d8(args.d8),1200);fixture=FIXTURES/"primitive_parameter_bridge";originals=w/"originals";harness=w/"harness";name="fixture.PrimitiveParameterBridgeHarness";sha=lambda p:hashlib.sha256(p.read_bytes()).hexdigest()
  sdk=args.android_jar.resolve();sdk_hash=sha(sdk);source_root=fixture/"java"
  compiler.compile_local(sorted(source_root.rglob("*.java")),originals,release=8)
  harness.mkdir();compiler.run([compiler.javac,"--release","8","-cp",originals,"-d",harness,FIXTURES/"harness/PrimitiveParameterBridgeHarness.java"],"javac-observer")
  frozen={"originals":hashes(originals),"harness":hashes(harness)};(w/"frozen-input-hashes.json").open("x").write(json.dumps(frozen,indent=2)+"\n");all_classes,all_methods=compiler_inventory(v,originals)
  if (len(all_classes),len(all_methods))!=(7,19):raise RuntimeError("Owned primitive parameter bridge inventory changed")
  facts=compiler_facts(originals);exceptions=exception_inventory(originals)
  if facts["Lfixture/Host$1;"]["signature"] or facts["Lfixture/Middle;"]["signature"]!="Lfixture/Base<Lfixture/Value;>;":raise RuntimeError("Owned literal superclass declaration changed")
  expected={"Lfixture/Base;->get(Ljava/lang/Object;)F":"(TT;)F","Lfixture/Base;->put(Ljava/lang/Object;F)V":"(TT;F)V"}
  for method,signature in expected.items():
   if facts["Lfixture/Base;"]["methods"][method]["signature"]!=signature:raise RuntimeError("Owned complete primitive contract changed")
  excluded={"Lfixture/Base;","Lfixture/Middle;","Lfixture/Value;"} if args.dependency_provider else set();classes=all_classes-excluded;methods={m for m in all_methods if m.split("->",1)[0] in classes};provider=w/"owned-provider.jar"
  with zipfile.ZipFile(provider,"x") as jar:
   for owner in ("Lfixture/Base;","Lfixture/Middle;","Lfixture/Value;"):jar.writestr(owner[1:-1]+".class",(originals/(owner[1:-1]+".class")).read_bytes())
  provider_hash=sha(provider)
  def execute(runtime,label):
   out=[]
   for route in range(4):
    for mode in range(12):
     text=v.run([v.java,"-cp",os.pathsep.join(map(str,runtime)),name,str(route),str(mode)],f"{label}-{route}-{mode}")
     if text!="attempts=2,primitive-parameter-bridge=matched\n":raise RuntimeError("Owned primitive parameter bridge observer changed")
     out.append(text)
   return out
  baseline=execute((originals,harness),"baseline");(w/"baseline.json").open("x").write(json.dumps(baseline,indent=2)+"\n")
  texts={f.name:f.read_text() for f in sorted(source_root.rglob("*.java"))}
  call="float result=((Base)receiver).get(value);"
  wrong=[
   ("lost-call","Core.java",call,"float result=0.0f;",0,0),
   ("repeated-call","Core.java",call,"((Base)receiver).get(value);"+call,0,0),
   ("wrong-input","Core.java",call,call.replace("get(value)","get(new Value())"),0,0),
   ("wrapped-failure","Core.java",call,"float result;try{result=((Base)receiver).get(value);}catch(Throwable failure){throw new IllegalArgumentException(failure);}",0,5),
   ("early-cast","Core.java","Trace.hit(0,value,0.0f);","Value early=(Value)value;Trace.hit(0,value,0.0f);",0,4),
   ("lost-get-effect","Host.java","Trace.hit(1,value,0.0f);",";",0,0),
   ("lost-put-effect","Host.java","Trace.hit(2,value,number);",";",2,0),
   ("lost-store","Host.java","value.number=number;",";",2,0),
   ("wrong-store","Host.java","value.number=number;","value.number=number+1.0f;",2,0),
   ("late-result-read","Host.java","return saved;","return value.number;",0,0),
   ("rounded-result","Host.java","return saved;","return (float)(int)saved;",0,0),
   ("canonical-nan","Host.java","return saved;","return Float.intBitsToFloat(Float.floatToIntBits(saved));",0,8),
   ("lost-signed-zero","Host.java","return saved;","return saved+0.0f;",0,9),
   ("wrong-put-argument","Core.java","((Base)receiver).put(value,number);","((Base)receiver).put(value,0.0f);",2,0),
   ("repeat-put","Core.java","((Base)receiver).put(value,number);","((Base)receiver).put(value,number);((Base)receiver).put(value,number);",2,0),
   ("lost-result-effect","Core.java","Trace.hit(4,value,result);",";",0,0)]
  for label,filename,anchor,replacement,route,mode in wrong:
   text=texts[filename]
   if not text.count(anchor):raise RuntimeError("Owned mutation anchor changed: "+label)
   source=w/label/filename;source.parent.mkdir();source.open("x").write(text.replace(anchor,replacement));compiled=w/(label+"-classes");compiler.compile_local([source],compiled,classpath=originals,release=8)
   result=subprocess.run([v.java,"-cp",os.pathsep.join(map(str,(compiled,originals,harness))),name,str(route),str(mode)],text=True,capture_output=True,timeout=1200,env=v.environment);(w/(label+"-counterexample.log")).open("x").write(result.stdout+result.stderr)
   if result.returncode==0 or "owned primitive parameter bridge mismatch" not in result.stderr:raise RuntimeError("Observer missed wrong primitive parameter bridge program: "+label)
  cp=["--classpath",sdk]
  if excluded:cp+=["--classpath",provider]
  dexdir=w/"dex";dexdir.mkdir();paths=[f for f in sorted(originals.rglob("*.class")) if "L"+f.relative_to(originals).as_posix()[:-6]+";" in classes];v.run([*v.d8,"--release","--min-api","24",*cp,"--output",dexdir,*paths],"d8-original");dex=dexdir/"classes.dex";frozen_dex=sha(dex);(w/"baseline-cli-sha256.txt").open("x").write(sha(args.neverd)+"\n");dump=v.run([v.dexdump,"-f","-h","-l","plain",dex],"sdk-inventory");inventory=parse_dexdump(dump,dex.name,dex_header(dex.read_bytes()))
  if {r["descriptor"] for r in inventory["classes"]}!=classes or {r["identity"] for r in inventory["methods"]}!=methods:raise RuntimeError("D8 changed an owned primitive parameter bridge definition")
  (w/"sdk-inventory.json").open("x").write(json.dumps(inventory,indent=2)+"\n");output=w/"recovered";v.run([args.neverd.resolve(),"mobile",dex,*cp,"--timeout","600","-o",output],"recover");coverage=json.loads((output/"report.json").read_text())["android_method_recovery"];rows={r["identity"] for r in coverage["methods"]}
  if rows!=methods or coverage["class_count"]!=len(classes) or coverage["method_count"]!=len(methods) or coverage["unrecovered_method_count"] or coverage.get("generic_signature_projections") or coverage.get("minimum_java_release",8)!=8:raise RuntimeError("Owned primitive bridge original coverage changed")
  owner="Lfixture/Host$1;";bindings=coverage.get("class_source_bindings",[])
  if len(bindings)!=1 or bindings[0]["class"]!=owner or bindings[0]["binding_kind"]!="anonymous-to-named-member" or bindings[0]["binary_name_status"]!="changed" or not bindings[0].get("metadata_differences"):raise RuntimeError("Owned primitive bridge lacks its exact reported source owner")
  binding=bindings[0];renamed=binding["source_enclosing_class"][:-1]+"$"+binding["source_name"]+";"
  affected={m for m in methods if m.startswith(owner+"->")}|{"Lfixture/Host;-><clinit>()V"}
  projected={r["identity"] for r in coverage["methods"] if r["status"]=="source-projected"}
  if projected!=affected or coverage.get("projected_method_count")!=len(affected):raise RuntimeError("Unexpected projected primitive bridge entry")
  by_id={r["identity"]:r for r in coverage["methods"]}
  for member in ("get","put"):
   suffix="(Ljava/lang/Object;)F" if member=="get" else "(Ljava/lang/Object;F)V"
   typed="(Lfixture/Value;)F" if member=="get" else "(Lfixture/Value;F)V"
   row=by_id[owner+"->"+member+suffix]
   if row.get("bridge_target")!=owner+"->"+member+typed or row.get("bridge_dispatch")!="Lfixture/Base;->"+member+suffix or row.get("projection_kind")!="generic-superclass-bridge" or not row.get("reason"):raise RuntimeError("Primitive bridge lost its complete original dispatch receipt")
  def rename(value):
   if isinstance(value,str):return value.replace(owner,renamed)
   if isinstance(value,dict):return {rename(k):rename(v) for k,v in value.items()}
   if isinstance(value,list):return [rename(v) for v in value]
   return value
  helpers={rename(r["class"]+"->"+r["name"]+r["prototype"]) for r in coverage.get("generated_source_helpers",[])};allowed={rename(o)+"->__neverdThrow(Ljava/lang/Throwable;)Ljava/lang/RuntimeException;" for o in classes};rebuilt=w/"rebuilt";deps=[provider] if excluded else [];v.compile_local(sorted((output/"sources").rglob("*.java")),rebuilt,classpath=os.pathsep.join(map(str,deps)) if deps else None,release=8);actual_classes,actual_methods=compiler_inventory(v,rebuilt)
  if actual_classes!={rename(o) for o in classes} or set(actual_methods)!={rename(m) for m in methods}|helpers or not helpers<=allowed:raise RuntimeError("Owned primitive bridge source invented or omitted a definition")
  after=compiler_facts(rebuilt);after_exceptions=exception_inventory(rebuilt)
  for cls in classes:
   wanted=rename(copy.deepcopy(facts[cls]));actual=copy.deepcopy(after[rename(cls)]);actual["methods"]={m:r for m,r in actual["methods"].items() if m not in helpers}
   if cls==owner:
    wanted["inner_class"]["name"]=binding["source_name"];wanted["inner_class"]["outer"]=binding["source_enclosing_class"];wanted["inner_class"]["access"]|=8;wanted["enclosing_method"]=None
   if actual!=wanted:raise RuntimeError("Owned primitive bridge compiler metadata changed beyond its reported member binding: "+cls)
   if any(after_exceptions[rename(m)]!=exceptions[m] for m in facts[cls]["methods"]):raise RuntimeError("Owned primitive bridge Exceptions changed")
  if execute((rebuilt,harness,*deps),"rebuilt")!=baseline:raise RuntimeError("Owned primitive parameter bridge effects or results changed")
  if hashes(originals)!=frozen["originals"] or hashes(harness)!=frozen["harness"] or sha(dex)!=frozen_dex or sha(provider)!=provider_hash or sha(sdk)!=sdk_hash:raise RuntimeError("Frozen primitive parameter bridge input changed")
  summary={"status":"verified","class_count":len(classes),"method_count":len(methods),"all_java_units":len(list((output/"sources").rglob("*.java"))),"minimum_java_release":8,"fresh_jvm_cases":48,"attempts_per_case":2,"wrong_programs":len(wrong),"reported_anonymous_bindings":1,"compiler_bridges_verified":2,"dependency_provider":args.dependency_provider};(w/"summary.json").open("x").write(json.dumps(summary,indent=2)+"\n");print(json.dumps(summary,sort_keys=True))
 return 0
if __name__=="__main__":raise SystemExit(main())
