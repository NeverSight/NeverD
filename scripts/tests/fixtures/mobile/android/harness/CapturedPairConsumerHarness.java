import fixture.*;import java.lang.invoke.*;import java.lang.reflect.*;
public class CapturedPairConsumerHarness {
 static void require(boolean ok){if(!ok)throw new AssertionError("owned captured pair consumer");}
 static void call(int route,Outer outer,Outer.Member member,Object a,Object b,Spec spec,String label,int mode)throws Throwable{
  if(route==0){Caller.call(outer,member,a,b,spec,label,mode);return;}if(route==1){CapturedPairConsumerCaller.call(outer,member,a,b,spec,label,mode);return;}
  if(route==2){try{Caller.class.getMethod("call",Outer.class,Outer.Member.class,Object.class,Object.class,Spec.class,String.class,int.class).invoke(null,outer,member,a,b,spec,label,mode);}catch(InvocationTargetException f){throw f.getCause();}return;}
  MethodHandles.publicLookup().findStatic(Caller.class,"call",MethodType.methodType(void.class,Outer.class,Outer.Member.class,Object.class,Object.class,Spec.class,String.class,int.class)).invokeExact(outer,member,a,b,spec,label,mode);
 }
 public static void main(String[]args)throws Throwable{int route=Integer.parseInt(args[0]),mode=Integer.parseInt(args[1]);boolean projected=args[2].equals("source");require(Modifier.isStatic(Outer.Member.class.getModifiers())==projected);Field capture=Outer.Member.class.getDeclaredField("this$0");capture.setAccessible(true);
  for(int attempt=0;attempt<2;attempt++){
   Object a=mode==1?null:new Object(),b=new Object(),value=new Object(),seed=new Object();Outer outer=new Outer(new Object());Vector vector=mode==10?null:new ConcreteVector(new int[]{attempt});Trace.fail=0;Outer.Member original=outer.new Member(value,vector);Outer.Member member=mode==2?null:original;Outer passed=mode==3?null:outer;Spec spec=mode==4?null:new Spec(){public Object read(){Trace.hit(5);return seed;}};String label=mode==11?null:new String("owned label");int selector=mode==12?Integer.MIN_VALUE:mode==13?Integer.MAX_VALUE:attempt;
   Trace.outer=Trace.member=Trace.first=Trace.second=Trace.label=Trace.value=Trace.vector=Trace.seed=null;Trace.mode=0;Trace.events=0;Trace.fail=mode>=5&&mode<=9?mode-4:0;Trace.failure=new RuntimeException("owned capture failure");Throwable caught=null;
   try{call(route,passed,member,a,b,spec,label,selector);}catch(Throwable f){caught=f;}
   if(mode>=5&&mode<=9)require(caught==Trace.failure);else if(mode==2||mode==4)require(caught instanceof NullPointerException);else require(caught==null);
   long expected=mode==2||mode==5?1:mode==6?12:mode==4||mode==7?123:mode==9?1235:12354;require(Trace.events==expected);
   require(Trace.outer==passed&&Trace.member==member&&Trace.first==a&&Trace.second==b&&Trace.label==label&&Trace.mode==selector);
   require(Trace.value==((mode==2||mode==5)?null:value)&&Trace.vector==((mode==2||mode==5||mode==6)?null:vector)&&Trace.seed==((mode==2||mode==4||mode==5||mode==6||mode==7||mode==9)?null:seed));require(capture.get(original)==outer);
   System.out.println("route="+route+",mode="+mode+",attempt="+attempt+",trace="+expected);
  }
 }
}
