#!/usr/bin/env python3
"""Verify original instanceof prefixes retain constructor delegation and final stores."""
from __future__ import annotations
import argparse,copy,hashlib,json,os,re,shutil,subprocess,tempfile,zipfile
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

def linear_sources(work, fixture):
    source = work / "linear-source"
    shutil.copytree(fixture / "java", source)
    observer = work / "linear-observer/InstanceofConstructorPrefixHarness.java"
    observer.parent.mkdir()
    shutil.copyfile(FIXTURES / "harness/InstanceofConstructorPrefixHarness.java", observer)

    def replace(path, before, after):
        text = path.read_text()
        if text.count(before) != 1:
            raise RuntimeError("Linear constructor fixture anchor changed: " + path.name)
        path.write_text(text.replace(before, after))

    replace(source / "fixture/Host.java", "context instanceof Activity?(Activity)context:null",
            "Trace.prepare().select(context,context instanceof Activity)")
    replace(source / "fixture/Trace.java", "    public Trace() {}", """    public Trace() {}
    public static Trace prepare() {
        hit(9,hook,null,null,null,0);
        return hook;
    }
    public Activity select(Context context,boolean selected) {
        hit(10,this,context,null,null,selected?1:0);
        return selected?(Activity)context:null;
    }""")
    replace(observer, "static final class Observer extends Trace {", """static final class Observer extends Trace {
        @Override public Activity select(Context value,boolean flag) {
            Activity result=super.select(value,flag);
            return mode==9?activity:result;
        }""")
    replace(observer, "if(phase==8) {", """if(phase==9) {
                check(route==1&&owner==this&&a==null&&c==null&&h==null&&selected==0);
            } else if(phase==10) {
                check(route==1&&owner==this&&a==context&&c==null&&h==null&&selected==(context instanceof Activity?1:0));
            } else if(phase==8) {""")
    replace(observer, "events.add(phase);", "events.add(phase);\n            if(mode==10&&phase==9||mode==11&&phase==10)throw FAILURE;")
    replace(observer, "Trace.hook=new Observer();", "if(route==1&&mode==9)activity=new Activity();\n        Trace.hook=new Observer();")
    replace(observer, "if(route==2){wanted.add(6);", "if(route==1){wanted.add(9);wanted.add(10);}\n        if(route==2){wanted.add(6);")
    replace(observer, "mode==8?6:-1", "mode==8?6:mode==10?9:mode==11?10:-1")
    return source, observer


