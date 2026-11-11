import fixture.*;
import java.lang.invoke.*;
import java.lang.reflect.*;
public final class NestedLockScopesHarness {
 static void require(boolean value){if(!value)throw new AssertionError("owned nested lock scopes");}
 static void call(int route,Object outer,Object inner,Fn callback)throws Throwable {
  if(route==0){Client.call(outer,inner,callback);return;}if(route==1){NestedLockScopesCaller.call(outer,inner,callback);return;}
  if(route==2){try{Client.class.getMethod("call",Object.class,Object.class,Fn.class).invoke(null,outer,inner,callback);}catch(InvocationTargetException e){throw e.getCause();}return;}
  MethodHandles.publicLookup().findStatic(Client.class,"call",MethodType.methodType(void.class,Object.class,Object.class,Fn.class)).invokeExact(outer,inner,callback);
 }
 public static void main(String[] args)throws Throwable {
  int route=Integer.parseInt(args[0]),mode=Integer.parseInt(args[1]);boolean alias=mode==11||mode==12||mode==19;
  Object outer=mode==8||mode==17||mode==18?null:new Object(),inner=mode==9||mode==16||mode==18?null:alias?outer:new Object();
  Trace.failure=new RuntimeException("owned primary");Error secondary=new AssertionError("owned secondary");
  Trace.failAt=mode>=1&&mode<=7?mode:mode==12||mode==13?3:mode==14?4:mode==15?2:mode==19?5:0;
  int secondaryAt=mode==13||mode==16?5:mode==14||mode==15||mode==17?6:0;
  Fn callback=mode==10?null:new Fn(){public void call(int event){
   boolean heldOuter=event>=2&&event<=5,heldInner=event==3||event==4||(alias&&heldOuter);
   require((outer!=null&&Thread.holdsLock(outer))==heldOuter);require((inner!=null&&Thread.holdsLock(inner))==heldInner);
   Trace.hit(event);if(event==secondaryAt)throw secondary;
  }};
  long[] expected={1234567,1,126,12356,123456,123456,123456,1234567,16,1256,0,1234567,12356,12356,123456,126,1256,16,16,123456};
  for(int attempt=0;attempt<2;attempt++){
   Trace.events=0;Trace.value=7;Throwable caught=null;try{call(route,outer,inner,callback);}catch(Throwable e){caught=e;}
   require(Trace.events==expected[mode]);require(Trace.value==(mode==0||mode>=4&&mode<=7||mode==11||mode==14||mode==19?8:7));
   require(outer==null||!Thread.holdsLock(outer));require(inner==null||!Thread.holdsLock(inner));
   if(secondaryAt!=0)require(caught==secondary);
   else if(Trace.failAt!=0)require(caught==Trace.failure);
   else if(mode==8||mode==9||mode==10||mode==18)require(caught instanceof NullPointerException);
   else require(caught==null);
   System.out.println("route="+route+",mode="+mode+",attempt="+attempt+",trace="+Trace.events);
  }
 }
}
