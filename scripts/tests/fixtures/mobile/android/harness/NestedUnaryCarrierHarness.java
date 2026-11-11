import fixture.*;
import java.lang.invoke.*;
import java.lang.reflect.*;

public final class NestedUnaryCarrierHarness {
 static final String MARKER="owned nested unary carrier";
 static void require(boolean ok){if(!ok)throw new AssertionError(MARKER);}
 static final class Token {
  public int hashCode(){throw new AssertionError(MARKER);}
  public String toString(){throw new AssertionError(MARKER);}
 }
 static final class Checked extends Exception {}
 @SuppressWarnings("unchecked") static <E extends Throwable> void fail(Throwable error)throws E{throw(E)error;}
 static Object field(Object cell)throws Exception {
  Field f=cell.getClass().getDeclaredField("value");f.setAccessible(true);require(Modifier.isFinal(f.getModifiers()));return f.get(cell);
 }
 static final class Scenario implements Point {
  final int mode,failPhase;final Object initial,alternate=new Token(),other;final Source<Object> source;
  final Throwable failure;Object factory,allocated;int phases;
  Scenario(int mode){
   this.mode=mode;Trace.probe=null;other=Core.box(new Token());
   Source<Object> input=new Source<Object>(mode==2?null:mode==16?new int[]{1,2}:mode==17?new String("item"):mode==18?other:new Token());
   if(mode==3)input.value=input;initial=input.value;source=mode==1?null:input;
   Trace.override=mode==10||mode==11||mode==12;Trace.replacement=mode==10?null:mode==12?new Token():other;
   failPhase=mode==4?1:mode==5?2:mode==6?3:mode==7||mode==9?4:mode==8?0:-1;
   failure=mode==6?new AssertionError("failure"):mode==4||mode==7?new Checked():new IllegalStateException("failure");
  }
  public void visit(int phase,Object owner,Object first,Object second){
   require(phase==phases++);
   if(phase==0){require(factory==null&&owner!=null&&first==source&&second==null);factory=owner;}
   else if(phase==1){require(owner==source&&first==initial&&second==null);if(mode==14)source.value=alternate;}
   else if(phase==2){
    require(owner!=null&&allocated==null&&first==initial&&second==null);allocated=owner;
    require(owner.getClass()==factory.getClass().getEnclosingClass());
    require(Modifier.isPrivate(owner.getClass().getModifiers())&&Modifier.isStatic(owner.getClass().getModifiers()));
    try{require(field(owner)==null);}catch(Exception error){throw new AssertionError(MARKER,error);}
   }else if(phase==3){
    require(owner==allocated&&first==initial&&second==null);
    try{require(field(owner)==initial);}catch(Exception error){throw new AssertionError(MARKER,error);}
   }else {require(phase==4&&owner==factory&&first==source&&second==allocated);if(mode==15){Trace.override=true;Trace.replacement=other;}}
   if(phase==failPhase){if(mode==9)throw(RuntimeException)null;NestedUnaryCarrierHarness.<RuntimeException>fail(failure);}
  }
 }
 static Object call(int route,Scenario s)throws Throwable {
  if(route==0)return Core.call(s.source,s.mode==13);
  if(route==1){try{return Core.class.getMethod("call",Source.class,boolean.class).invoke(null,s.source,s.mode==13);}catch(InvocationTargetException error){throw error.getCause();}}
  MethodHandle h=MethodHandles.publicLookup().findStatic(Core.class,"call",MethodType.methodType(Object.class,Source.class,boolean.class));
  return(Object)h.invokeExact((Source)s.source,s.mode==13);
 }
 public static void main(String[] args)throws Throwable {
  int route=Integer.parseInt(args[0]),mode=Integer.parseInt(args[1]);Object previousFactory=null;
  for(int attempt=0;attempt<2;attempt++){
   Scenario s=new Scenario(mode);Trace.probe=s;Object actual=null;Throwable caught=null;
   try{actual=call(route,s);}catch(Throwable error){caught=error;}
   int expected=mode==13?0:mode==1?1:s.failPhase<0?5:s.failPhase+1;require(s.phases==expected);
   if(mode==1||mode==13||s.failPhase>=0||mode==12){
    require(actual==null);if(mode==1||mode==13||mode==9)require(caught instanceof NullPointerException);else if(mode==12)require(caught instanceof ClassCastException);else require(caught==s.failure);
   }else require(caught==null&&actual==(Trace.override?Trace.replacement:s.allocated));
   if(s.allocated!=null)require(field(s.allocated)==(s.failPhase==2?null:s.initial));
   if(mode==14)require(s.source.value==s.alternate&&field(s.allocated)==s.initial);
   if(mode!=13){if(previousFactory!=null)require(previousFactory==s.factory);previousFactory=s.factory;}
  }
  System.out.println("attempts=2,nested-unary-carrier=matched");
 }
}
