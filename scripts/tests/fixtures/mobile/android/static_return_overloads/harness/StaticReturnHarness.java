import fixture.*;
import java.lang.reflect.*;
import java.lang.invoke.*;
import java.util.*;
public final class StaticReturnHarness {
 static void require(boolean value){if(!value)throw new AssertionError("owned static return overload changed");}
 static Object call(int route,int slot,int consume,String alias,Object[] input,Box box,Factory factory,String stamp,int flags,long wide)throws Throwable{
  if(route==0)return StaticReturnCaller.call(slot,consume,input,box,factory,stamp,flags,wide);
  if(route==1){if(consume!=0)return slot==0?Client.genericValue(input,box,factory,stamp,flags,wide):Client.nominalValue(input,box,factory,stamp,flags,wide);return slot==0?Client.generic(input,box,factory,stamp,flags,wide):Client.nominal(input,box,factory,stamp,flags,wide);}
  Class<?> result=slot==0?Object.class:Box.class;Object value;
  Class<?>[] params={Object[].class,Box.class,Factory.class,String.class,int.class,long.class};
  if(route==2){Method selected=null;for(Method method:Core.class.getDeclaredMethods())if(method.getName().equals(slot==0?"select":alias)&&method.getReturnType()==result&&Arrays.equals(method.getParameterTypes(),params))selected=method;require(selected!=null);try{value=selected.invoke(null,input,box,factory,stamp,flags,wide);}catch(InvocationTargetException error){throw error.getCause();}}
  else value=MethodHandles.publicLookup().findStatic(Core.class,slot==0?"select":alias,MethodType.methodType(result,params)).invoke(input,box,factory,stamp,flags,wide);
  Trace.after=value;Trace.hit(slot==0?8:9);
  if(consume==0)return value;if(slot==0)return (String)value;String payload=((Box<String>)value).value;return payload;
 }
 @SuppressWarnings({"unchecked","rawtypes"})
 public static void main(String[] args)throws Throwable{
  int route=Integer.parseInt(args[0]),slot=Integer.parseInt(args[1]),consume=Integer.parseInt(args[2]),mode=Integer.parseInt(args[3]);String alias=args[4];
  Trace.mode=mode;RuntimeException exact=new RuntimeException("owned static failure");Trace.failure=exact;
  Object token=new Object(),sentinel=new Object();String text=new String("owned payload");
  Object[] input=mode==3?null:new Object[]{token};Box box=mode==4?null:new Box(token);Factory factory=mode==5?null:new Factory(){public Object create(){throw new AssertionError("unexpected factory invocation");}};
  String stamp=mode==6?null:new String("owned stamp");int flags=mode==7?Integer.MIN_VALUE:0x5a71;long wide=mode==8?Long.MIN_VALUE:0x1234567887654321L;
  Object result=mode==9?null:mode==10?token:mode==11?new Box(token):mode==12?new Box(null):slot==0?text:new Box(text);
  for(int attempt=0;attempt<2;++attempt){
   Trace.events=0;Trace.slot=-1;Trace.input=Trace.box=Trace.factory=Trace.stamp=Trace.after=sentinel;Trace.flags=0;Trace.wide=0;Trace.result=result;
   Object actual=null;Throwable failure=null;try{actual=call(route,slot,consume,alias,input,box,factory,stamp,flags,wide);}catch(Throwable error){failure=error;}
   if(mode==1){require(attempt==0?failure instanceof ExceptionInInitializerError&&failure.getCause()==exact:failure instanceof NoClassDefFoundError);require(Trace.events==(attempt==0?1:0)&&Trace.slot==-1&&Trace.input==sentinel&&Trace.after==sentinel);}
   else {
    int events=attempt==0?1:0;events=events*10+(slot==0?2:5);events=events*10+(slot==0?3:6);
    require(Trace.slot==slot&&Trace.input==input&&Trace.box==box&&Trace.factory==factory&&Trace.stamp==stamp&&Trace.flags==flags&&Trace.wide==wide);
    if(mode==2){require(failure==exact&&Trace.after==sentinel&&Trace.events==events);}
    else {
     events=events*10+(slot==0?4:7);boolean badOuter=slot==1&&result!=null&&!(result instanceof Box);
     if(badOuter)require(failure instanceof ClassCastException&&Trace.after==sentinel&&Trace.events==events);
     else {events=events*10+(slot==0?8:9);require(Trace.after==result&&Trace.events==events);
      if(consume==0)require(failure==null&&actual==result);
      else if(slot==1&&result==null)require(failure instanceof NullPointerException);
      else {Object payload=slot==0?result:((Box)result).value;if(payload!=null&&!(payload instanceof String))require(failure instanceof ClassCastException);else require(failure==null&&actual==payload);}
     }
    }
   }
   System.out.println("route="+route+",slot="+slot+",consume="+consume+",mode="+mode+",attempt="+attempt+",events="+Trace.events);
  }
 }
}
