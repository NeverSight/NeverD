import fixture.*;import java.lang.invoke.*;import java.lang.reflect.*;
public final class AllocatedUnaryOwnerHarness {
 static void require(boolean ok){if(!ok)throw new AssertionError("owned allocated unary owner");}
 static Core make(int route,Object input,String label)throws Throwable{
  if(route==0)return Client.make(input,label);if(route==1)return AllocatedUnaryOwnerCaller.make(input,label);
  if(route==2){try{return(Core)Core.class.getConstructor(Object.class,String.class).newInstance(input,label);}catch(InvocationTargetException f){throw f.getCause();}}
  return(Core)MethodHandles.publicLookup().findConstructor(Core.class,MethodType.methodType(void.class,Object.class,String.class)).invokeExact(input,label);
 }
 public static void main(String[] args)throws Throwable{int route=Integer.parseInt(args[0]),mode=Integer.parseInt(args[1]);
  Constructor<?> ctor=Core.class.getConstructor(State.class,Core.class,String.class);Type[] p=ctor.getGenericParameterTypes();require(Core.class.getTypeParameters().length==1 && ((ParameterizedType)p[0]).getActualTypeArguments()[0].equals(Core.class.getTypeParameters()[0]) && ((ParameterizedType)p[1]).getActualTypeArguments()[0] instanceof WildcardType);
  for(int attempt=0;attempt<2;attempt++){
   Object value=mode==2?null:mode==7?Long.valueOf(Long.MIN_VALUE):new Object();Object input=mode==1?null:value;String label=mode==3?null:new String("owned label");Trace.partial=null;Trace.checked=null;Trace.events=0;Trace.failAt=mode>=4&&mode<=6?mode-2:0;Trace.failure=new RuntimeException("owned ctor failure");
   Core result=null;Throwable caught=null;try{result=make(route,input,label);}catch(Throwable f){caught=f;}
   if(mode>=4&&mode<=6)require(caught==Trace.failure);else require(caught==null && result==Trace.partial);
   long expected=mode==4?2:mode==5?23:234;require(Trace.events==expected && Trace.checked==null);
   require(Trace.partial!=null && Trace.partial.state==(mode==4?null:result==null?Trace.partial.state:result.state) && Trace.partial.parent==null && Trace.partial.label==((mode==4||mode==5)?null:label));if(mode!=4)require(Trace.partial.state instanceof Mutable && Trace.partial.state.value==input);
   System.out.println("route="+route+",mode="+mode+",attempt="+attempt+",trace="+expected);
  }
 }
}
