import fixture.*;
import java.lang.invoke.*;
import java.lang.reflect.*;
import java.util.*;

public final class NestedCarrierTransferHarness {
 static final String MARKER="owned nested carrier transfer";
 static void require(boolean ok){if(!ok)throw new AssertionError(MARKER);}
 static final class Token {
  public int hashCode(){throw new AssertionError(MARKER);}
  public String toString(){throw new AssertionError(MARKER);}
 }
 static final class Opaque extends AbstractList<Object> {
  final Object[] data={new Token()};
  public Object get(int index){throw new AssertionError(MARKER);}
  public int size(){throw new AssertionError(MARKER);}
  public Iterator<Object> iterator(){throw new AssertionError(MARKER);}
  public Object[] toArray(){throw new AssertionError(MARKER);}
  public <T>T[] toArray(T[] target){throw new AssertionError(MARKER);}
  public int hashCode(){throw new AssertionError(MARKER);}
  public boolean equals(Object other){throw new AssertionError(MARKER);}
  public String toString(){throw new AssertionError(MARKER);}
 }
 static final class Checked extends Exception {}
 @SuppressWarnings("unchecked") static <E extends Throwable> void fail(Throwable error)throws E{throw(E)error;}
 static final Method PRIVATE,ACCESS,CALL;
 static final MethodHandle PRIVATE_HANDLE,ACCESS_HANDLE,CALL_HANDLE;
 static {
  try {
   Class<?> facade=Class.forName("fixture.CoreKt");
   PRIVATE=facade.getDeclaredMethod("transfer",List.class,int.class,int.class);PRIVATE.setAccessible(true);
   ACCESS=facade.getDeclaredMethod("access$transfer",List.class,int.class,int.class);ACCESS.setAccessible(true);
   CALL=Caller.class.getDeclaredMethod("call",List.class,int.class,int.class);
   MethodType type=MethodType.methodType(Object.class,Object.class,int.class,int.class);
   PRIVATE_HANDLE=MethodHandles.lookup().unreflect(PRIVATE).asType(type);
   ACCESS_HANDLE=MethodHandles.lookup().unreflect(ACCESS).asType(type);
   CALL_HANDLE=MethodHandles.lookup().unreflect(CALL).asType(MethodType.methodType(Object.class,Object.class,Object.class,int.class,int.class));
  }catch(Exception error){throw new ExceptionInInitializerError(error);}
 }
 static final class Observer implements Point {
  final int mode,first,second,failPhase;final Object input,output;final Throwable failure;
  int events,calls;
  Observer(int mode,Object input,Object output,int first,int second){
   this.mode=mode;this.input=input;this.output=output;this.first=first;this.second=second;
   failPhase=mode==11||mode==12?0:mode==13||mode==14?1:-1;
   failure=mode==7||mode==12||mode==13?new Checked():mode==9||mode==14?new AssertionError("failure"):new IllegalStateException("failure");
  }
  public void visit(int phase,Object actualInput,int actualFirst,int actualSecond,Object actualOutput){
   require(actualInput==input&&actualFirst==first&&actualSecond==second);
   require(phase==0?events++==0&&actualOutput==null:phase==1&&events++==2&&actualOutput==output);
   if(phase==1&&mode==19)((Opaque)output).data[0]=this;
   if(phase==failPhase)NestedCarrierTransferHarness.<RuntimeException>fail(failure);
  }
  public Object choose(Object actualInput,int actualFirst,int actualSecond){
   require(events++==1&&calls++==0&&actualInput==input&&actualFirst==first&&actualSecond==second);
   if(mode==17)((Opaque)input).data[0]=this;
   if(mode==7||mode==8||mode==9)NestedCarrierTransferHarness.<RuntimeException>fail(failure);
   if(mode==10)throw(RuntimeException)null;
   return output;
  }
 }
 static Object call(int route,Object receiver,Object input,int first,int second)throws Throwable {
  try {
   if(route==0)return PRIVATE.invoke(null,input,first,second);
   if(route==1)return(Object)PRIVATE_HANDLE.invokeExact(input,first,second);
   if(route==2)return ACCESS.invoke(null,input,first,second);
   if(route==3)return(Object)ACCESS_HANDLE.invokeExact(input,first,second);
   if(route==4)return CALL.invoke(receiver,input,first,second);
   return(Object)CALL_HANDLE.invokeExact(receiver,input,first,second);
  }catch(InvocationTargetException error){throw error.getCause();}
 }
 public static void main(String[] args)throws Throwable {
  int route=Integer.parseInt(args[0]),mode=Integer.parseInt(args[1]);
  for(int attempt=0;attempt<2;attempt++)for(int variant=0;variant<6;variant++){
   Object input=mode==1?null:mode==4?Collections.emptyList():mode==5?Arrays.asList(new Token()):new Opaque();
   Object output=mode==2?null:mode==3?input:mode==15?new int[]{1,2}:mode==16?new Token():mode==18?Arrays.asList(new Token()):new Opaque();
   int first=mode==6?Integer.MIN_VALUE:variant==1?Integer.MAX_VALUE:variant==2?-1:7;
   int second=mode==6?Integer.MAX_VALUE:variant==3?Integer.MIN_VALUE:variant==4?0:19;
   boolean missing=route>=4&&mode==6,providerFailure=mode>=7&&mode<=10,bad=mode==15||mode==16;
   Observer observer=new Observer(mode,input,output,first,second);Trace.probe=observer;
   Object receiver=missing?null:new Caller(),actual=null;Throwable caught=null;
   try{actual=call(route,receiver,input,first,second);}catch(Throwable error){caught=error;}
   require(observer.events==(missing?0:observer.failPhase==0?1:providerFailure||bad?2:3));
   require(observer.calls==(missing||observer.failPhase==0?0:1));
   if(missing||mode==10)require(actual==null&&caught instanceof NullPointerException);
   else if(bad)require(actual==null&&caught instanceof ClassCastException);
   else if(observer.failPhase>=0||providerFailure)require(actual==null&&caught==observer.failure);
   else require(caught==null&&actual==output);
   if(mode==17)require(((Opaque)input).data[0]==observer);
   if(mode==19)require(((Opaque)output).data[0]==observer);
  }
  System.out.println("attempts=2,variants=6,nested-carrier-transfer=matched");
 }
}
