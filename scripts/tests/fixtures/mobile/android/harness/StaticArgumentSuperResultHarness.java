import fixture.*;
import java.lang.invoke.*;
import java.lang.reflect.*;
public final class StaticArgumentSuperResultHarness {
 private static void require(boolean value) {
  if(!value) throw new AssertionError("owned static argument super result changed");
 }
 public static void main(String[] args) throws Throwable {
  int route=Integer.parseInt(args[0]),mode=Integer.parseInt(args[1]);
  for(int attempt=0;attempt<2;++attempt) {
   Carrier<String> first=mode==1 || mode==3?null:new Carrier<String>("first");
   Carrier<Long> second=mode==2 || mode==3?null:new Carrier<Long>(Long.valueOf(71));
   Carrier<Integer> result=mode==4?null:new Carrier<Integer>(Integer.valueOf(29));
   int[] failures={1,9,2,3,4,5,6};
   int step=mode<5?0:failures[(mode-5)/2];
   Throwable failure=mode%2==0?new AssertionError("sentinel"):new IllegalStateException("sentinel");
   Trace.events=0;Trace.failStep=step;Trace.failure=failure;Trace.token=null;Trace.parent=null;
   Trace.returned=attempt==0?new Carrier<Object>(new Object()):result;Trace.prepared=result;
   Core<?,?> output=null;Throwable caught=null;
   try {
    if(route==0) output=new Core<String,Long>(first,second);
    else if(route==1) output=(Core<?,?>)Core.class.getConstructor(Carrier.class,Carrier.class).newInstance(first,second);
    else if(route==2) output=(Core<?,?>)MethodHandles.publicLookup().findConstructor(Core.class,MethodType.methodType(void.class,Carrier.class,Carrier.class)).invoke(first,second);
    else {
     MethodHandle handle=MethodHandles.publicLookup().findConstructor(Core.class,MethodType.methodType(void.class,Carrier.class,Carrier.class));
     output=(Core<?,?>)handle.invokeWithArguments(first,second);
    }
   } catch(InvocationTargetException e) {caught=e.getCause();} catch(Throwable e) {caught=e;}
   int[] phases=attempt==0?new int[]{1,9,2,3,4,5,6}:new int[]{1,2,3,4,5,6};
   long expected=0;int last=0;
   for(int phase:phases) {
    expected=expected*10+phase;last=phase;
    if(phase==step || attempt==1 && step==9 && phase==1) break;
   }
   require(Trace.events==expected);
   boolean called=last!=1 && last!=9;
   boolean parent=called && last>=3;
   require(Trace.token==(called?Payload.class:null));
   require((Trace.parent!=null)==parent);
   if(parent) {
    require(Trace.parent.carrier==(last>=4?result:null));
    Core<?,?> partial=(Core<?,?>)Trace.parent;
    require(partial.first==(last>=5?first:null));
    require(partial.second==(last>=6?second:null));
   }
   require(output==(caught==null?Trace.parent:null));
   if(step==9) {
    if(attempt==1) require(caught instanceof NoClassDefFoundError);
    else if(failure instanceof Error) require(caught==failure);
    else require(caught instanceof ExceptionInInitializerError && caught.getCause()==failure);
   } else require(caught==(step==0?null:failure));
   System.out.println("route="+route+",mode="+mode+",attempt="+attempt+",events="+expected);
  }
 }
}
