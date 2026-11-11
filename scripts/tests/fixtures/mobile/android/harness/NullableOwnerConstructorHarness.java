import fixture.*;import java.lang.invoke.*;import java.lang.reflect.*;
public final class NullableOwnerConstructorHarness {
 static void require(boolean ok){if(!ok)throw new AssertionError("owned nullable owner constructor");}
 static Core make(int route,Mutable input,String label)throws Throwable{
  if(route==0)return Client.make(input,label);if(route==1)return NullableOwnerConstructorCaller.make(input,label);
  if(route==2){try{return(Core)Core.class.getConstructor(Mutable.class,String.class).newInstance(input,label);}catch(InvocationTargetException f){throw f.getCause();}}
  return(Core)MethodHandles.publicLookup().findConstructor(Core.class,MethodType.methodType(void.class,Mutable.class,String.class)).invokeExact(input,label);
 }
 public static void main(String[] args)throws Throwable{int route=Integer.parseInt(args[0]),mode=Integer.parseInt(args[1]);
  Constructor<?> ctor=Core.class.getConstructor(State.class,Core.class,String.class);Type[] p=ctor.getGenericParameterTypes();require(Core.class.getTypeParameters().length==1 && ((ParameterizedType)p[0]).getActualTypeArguments()[0].equals(Core.class.getTypeParameters()[0]) && ((ParameterizedType)p[1]).getActualTypeArguments()[0] instanceof WildcardType);
  for(int attempt=0;attempt<2;attempt++){
   Object value=mode==2?null:mode==7?Long.valueOf(Long.MIN_VALUE):new Object();Mutable input=mode==1?null:new Mutable(value);String label=mode==3?null:new String("owned label");Trace.partial=null;Trace.checked=null;Trace.events=0;Trace.failAt=mode>=4&&mode<=6?mode-3:0;Trace.failure=new RuntimeException("owned ctor failure");
   Core result=null;Throwable caught=null;try{result=make(route,input,label);}catch(Throwable f){caught=f;}
   if(mode==1)require(caught instanceof IllegalArgumentException);else if(mode>=4&&mode<=6)require(caught==Trace.failure);else require(caught==null && result==Trace.partial);
   long expected=mode==1||mode==4?1:mode==5?12:mode==6?123:1234;require(Trace.events==expected && Trace.checked==input);
   if(mode==1||mode==4)require(Trace.partial==null);else require(Trace.partial!=null && Trace.partial.state==(mode==5?null:input) && Trace.partial.parent==null && Trace.partial.label==((mode==5||mode==6)?null:label));
   if(input!=null)require(input.value==value);System.out.println("route="+route+",mode="+mode+",attempt="+attempt+",trace="+expected);
  }
 }
}
