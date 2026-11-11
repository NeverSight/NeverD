import fixture.*;
import java.lang.invoke.*;
import java.lang.reflect.*;
public final class PairedArrayHarness {
 private static void require(boolean value) {
  if(!value) throw new AssertionError("owned independent array seeds changed");
 }
 public static void main(String[] args) throws Throwable {
  int route=Integer.parseInt(args[0]),mode=Integer.parseInt(args[1]);
  for(int attempt=0;attempt<2;++attempt) {
   Object old0=new Object(),old1=new Object();Object key=mode==1?null:new Object();
   Object[] source=mode==2?null:mode==3?new Object[0]:mode==4?new Object[]{old0}:new Object[]{old0,old1};
   int index=mode==5?-1:0,step=mode>=16?9:mode>=6?1+(mode-6)/2:0;
   Throwable failure=mode%2==1?new AssertionError("sentinel"):new IllegalStateException("sentinel");
   Trace.events=0;Trace.failStep=step;Trace.failure=failure;Trace.result=null;Trace.returnInput=mode==1;
   Object[] output=null;Throwable caught=null;
   try {
    if(route==0) output=PairedArrayCaller.make(source,index,key);
    else if(route==1) output=(Object[])Core.class.getMethod("make",Object[].class,int.class,Object.class).invoke(null,source,index,key);
    else if(route==2) output=(Object[])MethodHandles.publicLookup().findStatic(Core.class,"make",MethodType.methodType(Object[].class,Object[].class,int.class,Object.class)).invoke(source,index,key);
    else output=Core.make(source,index,key);
   }catch(InvocationTargetException e){caught=e.getCause();}catch(Throwable e){caught=e;}
   long expected=attempt==0?9:0;int last=0;
   if(step==9) expected=attempt==0?9:0;
   else for(int s=1;s<=5;++s) {
    expected=expected*10+s;last=s;
    if(s==step || s==2 && (source==null || source.length==0 || index<0)) break;
   }
   require(Trace.events==expected);
   if(step==9) {
    if(attempt==1) require(caught instanceof NoClassDefFoundError);
    else if(failure instanceof Error) require(caught==failure);
    else require(caught instanceof ExceptionInInitializerError && caught.getCause()==failure);
   }else if(mode==2) require(caught instanceof NullPointerException);
   else if(mode==3 || mode==5) require(caught instanceof ArrayIndexOutOfBoundsException);
   else require(caught==(step==0?null:failure));
   if(source!=null && source.length>0) {
    require(source[0]==(last>=3 && mode!=5?key:old0));
    if(source.length>1)require(source[1]==old1);
   }
   require((Trace.result!=null)==(last==5));
   if(last==5) {
    require(Trace.result==(mode==1?source:Trace.result));
    if(mode!=1) require(Trace.result!=source && Trace.result.length==source.length+1 && Trace.result[0]==null && Trace.result[1]==key && (source.length==1 || Trace.result[2]==old1));
   }
   require(output==(caught==null?Trace.result:null));
   System.out.println("route="+route+",mode="+mode+",attempt="+attempt+",events="+expected);
  }
 }
}
