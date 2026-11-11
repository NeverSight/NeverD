import fixture.*;
import java.lang.reflect.*;
import java.lang.invoke.*;
public final class ContextCallbackElementHarness {
 static void require(boolean value){if(!value)throw new AssertionError("owned context callback element");}
 static String call(int route,Frame frame,Fn callback)throws Throwable {
  if(route==0)return Client.call(frame,callback);if(route==1)return ContextCallbackElementCaller.call(frame,callback);
  if(route==2){try{return (String)Client.class.getMethod("call",Frame.class,Fn.class).invoke(null,frame,callback);}catch(InvocationTargetException e){throw e.getCause();}}
  return (String)MethodHandles.publicLookup().findStatic(Client.class,"call",MethodType.methodType(String.class,Frame.class,Fn.class)).invokeExact(frame,callback);
 }
 public static void main(String[] args)throws Throwable {
  int route=Integer.parseInt(args[0]),mode=Integer.parseInt(args[1]);Trace.failAt=mode>=1&&mode<=5?mode:0;Trace.failure=new RuntimeException("owned callback failure");
  Frame frame=mode==18?null:new Frame(mode==19?Long.MAX_VALUE:mode==20?0:Long.MIN_VALUE+37);
  Context context=new Context(Long.MAX_VALUE-1);Object input=mode==7?null:mode==8?new Object():context;
  Object saved=mode==12?new Object():mode==13?null:mode==15?context:new String("saved");Object peer=new Object(),sentinel=new Object();int[] callbacks={0};
  Fn<Context,Object> callback=mode==6?null:new Fn<Context,Object>(){public Object apply(Context value){require(value==input);callbacks[0]++;Trace.hit(3);if(mode==14)throw Trace.failure;if(value!=null)value.value++;if(mode==16){Trace.input=peer;frame.value++;}return saved;}};
  for(int attempt=0;attempt<2;attempt++) {
   Object reply=mode==9?new Object():mode==10?null:mode==11?saved:new String(mode==17&&attempt==1?"other":"result");
   Trace.events=0;Trace.input=input;Trace.context=Trace.saved=Trace.after=Trace.frame=sentinel;Trace.flags=17;Trace.reply=reply;int before=callbacks[0];long old=context.value;long oldFrame=frame==null?0:frame.value;Throwable failure=null;Object returned=sentinel;
   try{returned=call(route,frame,callback);}catch(Throwable e){failure=e;}
   int end=mode>=1&&mode<=5?mode:mode==8||mode==18?1:mode==6?2:mode==14?3:mode==9?4:5;
   int events=attempt==0?1:0;if(mode!=1)for(int e=2;e<=end;e++)events=events*10+e;if(mode==1&&attempt==1)events=0;
   require(Trace.events==events);boolean enters=mode!=1;
   require(Trace.context==(enters&&end>=2?input:sentinel));require(Trace.saved==(enters&&end>=4?saved:sentinel));require(Trace.after==(enters&&end>=5?reply:sentinel));
   boolean called=enters&&end>=3;require(callbacks[0]==before+(called?1:0));require(context.value==old+(called&&mode!=3&&mode!=14&&mode!=7?1:0));
   require(Trace.frame==(enters?frame:sentinel));require(Trace.flags==(enters&&mode!=18?oldFrame:17));require(frame==null||frame.value==oldFrame+(called&&mode==16?1:0));
   require(Trace.input==(mode==16&&called?peer:input));
   if(mode==1){if(attempt==0)require(failure instanceof ExceptionInInitializerError&&failure.getCause()==Trace.failure);else require(failure instanceof NoClassDefFoundError);}
   else if(mode>=2&&mode<=5||mode==14)require(failure==Trace.failure);
   else if(mode==6||mode==18)require(failure instanceof NullPointerException);
   else if(mode==8||mode==9)require(failure instanceof ClassCastException);
   else require(failure==null&&returned==reply);
   System.out.println("route="+route+",mode="+mode+",attempt="+attempt+",trace="+Trace.events);
  }
 }
}
