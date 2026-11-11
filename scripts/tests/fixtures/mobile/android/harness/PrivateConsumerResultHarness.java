import fixture.*;
import java.lang.reflect.*;
import java.lang.invoke.*;
public final class PrivateConsumerResultHarness {
 static void require(boolean value){if(!value)throw new AssertionError("owned private consumer result");}
 static Object call(int route,Provider caller,Provider peer,Fn callback)throws Throwable {
  if(route==0)return Client.call(caller,peer,callback);if(route==1)return PrivateConsumerResultCaller.call(caller,peer,callback);
  if(route==2){try{return Client.class.getMethod("call",Provider.class,Provider.class,Fn.class).invoke(null,caller,peer,callback);}catch(InvocationTargetException e){throw e.getCause();}}
  return (Object)MethodHandles.publicLookup().findStatic(Client.class,"call",MethodType.methodType(Object.class,Provider.class,Provider.class,Fn.class)).invokeExact(caller,peer,callback);
 }
 public static void main(String[] args)throws Throwable {
  int route=Integer.parseInt(args[0]),mode=Integer.parseInt(args[1]);Trace.failAt=mode>=1&&mode<=6?mode:0;Trace.failure=new RuntimeException("owned consumer failure");
  Provider caller=mode==17?null:new Provider(new Object());Provider peer=mode==7?null:mode==16?caller:new Provider(mode==18?null:new Object());
  int[] input={7},callbacks={0};Object seed=mode==9?null:mode==10?new Object():input;
  Object saved=mode==11?new Object():mode==12?null:new Unit(new Object());Object sentinel=new Object(),replacement=new Object();
  Fn<int[],Object> callback=mode==8?null:new Fn<int[],Object>(){public Object apply(int[] value){require(value==seed);callbacks[0]++;Trace.hit(2);if(value!=null)value[0]++;if(mode==19)peer.token=replacement;return saved;}};
  Object first=Provider.result(new Object()),second=Provider.result(new Object());
  for(int attempt=0;attempt<2;attempt++) {
   Object reply=mode==13?new Object():mode==14?null:mode==15&&attempt==1?second:first;
   Trace.events=0;Trace.input=seed;Trace.reply=reply;Trace.receiver=Trace.token=Trace.saved=Trace.bound=Trace.after=Trace.last=sentinel;
   int before=callbacks[0],old=input[0];Object oldToken=peer==null?null:peer.token;Throwable failure=null;Object returned=sentinel;
   try{returned=call(route,caller,peer,callback);}catch(Throwable e){failure=e;}
   int end=mode>=1&&mode<=6?mode:mode==7||mode==17?0:mode==8||mode==10?1:mode==11?2:mode==13?3:6;
   int events=0;for(int event=1;event<=end;event++)events=events*10+event;
   require(Trace.events==events);require(Trace.receiver==(end>=1?peer:sentinel)&&Trace.token==(end>=1?oldToken:sentinel));
   require(Trace.saved==(end>=3?saved:sentinel));require(Trace.bound==(end>=4?reply:sentinel));require(Trace.after==(end>=5?reply:sentinel));require(Trace.last==(end>=6?reply:sentinel));
   require(callbacks[0]==before+(end>=2?1:0));require(input[0]==old+(end>=2&&mode!=2&&mode!=9?1:0));require(peer==null||peer.token==(mode==19&&end>=2?replacement:oldToken));
   if(mode>=1&&mode<=6)require(failure==Trace.failure);
   else if(mode==7||mode==8||mode==17)require(failure instanceof NullPointerException);
   else if(mode==10||mode==11||mode==13)require(failure instanceof ClassCastException);
   else require(failure==null&&returned==reply);
   System.out.println("route="+route+",mode="+mode+",attempt="+attempt+",trace="+Trace.events);
  }
 }
}
