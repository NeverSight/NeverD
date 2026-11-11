import fixture.*;
import java.lang.invoke.*;
import java.lang.reflect.*;
public final class ClosedBuilderHarness {
 static void require(boolean value){if(!value)throw new AssertionError("owned closed map builder changed");}
 static Child call(int route,Snapshot snapshot)throws Throwable {
  if(route==0)return new Child(snapshot);
  if(route==1)return ClosedBuilderCaller.make(snapshot);
  if(route==2)try{return Child.class.getConstructor(Snapshot.class).newInstance(snapshot);}
   catch(InvocationTargetException failure){throw failure.getCause();}
  return (Child)MethodHandles.publicLookup().findConstructor(Child.class,MethodType.methodType(void.class,Snapshot.class)).invoke(snapshot);
 }
 public static void main(String[] args)throws Throwable {
  int route=Integer.parseInt(args[0]),mode=Integer.parseInt(args[1]);
  RuntimeException exact=new RuntimeException("owned constructor failure");
  Trace.failure=exact;Trace.failAt=mode>=1&&mode<=4?mode+4:mode==8?3:mode==9?4:0;
  Object sentinel=new Object();
  for(int attempt=0;attempt<2;++attempt){
   Snapshot snapshot=mode==5||mode==6?null:new Snapshot();
   Key<Object> key=new Key<Object>(new Object());Box<Object> value=mode==10?null:new Box<Object>(new Object());
   Trace.key=key;Trace.value=value;Trace.mutate=mode==6||mode==7||mode==10||mode==11;
   Trace.events=0;Trace.partial=sentinel;Trace.seen=sentinel;
   Child result=null;Throwable failure=null;
   try{result=call(route,snapshot);}catch(Throwable problem){failure=problem;}
   boolean init=mode==8||mode==9;
   int trace=attempt==0?3:0;
   if(init){
    if(attempt==0&&mode==9)trace=34;
    else if(attempt==1)trace=0;
    require(Trace.events==trace&&Trace.partial==sentinel&&Trace.seen==sentinel&&result==null);
    if(attempt==0)require(failure instanceof ExceptionInInitializerError&&failure.getCause()==exact);
    else require(failure instanceof NoClassDefFoundError);
   }else{
    if(attempt==0)trace=34;
    trace=trace*10+5;
    boolean assigned=mode!=1,mutated=assigned&&Trace.mutate&&snapshot!=null;
    boolean parentDone=assigned&&mode!=6;
    if(parentDone)trace=trace*10+6;
    boolean childEntered=parentDone&&mode!=2;
    if(childEntered)trace=trace*10+7;
    boolean ownAssigned=childEntered&&mode!=3;
    if(ownAssigned)trace=trace*10+8;
    require(Trace.events==trace&&Trace.seen==snapshot&&Trace.partial instanceof Child);
    Child partial=(Child)Trace.partial;
    require(partial.inherited==(assigned?snapshot:null)&&partial.snapshot==(ownAssigned?snapshot:null));
    if(snapshot!=null)require(snapshot.size()==(mutated?1:0)&&(!mutated||snapshot.get(key)==value));
    if(mode>=1&&mode<=4)require(failure==exact&&result==null);
    else if(mode==6)require(failure instanceof NullPointerException&&result==null);
    else require(failure==null&&result==partial&&result.snapshot==snapshot&&result.inherited==snapshot);
   }
   System.out.println("route="+route+",mode="+mode+",attempt="+attempt+",events="+Trace.events);
  }
 }
}
