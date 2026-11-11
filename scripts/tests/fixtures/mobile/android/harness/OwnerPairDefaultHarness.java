import fixture.*;import java.lang.invoke.*;import java.lang.reflect.*;
public final class OwnerPairDefaultHarness {
 static void require(boolean ok){if(!ok)throw new AssertionError("owned owner pair default");}
 static boolean call(int route,DefaultApi receiver,Object a,Object b,String name)throws Throwable{
  if(route==0)return receiver.same(a,b);if(route==1)return OwnerPairDefaultCaller.call(receiver,a,b);
  if(route==2){if(name.equals("same")){try{return(Boolean)DefaultApi.DefaultImpls.class.getMethod("same",DefaultApi.class,Object.class,Object.class).invoke(null,receiver,a,b);}catch(InvocationTargetException f){throw f.getCause();}}Method helper=null;for(Method m:DefaultApi.class.getDeclaredMethods())if(m.getName().equals(name)){helper=m;break;}require(helper!=null);helper.setAccessible(true);try{return(Boolean)helper.invoke(receiver,a,b);}catch(InvocationTargetException f){throw f.getCause();}}
  MethodHandle handle=MethodHandles.publicLookup().findStatic(DefaultApi.DefaultImpls.class,"same",MethodType.methodType(boolean.class,DefaultApi.class,Object.class,Object.class));return(boolean)handle.invokeExact(receiver,a,b);
 }
 public static void main(String[] args)throws Throwable{int route=Integer.parseInt(args[0]),variant=Integer.parseInt(args[1]),mode=Integer.parseInt(args[2]);String name=args[3];
  require(DefaultApi.class.getTypeParameters().length==1);Type[] params=DefaultApi.class.getMethod("same",Object.class,Object.class).getGenericParameterTypes();require(params[0].equals(DefaultApi.class.getTypeParameters()[0]) && params[1].equals(params[0]));
  for(int attempt=0;attempt<2;attempt++){
   Object a=mode==1?null:new Object(),b=mode==2?null:new String("owned second"),first=mode==3?new Object():a,second=mode==4?new Object():b;
   DefaultApi receiver=mode==5?null:variant==0?new DefaultBase(first,second):new DefaultOverride(first,second);Trace.events=0;Trace.overrides=0;Trace.first=Trace.second=null;Trace.failAt=mode>=6?mode-5:0;Trace.failure=new RuntimeException("owned default failure");
   boolean actual=false;Throwable caught=null;try{actual=call(route,receiver,a,b,name);}catch(Throwable f){caught=f;}
   boolean virtual=mode!=5&&route==0&&variant==1;boolean early=mode==3;long expected=mode==5?0:virtual?9:mode==6?1:mode==7?12:mode==8||early?123:mode==9?1234:12345;
   if(mode==5)require(caught instanceof NullPointerException);else if(!virtual&&mode>=6&&!(early&&mode>=9))require(caught==Trace.failure);else require(caught==null && actual==(virtual||(!early&&mode!=4)));
   require(Trace.events==expected && Trace.overrides==(virtual?1:0));if(mode!=5&&!virtual)require(Trace.first==a&&Trace.second==b);
   System.out.println("route="+route+",variant="+variant+",mode="+mode+",attempt="+attempt+",trace="+expected);
  }
 }
}
