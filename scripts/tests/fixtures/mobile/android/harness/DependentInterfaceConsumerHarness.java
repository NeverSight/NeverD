import fixture.*;
import java.lang.invoke.*;
import java.lang.reflect.*;

public final class DependentInterfaceConsumerHarness {
 static final String MARKER="owned dependent interface consumer";
 static void require(boolean ok){if(!ok)throw new AssertionError(MARKER);}
 static class Token {
  public int hashCode(){throw new AssertionError(MARKER);}
  public String toString(){throw new AssertionError(MARKER);}
 }
 static final class Scope extends Token implements Context {}
 static final class Checked extends Exception {}
 @SuppressWarnings("unchecked") static <E extends Throwable> void fail(Throwable error)throws E{throw(E)error;}
 static final Method USE,CALL;
 static final MethodHandle USE_HANDLE,CALL_HANDLE;
 static {
  try {
   USE=Core.class.getMethod("use",Object.class,Mapper.class,Context.class);
   CALL=Core.class.getMethod("call",Object.class,Mapper.class,Context.class);
   MethodType type=MethodType.methodType(Object.class,Object.class,Object.class,Object.class);
   USE_HANDLE=MethodHandles.lookup().unreflect(USE).asType(type);CALL_HANDLE=MethodHandles.lookup().unreflect(CALL).asType(type);
   TypeVariable<?>[] t=USE.getTypeParameters();require(t.length==3&&t[1].getBounds()[0]==Object.class&&t[2].getBounds()[0]==Object.class);
   ParameterizedType bound=(ParameterizedType)t[0].getBounds()[0];Type[] slots=bound.getActualTypeArguments();
   require(bound.getRawType()==Mapper.class&&slots.length==2&&slots[0].equals(t[1])&&slots[1].equals(t[2]));
  }catch(Exception error){throw new ExceptionInInitializerError(error);}
 }
 static final class Observer implements Point {
  final int mode;final Object value;final Context context;final Throwable failure;
  Object result,mapper;int phases,calls;
  Observer(int mode){
   this.mode=mode;value=mode==2?null:mode==15?new Object[]{new Token()}:mode==18?new String("typed"):new Token();
   context=mode==3?null:new Scope();result=mode==4?null:mode==5?value:mode==6?context:mode==16?new Object[]{new Token()}:new Token();
   failure=mode==7||mode==11||mode==13?new Checked():mode==8||mode==12?new AssertionError("failure"):new IllegalStateException("failure");
  }
  public void visit(int phase,Object actualValue,Object actualMapper,Object actualContext,Object actualResult){
   require(phase==phases++&&actualValue==value&&actualMapper==mapper&&actualContext==context&&actualResult==(phase==0?null:result));
   if(mode==16&&phase==1)((Object[])result)[0]=this;
   if(phase==0&&(mode==10||mode==13)||phase==1&&(mode==11||mode==12))DependentInterfaceConsumerHarness.<RuntimeException>fail(failure);
  }
  Object mapped(Context actualContext,Object actualValue){
   require(phases==1&&actualContext==context&&actualValue==value);calls++;
   if(mode==15)((Object[])value)[0]=this;
   if(mode==7||mode==8||mode==14)DependentInterfaceConsumerHarness.<RuntimeException>fail(failure);
   if(mode==9)throw(RuntimeException)null;
   return result;
  }
 }
 static final class Adapter implements Mapper<Object,Object> {
  final Observer state;Adapter(Observer state){this.state=state;}
  public Object apply(Context context,Object value){return state.mapped(context,value);}
 }
 static final class Strict implements Mapper<String,Object> {
  final Observer state;Strict(Observer state){this.state=state;}
  public Object apply(Context context,String value){return state.mapped(context,value);}
 }
 static Object call(int route,Object value,Object mapper,Object context)throws Throwable {
  try {
   if(route==0)return USE.invoke(null,value,mapper,context);
   if(route==1)return(Object)USE_HANDLE.invokeExact(value,mapper,context);
   if(route==2)return CALL.invoke(null,value,mapper,context);
   if(route==3)return(Object)CALL_HANDLE.invokeExact(value,mapper,context);
   return Core.call(value,(Mapper)mapper,(Context)context);
  }catch(InvocationTargetException error){throw error.getCause();}
 }
 public static void main(String[] args)throws Throwable {
  int route=Integer.parseInt(args[0]),mode=Integer.parseInt(args[1]);
  for(int attempt=0;attempt<2;attempt++){
   Observer state=new Observer(mode);state.mapper=mode==1?null:mode>=18?new Strict(state):new Adapter(state);Trace.probe=state;
   for(int round=0;round<(mode==17?2:1);round++){
    if(round>0)state.result=new Token();state.phases=0;int before=state.calls;Object actual=null;Throwable caught=null;
    try{actual=call(route,state.value,state.mapper,state.context);}catch(Throwable error){caught=error;}
    boolean pre=mode==10||mode==13,provider=mode==7||mode==8||mode==9||mode==14,strict=mode==19;
    require(state.phases==(pre||mode==1||provider||strict?1:2));
    require(state.calls==before+(pre||mode==1||strict?0:1));
    if(mode==1||mode==9)require(actual==null&&caught instanceof NullPointerException);
    else if(strict)require(actual==null&&caught instanceof ClassCastException);
    else if(pre||provider||mode==11||mode==12)require(actual==null&&caught==state.failure);
    else require(caught==null&&actual==state.result);
    if(mode==15)require(((Object[])state.value)[0]==state);
    if(mode==16)require(((Object[])state.result)[0]==state);
   }
  }
  System.out.println("attempts=2,dependent-interface-consumer=matched");
 }
}
