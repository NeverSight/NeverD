import fixture.*;import java.lang.invoke.*;import java.lang.reflect.*;
public final class PairedArrayCarrierHarness {
 static void require(boolean ok){if(!ok)throw new AssertionError("owned paired array carrier");}
 static Result<?,?> make(int route,Pair<Object,Object>[] items)throws Throwable{
  if(route==0)return Client.make(items);if(route==1)return PairedArrayCarrierCaller.make(items);
  if(route==2){try{return(Result<?,?>)Provider.class.getMethod("make",Pair[].class).invoke(null,(Object)items);}catch(InvocationTargetException f){throw f.getCause();}}
  return(Result<?,?>)MethodHandles.publicLookup().findStatic(Provider.class,"make",MethodType.methodType(Result.class,Pair[].class)).asFixedArity().invokeExact((Pair[])items);
 }
 public static void main(String[] args)throws Throwable{
  int route=Integer.parseInt(args[0]),mode=Integer.parseInt(args[1]);Method method=Provider.class.getMethod("make",Pair[].class);require(method.isVarArgs() && method.getTypeParameters().length==2 && method.getGenericParameterTypes()[0] instanceof GenericArrayType);
  for(int attempt=0;attempt<2;attempt++){
   Object key=mode==8?null:new Object(),value=mode==9?null:new Object();Pair<Object,Object> pair=new Pair<>(key,value);Pair<Object,Object>[] items=mode==1?null:new Pair[]{pair,new Pair<>(value,key)};if(mode==0)items=new Pair[0];if(mode==2)items[1]=null;
   Result<Object,Object> expected=mode==7?null:new Result<>(new Object(),new Object());Trace.output=expected;Trace.events=0;Trace.array=Trace.first=Trace.second=null;Trace.failAt=mode>=3&&mode<=5?mode-2:0;Trace.failure=new RuntimeException("owned array failure");
   Throwable caught=null;Result<?,?> actual=null;try{actual=make(route,items);}catch(Throwable t){caught=t;}
   long events=mode==0?13:mode==1||mode==3?1:mode==2||mode==4?12:1223;require(Trace.events==events && Trace.array==items);
   if(mode==1||mode==2)require(caught instanceof NullPointerException);else if(Trace.failAt!=0)require(caught==Trace.failure);else require(caught==null && actual==expected);
   boolean read=mode!=0&&mode!=1&&mode!=3;boolean second=read&&mode!=2&&mode!=4;require(Trace.first==(second?value:read?key:null) && Trace.second==(second?key:read?value:null));
   if(items!=null&&items.length!=0)require(items[0]==((mode==2||mode==3||mode==4)?pair:null));
   if(actual!=null)require(actual.first==expected.first && actual.second==expected.second);
   System.out.println("route="+route+",mode="+mode+",attempt="+attempt+",trace="+events);
  }
 }
}
