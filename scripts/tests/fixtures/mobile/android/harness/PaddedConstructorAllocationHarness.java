import fixture.*;
import java.lang.invoke.*;
import java.lang.reflect.*;
import java.util.*;

public final class PaddedConstructorAllocationHarness {
 static final String MARKER="owned padded constructor allocation";
 static void require(boolean ok){if(!ok)throw new AssertionError(MARKER);}
 static final class Token {
  public int hashCode(){throw new AssertionError(MARKER);}
  public String toString(){throw new AssertionError(MARKER);}
 }
 static final class Checked extends Exception {}
 @SuppressWarnings("unchecked") static <E extends Throwable> void fail(Throwable error)throws E{throw(E)error;}
 static final class Observer implements Point {
  final Object[] expected;
  final int mode,failPhase;
  final Throwable failure;
  Value value;Core core;Object[] before,after;int phases;
  Observer(int mode){
   this.mode=mode;expected=new Object[6];for(int i=0;i<6;i++)expected[i]=mode==1?null:new Token();
   if(mode==2)Arrays.fill(expected,expected[0]);
   if(mode==3){expected[5]=expected[0];expected[2]=expected[1];}
   if(mode==15){expected[0]=new String("first");expected[1]=new int[]{1,2};expected[5]=new Object[]{expected[2]};}
   failPhase=mode==4||mode==7||mode==10?0:mode==5||mode==8||mode==11?1:mode==6||mode==9||mode==12?2:-1;
   failure=mode==6||mode==7?new AssertionError("failure"):mode==5||mode==9?new Checked():new IllegalStateException("failure");
  }
  Object[] fields(){return new Object[]{value.a,value.b,value.c,value.d,value.e,value.f};}
  void same(Object[] left,Object[] right){require(left.length==right.length);for(int i=0;i<left.length;i++)require(left[i]==right[i]);}
  public void visit(int phase,Object owner,Object[] arguments){
   require(phase==phases);phases++;
   if(phase==0){require(owner instanceof Value&&value==null);value=(Value)owner;same(fields(),new Object[6]);same(arguments,expected);before=arguments;}
   else if(phase==1){require(owner==value);same(fields(),expected);same(arguments,expected);after=arguments;require(before!=after);}
   else {require(phase==2&&owner instanceof Core&&core==null);core=(Core)owner;require(core.value==value&&arguments.length==1&&arguments[0]==value);same(fields(),expected);}
   if(mode==13&&phase==0)Arrays.fill(arguments,new Token());
   if(phase==failPhase){if(mode>=10&&mode<=12)throw (RuntimeException)null;PaddedConstructorAllocationHarness.<RuntimeException>fail(failure);}
  }
 }
 static Core create(int route,Object[] values)throws Throwable {
  if(route==0)return new Core(values[0],values[1],values[2],values[3],values[4],values[5]);
  if(route==1){try{return Core.class.getConstructor(Object.class,Object.class,Object.class,Object.class,Object.class,Object.class).newInstance(values);}catch(InvocationTargetException error){throw error.getCause();}}
  MethodHandle ctor=MethodHandles.publicLookup().findConstructor(Core.class,MethodType.methodType(void.class,Object.class,Object.class,Object.class,Object.class,Object.class,Object.class));
  return(Core)ctor.invokeExact(values[0],values[1],values[2],values[3],values[4],values[5]);
 }
 public static void main(String[] args)throws Throwable {
  int route=Integer.parseInt(args[0]),mode=Integer.parseInt(args[1]);
  require(Modifier.isFinal(Core.class.getField("value").getModifiers()));
  for(int attempt=0;attempt<2;attempt++){
   Observer observer=new Observer(mode);Trace.probe=observer;Core actual=null;Throwable caught=null;
   try{actual=create(route,observer.expected);}catch(Throwable error){caught=error;}
   require(observer.value!=null&&observer.phases==(observer.failPhase<0?3:observer.failPhase+1));
   if(observer.failPhase>=0){require(actual==null);if(mode>=10&&mode<=12)require(caught instanceof NullPointerException);else require(caught==observer.failure);}
   else require(caught==null&&actual==observer.core&&actual.value==observer.value);
   observer.same(observer.fields(),observer.failPhase==0?new Object[6]:observer.expected);
   if(observer.failPhase<0||observer.failPhase==2)require(observer.core!=null&&observer.core.value==observer.value);else require(observer.core==null);
  }
  System.out.println("attempts=2,padded-constructor-allocation=matched");
 }
}
