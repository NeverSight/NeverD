import fixture.Action;
import fixture.Core;
import fixture.Trace;
import fixture.Value;
import fixture.Wrapped;
import java.io.IOException;
import java.lang.invoke.MethodHandles;
import java.lang.invoke.MethodType;
import java.lang.reflect.InvocationTargetException;
public final class ProtectedSingleFinalHarness {
 private static void require(boolean value) { if(!value)throw new AssertionError("owned protected single final mismatch"); }
 private static final class Input { public String toString(){throw new AssertionError("owned protected single final mismatch");} }
 private static Core call(int route,Object input,long tag,boolean after) throws Throwable {
  if(route==0)return new Core(input,tag,after);
  if(route==1)return ProtectedSingleFinalCaller.call(input,tag,after);
  if(route==2){try{return Core.class.getConstructor(Object.class,long.class,boolean.class).newInstance(input,tag,after);}catch(InvocationTargetException failure){throw failure.getCause();}}
  return (Core)MethodHandles.publicLookup().findConstructor(Core.class,MethodType.methodType(void.class,Object.class,long.class,boolean.class)).invokeExact(input,tag,after);
 }
 public static void main(String[] args) throws Throwable {
  final int route=Integer.parseInt(args[0]),operation=Integer.parseInt(args[1]),mode=Integer.parseInt(args[2]);
  final boolean after=operation==1;
  final Object input=mode==1?null:new Input();
  final long tag=operation==0?Long.MIN_VALUE+mode:Long.MAX_VALUE-mode;
  Trace.checked=new IOException("owned checked");Trace.runtime=new IllegalStateException("owned runtime");Trace.fatal=new Error("owned error");Trace.failure=new IllegalArgumentException("owned stage");
  Trace.failAt=mode==5?1:mode==9?3:mode==10?4:mode==11?7:mode==12?8:mode==13?6:0;
  for(int attempt=0;attempt<2;attempt++) {
   Trace.events=0;Trace.partial=null;Trace.allocated=null;Trace.wrapper=null;Trace.caught=null;
   Trace.valueAction=mode==14?null:new Action(){public void run()throws IOException {
    Trace.hit(2);
    require(Trace.partial instanceof Core&&Trace.allocated instanceof Value);
    Core partial=(Core)Trace.partial;Value value=(Value)Trace.allocated;
    require(partial.value==null&&partial.stamp==tag&&value.input==input&&value.tag==tag);
    if(mode==2||mode==11||mode==13)throw Trace.checked;
    if(mode==3)throw Trace.runtime;
    if(mode==4)throw Trace.fatal;
   }};
   Trace.afterAction=mode==15?null:new Action(){public void run()throws IOException {
    Trace.hit(5);
    require(after&&Trace.partial instanceof Core&&((Core)Trace.partial).value==Trace.allocated);
    if(mode==6)throw Trace.checked;
    if(mode==7)throw Trace.runtime;
    if(mode==8)throw Trace.fatal;
   }};
   Core result=null;Throwable failure=null;
   try { result=call(route,input,tag,after); } catch(Throwable error){ failure=error; }
   boolean caught=mode==2||mode==11||mode==13||(after&&mode==6);
   boolean before=mode==2||mode==3||mode==4||mode==5||mode==9||mode==11||mode==13||mode==14;
   long expected=mode==5||mode==14?1:mode==3||mode==4?12:mode==9?123:mode==10||(after&&mode==15)?1234:mode==13?126:mode==2||mode==11?1267:after&&mode==6?1234567:after&&(mode==7||mode==8)?12345:after?123458:12348;
   require(Trace.events==expected&&Trace.partial instanceof Core);
   Core partial=(Core)Trace.partial;
   require(partial.stamp==tag&&partial.value==(before?null:Trace.allocated));
   require((Trace.allocated==null)==(mode==5));
   if(Trace.allocated!=null){Value value=(Value)Trace.allocated;require(value.input==input&&value.tag==tag);}
   require(Trace.caught==(caught?Trace.checked:null));
   require((Trace.wrapper!=null)==(caught&&mode!=13));
   if(Trace.wrapper!=null)require(((Wrapped)Trace.wrapper).getCause()==Trace.checked);
   Throwable exact=caught?(mode==11||mode==13?Trace.failure:(Throwable)Trace.wrapper):mode==3||(after&&mode==7)?Trace.runtime:mode==4||(after&&mode==8)?Trace.fatal:Trace.failAt!=0?Trace.failure:null;
   if(exact!=null)require(failure==exact&&result==null);
   else if(mode==14||(after&&mode==15))require(failure instanceof NullPointerException&&result==null);
   else require(failure==null&&result==partial&&result.value==Trace.allocated);
   System.out.println("route="+route+",operation="+operation+",mode="+mode+",attempt="+attempt+",trace="+Trace.events);
  }
 }
}
