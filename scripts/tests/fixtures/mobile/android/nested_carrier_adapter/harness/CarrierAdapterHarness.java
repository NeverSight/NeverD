import fixture.*;
import java.lang.invoke.*;
import java.lang.reflect.*;
public final class CarrierAdapterHarness {
 static void require(boolean value){if(!value)throw new AssertionError("owned nested carrier adapter changed");}
 static Object call(int route,int operation,Pair input)throws Throwable{
  if(route==0)return operation==0?Client.whole(input):operation==1?Client.first(input):Client.tail(input);
  if(route==1)return CarrierAdapterCaller.call(operation,input);
  String name=operation==0?"whole":operation==1?"first":"tail";
  if(route==2)try{return Client.class.getMethod(name,Pair.class).invoke(null,input);}catch(InvocationTargetException error){throw error.getCause();}
  return MethodHandles.publicLookup().findStatic(Client.class,name,MethodType.methodType(operation==1?String.class:Object.class,Pair.class)).invoke(input);
 }
 @SuppressWarnings({"rawtypes","unchecked"})
 public static void main(String[] args)throws Throwable{
  int route=Integer.parseInt(args[0]),operation=Integer.parseInt(args[1]),mode=Integer.parseInt(args[2]);
  RuntimeException exact=new RuntimeException("owned carrier failure");Trace.failure=exact;Trace.failAt=mode>=1&&mode<=4?mode:0;
  Object sentinel=new Object();
  for(int attempt=0;attempt<2;++attempt){
   Object leftInput=new Object(),rightInput=new Object();Pair input=mode==5?null:new Pair(leftInput,rightInput);
   Object first=mode==10?new Object():mode==12?input:mode==14?null:new String("owned nested result");
   Object tail=mode==15?input:new Object();
   Object left=mode==8?new Object():mode==9?null:new Box(first);
   Object right=mode==11?new Object():mode==13?null:new Box(tail);
   Pair pair=new Pair(left,right);Trace.result=mode==6?new Object():mode==7?null:pair;
   Trace.events=0;Trace.seen=sentinel;Trace.left=sentinel;Trace.right=sentinel;Trace.after=sentinel;
   Object result=null;Throwable failure=null;try{result=call(route,operation,input);}catch(Throwable error){failure=error;}
   int events=attempt==0?1:0;boolean body=mode!=1;
   if(body)events=events*10+2;
   boolean read=body&&mode!=2&&mode!=5;
   if(read)events=events*10+3;
   boolean fetched=read&&mode!=3;
   if(fetched)events=events*10+4;
   boolean returned=fetched&&mode!=4&&mode!=6;
   if(returned)events=events*10+5;
   require(Trace.events==events&&Trace.seen==(body?input:sentinel));
   require(Trace.left==(read?leftInput:sentinel)&&Trace.right==(read?rightInput:sentinel)&&Trace.after==(returned?Trace.result:sentinel));
   if(mode==1){if(attempt==0)require(failure instanceof ExceptionInInitializerError&&failure.getCause()==exact);else require(failure instanceof NoClassDefFoundError);}
   else if(mode>=2&&mode<=4)require(failure==exact);
   else if(mode==5||(operation!=0&&mode==7)||(operation==1&&mode==9)||(operation==2&&mode==13))require(failure instanceof NullPointerException);
   else if(mode==6||(operation==1&&(mode==8||mode==10||mode==12))||(operation==2&&mode==11))require(failure instanceof ClassCastException);
   else require(failure==null&&result==(operation==0?Trace.result:operation==1?first:tail));
   System.out.println("route="+route+",operation="+operation+",mode="+mode+",attempt="+attempt+",events="+Trace.events);
  }
 }
}
