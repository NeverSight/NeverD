import fixture.*;
import kotlin.jvm.functions.*;
import java.lang.invoke.*;
import java.lang.reflect.*;

public final class PrivateContextFactoryHarness {
 static final String MARKER="owned private context factory";
 static void require(boolean ok){if(!ok)throw new AssertionError(MARKER);}
 static class Token {
  public int hashCode(){throw new AssertionError(MARKER);}
  public String toString(){throw new AssertionError(MARKER);}
 }
 static final class Scope extends Token implements Context {}
 static final class Checked extends Exception {}
 @SuppressWarnings("unchecked") static <E extends Throwable> void fail(Throwable error)throws E{throw(E)error;}
 static final Method PRIVATE,PUBLIC;
 static final MethodHandle PRIVATE_HANDLE,PUBLIC_HANDLE;
 static {
  try {
   Class<?> facade=Class.forName("fixture.CoreKt");
   PRIVATE=facade.getDeclaredMethod("make",Function2.class,Function1.class);PRIVATE.setAccessible(true);
   PUBLIC=facade.getDeclaredMethod("call",Function2.class,Function1.class);PUBLIC.setAccessible(true);
   MethodType type=MethodType.methodType(Object.class,Object.class,Object.class);
   PRIVATE_HANDLE=MethodHandles.lookup().unreflect(PRIVATE).asType(type);
   PUBLIC_HANDLE=MethodHandles.lookup().unreflect(PUBLIC).asType(type);
  }catch(Exception error){throw new ExceptionInInitializerError(error);}
 }
 static final class Observer implements Point {
  final int mode;final Context context;final Object input;final Throwable failure;
  Object middle,back,carrier,save,restore;long log;int forwardCalls,reverseCalls,failAt;
  Observer(int mode){
   this.mode=mode;context=mode==3?null:new Scope();input=mode==4?null:mode==17?new Object[]{new Token()}:new Token();
   middle=mode==5?null:mode==17?new Object[]{new Token()}:mode==18?context:new Token();back=mode==6?null:input;
   failAt=mode==9?1:mode==10?2:mode==11?3:mode==12?4:mode==13?5:mode==14?6:mode==7||mode==15?7:mode==8||mode==16?8:0;
   failure=mode==7||mode==8||mode==10||mode==13?new Checked():mode==11||mode==14||mode==16?new AssertionError("failure"):new IllegalStateException("failure");
  }
  void hit(int event){log=log*10+event;if(event==failAt){if(mode==15)throw(RuntimeException)null;PrivateContextFactoryHarness.<RuntimeException>fail(failure);}}
  void fields(){
   try {
    for(String name:new String[]{"saveFn","restoreFn"}){
     Field field=carrier.getClass().getDeclaredField(name);field.setAccessible(true);
     require(Modifier.isFinal(field.getModifiers())&&field.get(carrier)==(name.equals("saveFn")?save:restore));
    }
   }catch(ReflectiveOperationException error){throw new AssertionError(MARKER,error);}
  }
  public void construct(int phase,Object actualSave,Object actualRestore,Object result){
   require(actualSave==save&&actualRestore==restore&&forwardCalls==0&&reverseCalls==0);
   if(phase==0){require(log==0&&result==null&&carrier==null);hit(1);}
   else {require(phase==1&&log==1&&result instanceof Pair);carrier=result;fields();hit(2);}
  }
  public void operation(int phase,Object owner,Object actualContext,Object value,Object result){
   require(owner==carrier);
   if(phase==2||phase==3)require(actualContext==context&&value==input&&result==(phase==2?null:middle));
   else require((phase==4||phase==5)&&actualContext==null&&value==middle&&result==(phase==4?null:back));
   hit(phase+1);
  }
 }
 static final class Save implements Function2<Context,Object,Object> {
  final Observer state;Save(Observer state){this.state=state;}
  public Object invoke(Context context,Object value){
   require(context==state.context&&value==state.input);state.forwardCalls++;state.hit(7);
   if(state.mode==17)((Object[])value)[0]=this;
   return state.middle;
  }
 }
 static final class Restore implements Function1<Object,Object> {
  final Observer state;Restore(Observer state){this.state=state;}
  public Object invoke(Object value){
   require(value==state.middle);state.reverseCalls++;state.hit(8);
   if(state.mode==17)((Object[])value)[0]=this;
   return state.back;
  }
 }
 static Object create(int route,Object save,Object restore)throws Throwable {
  try {
   if(route==0)return PRIVATE.invoke(null,save,restore);
   if(route==1)return(Object)PRIVATE_HANDLE.invokeExact(save,restore);
   if(route==2)return PUBLIC.invoke(null,save,restore);
   return(Object)PUBLIC_HANDLE.invokeExact(save,restore);
  }catch(InvocationTargetException error){throw error.getCause();}
 }
 @SuppressWarnings("unchecked") public static void main(String[] args)throws Throwable {
  int route=Integer.parseInt(args[0]),mode=Integer.parseInt(args[1]);
  for(int attempt=0;attempt<2;attempt++){
   Observer state=new Observer(mode);state.save=mode==1?null:new Save(state);state.restore=mode==2?null:new Restore(state);Trace.probe=state;
   Object actual=null;Throwable caught=null;try{actual=create(route,state.save,state.restore);}catch(Throwable error){caught=error;}
   if(mode==9){require(actual==null&&caught==state.failure&&state.log==1&&state.carrier==null);continue;}
   if(mode==10){require(actual==null&&caught==state.failure&&state.log==12&&state.carrier!=null);state.failAt=0;}
   else require(caught==null&&actual==state.carrier&&state.carrier!=null&&state.log==12);
   state.fields();
   for(int round=0;round<(mode==19?2:1);round++){
    if(mode==19&&round==1){state.middle=new Token();state.back=new Token();}
    state.log=0;int left=state.forwardCalls,right=state.reverseCalls;Object saved=null,restored=null;caught=null;
    try{saved=((Parent<Object,Object>)state.carrier).save(state.context,state.input);require(saved==state.middle);restored=((Parent<Object,Object>)state.carrier).restore(saved);}catch(Throwable error){caught=error;}
    int stop=mode==1?3:mode==2?5:state.failAt;long wanted=0;boolean f=false,r=false;
    for(int event:new int[]{3,7,4,5,8,6}){wanted=wanted*10+event;f|=event==7;r|=event==8;if(event==stop)break;}
    require(state.log==wanted&&state.forwardCalls==left+(f?1:0)&&state.reverseCalls==right+(r?1:0));
    if(mode==1||mode==2||mode==15)require(caught instanceof NullPointerException);
    else if(stop!=0)require(caught==state.failure);
    else require(caught==null&&saved==state.middle&&restored==state.back);
    if(mode==17){require(((Object[])state.input)[0]==state.save&&((Object[])state.middle)[0]==state.restore);}
    state.fields();
   }
  }
  System.out.println("attempts=2,private-context-factory=matched");
 }
}
