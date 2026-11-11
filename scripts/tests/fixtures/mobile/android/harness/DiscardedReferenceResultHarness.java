import fixture.*;
import java.lang.invoke.*;
import java.lang.reflect.*;

public final class DiscardedReferenceResultHarness {
 static final String MARKER="owned discarded reference result";
 static void require(boolean ok){if(!ok)throw new AssertionError(MARKER);}
 static final class Checked extends Exception {}
 @SuppressWarnings("unchecked") static <E extends Throwable> void fail(Throwable error)throws E{throw(E)error;}
 static final class Scenario implements Point {
  final int route,mode;final char input;final Core core=new Core(),other=new Core();
  final Throwable failure;int phases,expectedTotal;
  Scenario(int route,int mode){
   this.route=route;this.mode=mode;input=mode==1?0:mode==2?'\ud800':mode==3?'\udfff':mode==4?'\uffff':mode==5?(char)0x100:'a';
   core.total=123;expectedTotal=123;failure=mode==7?new AssertionError("failure"):mode==8?new Checked():new IllegalStateException("failure");
   Trace.result=mode==14?null:mode==15?core:other;
  }
  void abort(){DiscardedReferenceResultHarness.<RuntimeException>fail(failure);}
  public void visit(int phase,Core owner,char value){
   require(phase==phases++&&owner==core&&value==input);
   int frames=0;
   for(StackTraceElement frame:new Throwable().getStackTrace())if(frame.getClassName().equals("fixture.Core")&&!frame.getMethodName().startsWith("__neverdThrow"))frames++;
   require(frames==(route==3?1:2));
   if(phase==0){require(core.total==123);if(mode==13){core.total=307;expectedTotal=307;}if(mode==11)Trace.result=core;if(mode==6||mode==8)abort();}
   else {require(phase==1&&core.total==expectedTotal+input);if(mode==12)Trace.result=core;if(mode==7)abort();if(mode==9)throw(RuntimeException)null;}
  }
 }
 static Object call(int route,Core owner,char value,String alias)throws Throwable {
  if(route==0){Client.call(owner,value);return null;}
  if(route==1){
   Method wanted=null;for(Method m:Core.class.getDeclaredMethods())if(m.getName().equals(alias)&&m.getReturnType()==void.class&&java.util.Arrays.equals(m.getParameterTypes(),new Class<?>[]{char.class})){require(wanted==null);wanted=m;}
   require(wanted!=null&&Modifier.isFinal(wanted.getModifiers()));
   try{return wanted.invoke(owner,value);}catch(InvocationTargetException error){throw error.getCause();}
  }
  if(route==2){MethodHandle h=MethodHandles.publicLookup().findVirtual(Core.class,alias,MethodType.methodType(void.class,char.class));h.invokeExact(owner,value);return null;}
  if(route==3){MethodHandle h=MethodHandles.publicLookup().findVirtual(Core.class,"apply",MethodType.methodType(Core.class,char.class));return(Core)h.invokeExact(owner,value);}
  return((Port)owner).apply(value);
 }
 public static void main(String[] args)throws Throwable {
  int route=Integer.parseInt(args[0]),mode=Integer.parseInt(args[1]);String alias=args[2];
  for(int attempt=0;attempt<2;attempt++){
   Scenario s=new Scenario(route,mode);Trace.probe=s;Object actual=null;Throwable caught=null;
   try{actual=call(route,mode==10?null:s.core,s.input,alias);}catch(Throwable error){caught=error;}
   int phases=mode==10?0:mode==6||mode==8?1:2;require(s.phases==phases);
   if(mode>=6&&mode<=10&&mode!=11){
    require(actual==null);if(mode==9||mode==10)require(caught instanceof NullPointerException);else require(caught==s.failure);
   }else require(caught==null&&actual==(route<3?null:Trace.result));
   require(s.core.total==(mode==10||mode==6||mode==8?123:s.expectedTotal+s.input));
  }
  System.out.println("attempts=2,discarded-reference-result=matched");
 }
}
