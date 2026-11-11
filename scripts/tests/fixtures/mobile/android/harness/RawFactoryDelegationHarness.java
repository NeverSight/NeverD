import fixture.*;
import java.lang.invoke.*;
import java.lang.reflect.*;
public final class RawFactoryDelegationHarness {
 static void require(boolean value){if(!value)throw new AssertionError("owned raw factory delegation");}
 static Core call(int route)throws Throwable {
  if(route==0)return Client.call();if(route==1)return RawFactoryDelegationCaller.call();
  if(route==2){try{return (Core)Client.class.getMethod("call").invoke(null);}catch(InvocationTargetException e){throw e.getCause();}}
  return (Core)MethodHandles.publicLookup().findStatic(Client.class,"call",MethodType.methodType(Core.class)).invokeExact();
 }
 public static void main(String[] args)throws Throwable {
  int route=Integer.parseInt(args[0]),mode=Integer.parseInt(args[1]);Trace.failAt=mode>=1&&mode<=7?mode:0;Trace.failure=new RuntimeException("owned constructor failure");
  Object sentinel=new Object(),value=mode==10?null:mode==11?Long.valueOf(Long.MIN_VALUE+41):new Object();Box box=new Box(value);Core previous=null;
  for(int attempt=0;attempt<2;attempt++) {
   Box actualBox=mode==12&&attempt==1?new Box(new Object()):box;Object reply=mode==8?null:mode==9?new Object():actualBox;
   Trace.events=0;Trace.reply=reply;Trace.replacement=mode==14?null:new Object();Trace.mutate=mode==13||mode==14;Trace.partial=Trace.element=Trace.after=sentinel;
   Object before=actualBox.value,expectedValue=Trace.mutate?Trace.replacement:before;Throwable failure=null;Core returned=null;
   try{returned=call(route);}catch(Throwable e){failure=e;}
   int end=mode>=1&&mode<=7?mode:mode==9?2:mode==8?4:7;
   int events=attempt==0?1:0;if(mode!=1)for(int e=2;e<=end;e++)events=events*10+e;if(mode==1&&attempt==1)events=0;
   require(Trace.events==events);boolean entered=mode!=1;
   boolean published=entered&&end>=3;require((Trace.partial!=sentinel)==published);
   if(published){Core partial=(Core)Trace.partial;require(partial.values==(end>=4?reply:null));if(failure==null)require(returned==partial&&returned!=previous);}
   require(Trace.element==(entered&&end>=5?expectedValue:sentinel));require(Trace.after==(entered&&end>=7?Trace.partial:sentinel));
   boolean mutation=Trace.mutate&&entered&&mode!=2;require(actualBox.value==(mutation?Trace.replacement:before));
   if(mode==1){if(attempt==0)require(failure instanceof ExceptionInInitializerError&&failure.getCause()==Trace.failure);else require(failure instanceof NoClassDefFoundError);}
   else if(mode>=2&&mode<=7)require(failure==Trace.failure);
   else if(mode==8)require(failure instanceof NullPointerException);
   else if(mode==9)require(failure instanceof ClassCastException);
   else require(failure==null&&returned==Trace.partial);
   previous=returned;System.out.println("route="+route+",mode="+mode+",attempt="+attempt+",trace="+Trace.events);
  }
 }
}