def main():
 p=argparse.ArgumentParser(description=__doc__)
 for name in ("neverd","android-jar","java-home","compiler-java-home"):p.add_argument("--"+name,type=Path,required=True)
 for name in ("d8","work-dir"):p.add_argument("--"+name,type=Path)
 p.add_argument("--sdk-provider",action="store_true");p.add_argument("--linear-prefix",action="store_true");args=p.parse_args()
 if args.work_dir:args.work_dir.mkdir(parents=True,exist_ok=False)
 context=nullcontext(args.work_dir) if args.work_dir else tempfile.TemporaryDirectory(prefix="neverd-instanceof-constructor-prefix-")
 with context as directory:
  w=Path(directory).resolve();v=Verify(w,choose_jdk(args.java_home),choose_d8(args.d8),1200);cw=w/"compiler";cw.mkdir();compiler=Verify(cw,choose_jdk(args.compiler_java_home),choose_d8(args.d8),1200);fixture=FIXTURES/"instanceof_constructor_prefix";originals=w/"originals";harness=w/"harness";name="fixture.InstanceofConstructorPrefixHarness";sha=lambda p:hashlib.sha256(p.read_bytes()).hexdigest()
  sdk=args.android_jar.resolve();sdk_hash=sha(sdk);source_root=fixture/"java";observer=FIXTURES/"harness/InstanceofConstructorPrefixHarness.java"
  if args.linear_prefix:source_root,observer=linear_sources(w,fixture)
  compiler.compile_local(sorted(source_root.rglob("*.java")),originals,release=8)
  harness.mkdir();compiler.run([compiler.javac,"--release","8","-cp",originals,"-d",harness,observer],"javac-observer")
  frozen={"originals":hashes(originals),"harness":hashes(harness)};(w/"frozen-input-hashes.json").open("x").write(json.dumps(frozen,indent=2)+"\n");all_classes,all_methods=compiler_inventory(v,originals)
  if (len(all_classes),len(all_methods))!=(7,16 if args.linear_prefix else 14):raise RuntimeError("Owned instanceof constructor prefix inventory changed")
  facts=compiler_facts(originals);exceptions=exception_inventory(originals)
  if facts["Lfixture/Host;"]["signature"]!="<H:Ljava/lang/Object;>Lfixture/Base;":raise RuntimeError("Owned original generic owner signature changed")
  excluded={"Lfixture/Base;","Lfixture/Context;","Lfixture/Activity;","Lfixture/Handler;"} if args.sdk_provider else set();classes=all_classes-excluded;methods={m for m in all_methods if m.split("->",1)[0] not in excluded};provider=w/"owned-provider.jar"
  with zipfile.ZipFile(provider,"x") as jar:
   for descriptor in sorted(excluded):
    path=descriptor[1:-1]+".class";jar.write(originals/path,path)
  provider_hash=sha(provider)
  def execute(runtime,label):
   out=[]
   modes=12 if args.linear_prefix else 10
   cases=[(route,variant,mode) for route in range(2) for variant in range(2) for mode in range(modes)]+[(2,1,mode) for mode in range(modes)]
   for route,variant,mode in cases:
    text=v.run([v.java,"-cp",os.pathsep.join(map(str,runtime)),name,str(route),str(variant),str(mode)],f"{label}-{route}-{variant}-{mode}")
    if text!="attempts=2,instanceof-constructor-prefix=matched\n":raise RuntimeError("Owned instanceof constructor prefix observer changed")
    out.append(text)
   return out
  baseline=execute((originals,harness),"baseline");(w/"baseline.json").open("x").write(json.dumps(baseline,indent=2)+"\n")
  texts={f.name:f.read_text() for f in sorted(source_root.rglob("*.java"))}
  branch="context instanceof Activity?(Activity)context:null"
  if args.linear_prefix:branch="Trace.prepare().select(context,context instanceof Activity)"
  delegation="this("+branch+",context,handler,code);"
  wrong=[
   ("lost-instanceof","Host.java",branch,"null",1,1,0),
   ("unsafe-cast","Host.java",branch,"(Activity)context",1,0,0),
   ("wrong-context-field","Host.java","this.context=context;","this.context=null;",0,0,0),
   ("wrong-handler-field","Host.java","this.handler=handler;","this.handler=null;",0,0,0),
   ("wrong-activity-field","Host.java","this.activity=activity;","this.activity=null;",0,0,0),
   ("wrong-code-field","Host.java","this.code=code;","this.code=0;",0,0,0),
   ("lost-context-check","Host.java","Trace.require(context,1);",";",0,0,0),
   ("lost-handler-check","Host.java","Trace.require(handler,2);",";",0,0,0),
   ("repeated-context-check","Host.java","Trace.require(context,1);","Trace.require(context,1);Trace.require(context,1);",0,0,0),
   ("lost-final-suffix","Host.java","Trace.hit(3,this,activity,context,handler,code);",";",0,0,0),
   ("repeated-final-suffix","Host.java","Trace.hit(3,this,activity,context,handler,code);","Trace.hit(3,this,activity,context,handler,code);Trace.hit(3,this,activity,context,handler,code);",0,0,0),
   ("wrong-context-input","Host.java",delegation,delegation.replace(",context,handler,code",",null,handler,code"),1,1,0),
   ("wrong-handler-input","Host.java",delegation,delegation.replace(",context,handler,code",",context,null,code"),1,1,0),
   ("lost-activity-check","Host.java","(Activity)Trace.require(activity,6)","activity",2,1,0),
   ("lost-handler-allocation","Host.java","new Handler()","null",2,1,0),
   ("wrapped-failure","Trace.java","if(current!=null)current.observe(phase,owner,activity,context,handler,code);","try{if(current!=null)current.observe(phase,owner,activity,context,handler,code);}catch(Throwable failure){throw new IllegalStateException(failure);}",0,0,3)]
  if args.linear_prefix:
   wrong += [
    ("inverted-type-test","Host.java","context instanceof Activity","!(context instanceof Activity)",1,1,0),
    ("replaced-prefix-receiver","Trace.java","return hook;","return new Trace();",1,1,9),
    ("lost-factory-effect","Trace.java","hit(9,hook,null,null,null,0);",";",1,1,0),
    ("lost-selection-effect","Trace.java","hit(10,this,context,null,null,selected?1:0);",";",1,1,0),
    ("repeated-prefix-factory","Host.java","Trace.prepare().select","Trace.prepare().prepare().select",1,1,0)]
  for label,filename,anchor,replacement,route,variant,mode in wrong:
   text=texts[filename]
   if not text.count(anchor):raise RuntimeError("Owned mutation anchor changed: "+label)
   source=w/label/filename;source.parent.mkdir();source.open("x").write(text.replace(anchor,replacement));compiled=w/(label+"-classes");compiler.compile_local([source],compiled,classpath=originals,release=8)
   result=subprocess.run([v.java,"-cp",os.pathsep.join(map(str,(compiled,originals,harness))),name,str(route),str(variant),str(mode)],text=True,capture_output=True,timeout=1200,env=v.environment);(w/(label+"-counterexample.log")).open("x").write(result.stdout+result.stderr)
   if result.returncode==0 or "owned instanceof constructor prefix mismatch" not in result.stderr:raise RuntimeError("Observer missed wrong instanceof constructor prefix program: "+label)
  cp=["--classpath",sdk]
  if excluded:cp+=["--classpath",provider]
  dexdir=w/"dex";dexdir.mkdir();paths=[f for f in sorted(originals.rglob("*.class")) if "L"+f.relative_to(originals).as_posix()[:-6]+";" in classes];v.run([*v.d8,"--release","--min-api","24",*cp,"--output",dexdir,*paths],"d8-original");dex=dexdir/"classes.dex";frozen_dex=sha(dex);(w/"baseline-cli-sha256.txt").open("x").write(sha(args.neverd)+"\n");dump=v.run([v.dexdump,"-f","-h","-l","plain",dex],"sdk-inventory");inventory=parse_dexdump(dump,dex.name,dex_header(dex.read_bytes()))
  if {r["descriptor"] for r in inventory["classes"]}!=classes or {r["identity"] for r in inventory["methods"]}!=methods:raise RuntimeError("D8 changed an owned instanceof constructor prefix definition")
  (w/"sdk-inventory.json").open("x").write(json.dumps(inventory,indent=2)+"\n")
  if args.linear_prefix:
   disassembly=v.run([v.dexdump,"-d",dex],"original-linear-prefix")
   witnesses=[]
   for block in re.finditer(r"(?ms)^\s+#\d+\s+:\s+\(in ([^)]+)\)\s*\n(.*?)(?=^\s+#\d+\s+:\s+\(in |^Class #|\Z)",disassembly):
    if block[1]!="Lfixture/Host;" or "'(Lfixture/Context;Lfixture/Handler;I)V'" not in block[2]:continue
    ops=[op.split('/')[0] for op in re.findall(r"\|[0-9a-fA-F]+:\s+(\S+)",block[2])]
    if not all(op in ops for op in ("invoke-static","instance-of","invoke-virtual","invoke-direct")):continue
    head=ops.index("invoke-direct")
    if not ops.index("invoke-static")<ops.index("instance-of")<ops.index("invoke-virtual")<head or any(op.startswith(("if-","goto")) for op in ops[:head]):raise RuntimeError("Owned linear type test changed execution order or gained a branch")
    witnesses.append(ops)
   if len(witnesses)!=1:raise RuntimeError("Owned DEX lacks its exact linear constructor type test")
   (w/"linear-prefix-witness.json").open("x").write(json.dumps({"dex_sha256":frozen_dex,"method":"Lfixture/Host;-><init>(Lfixture/Context;Lfixture/Handler;I)V","opcodes":witnesses[0]},indent=2)+"\n")
  output=w/"recovered";v.run([args.neverd.resolve(),"mobile",dex,*cp,"--timeout","600","-o",output],"recover");coverage=json.loads((output/"report.json").read_text())["android_method_recovery"];rows={r["identity"] for r in coverage["methods"]}
  if rows!=methods or coverage["class_count"]!=len(classes) or coverage["method_count"]!=len(methods) or coverage["unrecovered_method_count"] or coverage.get("projected_method_count",0) or coverage.get("class_source_bindings") or coverage.get("generic_signature_projections") or coverage.get("minimum_java_release",8)!=25:raise RuntimeError("Owned instanceof constructor prefix original coverage changed")
  helpers={r["class"]+"->"+r["name"]+r["prototype"] for r in coverage.get("generated_source_helpers",[])};allowed={o+"->__neverdThrow(Ljava/lang/Throwable;)Ljava/lang/RuntimeException;" for o in classes};rebuilt=w/"rebuilt";deps=[provider] if excluded else [];v.compile_local(sorted((output/"sources").rglob("*.java")),rebuilt,classpath=os.pathsep.join(map(str,deps)) if deps else None,release=25);actual_classes,actual_methods=compiler_inventory(v,rebuilt)
  if actual_classes!=classes or set(actual_methods)!=methods|helpers or not helpers<=allowed:raise RuntimeError("Owned instanceof constructor prefix source invented or omitted a definition")
  after=compiler_facts(rebuilt);after_exceptions=exception_inventory(rebuilt)
  for owner in classes:
   wanted=copy.deepcopy(facts[owner]);actual=copy.deepcopy(after[owner]);actual["methods"]={m:r for m,r in actual["methods"].items() if m not in helpers}
   if (wanted.get("major"),actual.get("major"))!=(52,69):raise RuntimeError("Owned original or required compiler release changed")
   actual["major"]=wanted["major"]
   if actual!=wanted:raise RuntimeError("Owned instanceof constructor prefix compiler metadata changed: "+owner)
   if any(after_exceptions[m]!=exceptions[m] for m in facts[owner]["methods"]):raise RuntimeError("Owned instanceof constructor prefix Exceptions changed")
  if execute((rebuilt,harness,*deps),"rebuilt")!=baseline:raise RuntimeError("Owned instanceof constructor prefix effects or results changed")
  if hashes(originals)!=frozen["originals"] or hashes(harness)!=frozen["harness"] or sha(dex)!=frozen_dex or sha(provider)!=provider_hash or sha(sdk)!=sdk_hash:raise RuntimeError("Frozen instanceof constructor prefix input changed")
  summary={"status":"verified","class_count":len(classes),"method_count":len(methods),"all_java_units":len(list((output/"sources").rglob("*.java"))),"minimum_java_release":25,"fresh_jvm_cases":len(baseline),"attempts_per_case":2,"wrong_programs":len(wrong),"sdk_provider":args.sdk_provider,"linear_prefix":args.linear_prefix};(w/"summary.json").open("x").write(json.dumps(summary,indent=2)+"\n");print(json.dumps(summary,sort_keys=True))
 return 0
if __name__=="__main__":raise SystemExit(main())
