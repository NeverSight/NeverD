import fixture.*;
import java.util.*;
import java.lang.invoke.*;
import java.lang.reflect.*;
public final class ProducerCallbackNominalHarness {
 static void require(boolean value) { if(!value)throw new AssertionError("owned producer callback nominal"); }
 static String call(int route,List values,Target target,CharSequence head,int limit,Fn callback) throws Throwable {
  if(route==0)return Client.call(values,target,head,limit,callback);
  if(route==1)return ProducerCallbackNominalCaller.call(values,target,head,limit,callback);
  if(route==2) { try{return (String)Client.class.getMethod("call",List.class,Target.class,CharSequence.class,int.class,Fn.class).invoke(null,values,target,head,limit,callback);}catch(InvocationTargetException e){throw e.getCause();} }
  return (String)MethodHandles.publicLookup().findStatic(Client.class,"call",MethodType.methodType(String.class,List.class,Target.class,CharSequence.class,int.class,Fn.class)).invokeExact(values,target,head,limit,callback);
 }
 public static void main(String[] args) throws Throwable {
  int route=Integer.parseInt(args[0]),mode=Integer.parseInt(args[1]);
  Trace.failAt=mode>=1&&mode<=9?mode:0;Trace.failure=new RuntimeException("owned failure");
  final Object sentinel=new Object();final int[] seed={17},callbacks={0},appends={0};final Object[] appended={sentinel,sentinel};
  Object element=mode==20?null:mode==21?new Object():seed;
  List values=mode==10?null:mode==11?Collections.emptyList():Collections.singletonList(element);
  Object text=mode==14?new Object():mode==18?null:new String("text");
  CharSequence head=mode==19?null:new String("head");int limit=Integer.MIN_VALUE+29;
  Target target=mode==13?null:new Target(){public Target append(CharSequence value){int index=appends[0]++%2;appended[index]=value;Trace.hit(index+7);return null;}};
  String peer=new String("opaque");
  Object result=mode==15?new Object():mode==16?null:mode==17?head:peer;
  Fn<int[],Object> callback=mode==12?null:new Fn<int[],Object>(){public Object apply(int[] value){require(value==element);callbacks[0]++;Trace.hit(4);if(value!=null)value[0]++;return text;}};
  for(int attempt=0;attempt<2;attempt++) {
   Trace.events=0;Trace.values=Trace.target=Trace.head=Trace.callback=Trace.element=Trace.text=Trace.after=sentinel;Trace.limit=7;Trace.result=result;
   appended[0]=appended[1]=sentinel;appends[0]=0;int before=callbacks[0],oldSeed=seed[0];Throwable failure=null;Object returned=sentinel;
   try{returned=call(route,values,target,head,limit,callback);}catch(Throwable e){failure=e;}
   int end=mode>=1&&mode<=9?mode:mode==10||mode==11?2:mode==12||mode==21?3:mode==14?4:mode==13?6:mode==15?8:9;
   int events=0;if(attempt==0)events=1;
   if(mode!=1)for(int e=2;e<=end;e++)events=events*10+e;
   if(mode==1&&attempt==1)events=0;
   require(Trace.events==events);
   boolean entered=mode!=1;
   require(Trace.values==(entered?values:sentinel)&&Trace.target==(entered?target:sentinel)&&Trace.head==(entered?head:sentinel)&&Trace.callback==(entered?callback:sentinel));
   require(Trace.limit==(entered?limit:7));
   require(Trace.element==(entered&&end>=3?element:sentinel));
   require(Trace.text==(entered&&end>=5?text:sentinel));
   require(Trace.after==(entered&&end>=9?result:sentinel));
   require(callbacks[0]==before+(entered&&end>=4?1:0));
   require(seed[0]==oldSeed+(entered&&end>=4&&mode!=4&&mode!=20?1:0));
   int count=entered&&end>=7?end>=8?2:1:0;require(appends[0]==count);
   require(appended[0]==(count>=1?head:sentinel)&&appended[1]==(count>=2?text:sentinel));
   if(mode==1){if(attempt==0)require(failure instanceof ExceptionInInitializerError&&failure.getCause()==Trace.failure);else require(failure instanceof NoClassDefFoundError);}
   else if(mode>=2&&mode<=9)require(failure==Trace.failure);
   else if(mode==10||mode==12||mode==13)require(failure instanceof NullPointerException);
   else if(mode==11)require(failure instanceof IndexOutOfBoundsException);
   else if(mode==14||mode==15||mode==21)require(failure instanceof ClassCastException);
   else require(failure==null&&returned==result);
   System.out.println("route="+route+",mode="+mode+",attempt="+attempt+",trace="+Trace.events);
  }
 }
}
