import fixture.*;
import java.lang.invoke.*;
import java.lang.reflect.*;
public final class ConcreteSuperHarness {
 private static void require(boolean value) {
  if(!value) throw new AssertionError("owned concrete superclass resolution changed");
 }
 public static void main(String[] args) throws Throwable {
  int route=Integer.parseInt(args[0]),mode=Integer.parseInt(args[1]);
  for(int attempt=0;attempt<2;++attempt) {
   Object value=mode==2?null:new Object(),other=new Object();
   int step=mode==11||mode==12?9:mode>=3&&mode<=10?1+(mode-3)/2:0;
   Throwable failure=mode>=3&&mode%2==0?new AssertionError("sentinel"):new IllegalStateException("sentinel");
   Trace.events=0;Trace.failStep=step;Trace.failure=failure;Trace.answer=mode==1;Trace.commit=31;
   Trace.input=value;Trace.other=other;Trace.receiver=null;Trace.seen=null;Trace.created=null;
   Child<Object> receiver=null;Boolean output=null;Throwable caught=null;
   try {
    Child<Object> actual=new Child<Object>();receiver=mode==13?null:actual;
    if(route==0) output=ConcreteSuperCaller.call(receiver,value);
    else if(route==1) output=(Boolean)Child.class.getMethod("touch",Object.class).invoke(receiver,value);
    else if(route==2) output=(boolean)MethodHandles.publicLookup().findVirtual(Child.class,"touch",MethodType.methodType(boolean.class,Object.class)).invoke(receiver,value);
    else output=receiver.touch(value);
   } catch(InvocationTargetException e){caught=e.getCause();}catch(Throwable e){caught=e;}
   long expected=attempt==0?9:0;int last=0;
   if(step==9) expected=attempt==0?9:0;
   else if(mode!=13) for(int s=1;s<=4;++s){expected=expected*10+s;last=s;if(s==step)break;}
   require(Trace.events==expected);
   if(step==9){
    if(attempt==1)require(caught instanceof NoClassDefFoundError);
    else if(failure instanceof Error)require(caught==failure);
    else require(caught instanceof ExceptionInInitializerError&&caught.getCause()==failure);
   }else if(mode==13)require(caught instanceof NullPointerException);
   else require(caught==(step==0?null:failure));
   require((Trace.created!=null)==(step!=9));
   require(Trace.receiver==(last>=2?receiver:null));
   require(Trace.seen==(last>=2?value:null));
   require(Trace.input==(last>=1?other:value));
   if(Trace.created!=null)require(Trace.created.committed==(last>=3?73:0));
   require(Trace.answer==(last==4?(mode!=1):(mode==1)));
   require((output!=null)==(caught==null));
   if(output!=null)require(output.booleanValue()==(mode==1));
   System.out.println("route="+route+",mode="+mode+",attempt="+attempt+",events="+expected);
  }
 }
}
