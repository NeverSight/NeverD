import fixture.*;
import kotlin.jvm.functions.Function1;
import java.lang.invoke.*;
import java.lang.reflect.*;

public final class ScopedCallbackCarrierHarness {
 static final String MARKER="owned scoped callback carrier";
 static void require(boolean ok){if(!ok)throw new AssertionError(MARKER);}
 static final class Token {
  public int hashCode(){throw new AssertionError(MARKER);}
  public String toString(){throw new AssertionError(MARKER);}
 }
 static final class Checked extends Exception {}
 @SuppressWarnings("unchecked") static <E extends Throwable> void fail(Throwable error)throws E{throw(E)error;}
 static final Constructor<?> CTOR;
 static final Field SEED;
 static final Method DIRECT,DEFAULT;
 static final MethodHandle DIRECT_HANDLE,DEFAULT_HANDLE;
 static {
  try {
   Class<?> core=Class.forName("fixture.Core");
   CTOR=core.getDeclaredConstructor(Object.class);CTOR.setAccessible(true);
   SEED=core.getDeclaredField("seed");SEED.setAccessible(true);require(Modifier.isFinal(SEED.getModifiers()));
   DIRECT=core.getDeclaredMethod("convert",Function1.class,int.class);DIRECT.setAccessible(true);
   DEFAULT=core.getDeclaredMethod("convert$default",core,Function1.class,int.class,int.class,Object.class);DEFAULT.setAccessible(true);
   DIRECT_HANDLE=MethodHandles.lookup().unreflect(DIRECT).asType(MethodType.methodType(Object.class,Object.class,Object.class,int.class));
   DEFAULT_HANDLE=MethodHandles.lookup().unreflect(DEFAULT).asType(MethodType.methodType(Object.class,Object.class,Object.class,int.class,int.class,Object.class));
  }catch(Exception error){throw new ExceptionInInitializerError(error);}
 }
 static final class State implements Point {
  final int mode,limit,failPhase;final Object seed,output;final Throwable failure,mapperFailure;
  Object receiver,leaked;Mapping mapper;int phases;
  State(int mode,Object seed,Object output,int limit){
   this.mode=mode;this.seed=seed;this.output=output;this.limit=limit;
   failPhase=mode==10||mode==14?0:mode==11||mode==12?1:-1;
   failure=mode==11||mode==14?new Checked():mode==12?new AssertionError("failure"):new IllegalStateException("failure");
   mapperFailure=mode==7?new Checked():mode==8?new AssertionError("mapper"):new IllegalArgumentException("mapper");
  }
  public void visit(int phase,Object owner,Object actualMapper,Object actualSeed,int actualLimit,Object result){
   require(phase==phases++&&owner==receiver&&actualMapper==mapper&&actualSeed==seed&&actualLimit==limit);
   try{require(SEED.get(owner)==seed);}catch(IllegalAccessException error){throw new AssertionError(MARKER,error);}
   if(phase==0)require(result==null);
   else {
    require(phase==1&&mapper.calls==1&&result instanceof Box);leaked=result;
    require(((Box<?>)result).item==output&&((Box<?>)result).limit==limit);
    if(mode==19)((Object[])output)[0]=this;
   }
   if(phase==failPhase)ScopedCallbackCarrierHarness.<RuntimeException>fail(failure);
  }
 }
 static final class Mapping implements Function1<Object,Object> {
  final State state;int calls;
  Mapping(State state){this.state=state;}
  public Object invoke(Object value){
   require(state.phases==1&&calls++==0&&value==state.seed);
   if(state.mode==5)((Object[])value)[0]=this;
   if(state.mode==7||state.mode==8||state.mode==13)ScopedCallbackCarrierHarness.<RuntimeException>fail(state.mapperFailure);
   if(state.mode==9)throw(RuntimeException)null;
   return state.output;
  }
 }
 static Object call(int route,Object owner,Object mapper,int limit,int mask,Object marker)throws Throwable {
  try {
   if(route==0)return DEFAULT.invoke(null,owner,mapper,limit,mask,marker);
   if(route==1)return(Object)DEFAULT_HANDLE.invokeExact(owner,mapper,limit,mask,marker);
   if(route==2)return DIRECT.invoke(owner,mapper,limit);
   return(Object)DIRECT_HANDLE.invokeExact(owner,mapper,limit);
  }catch(InvocationTargetException error){throw error.getCause();}
 }
 public static void main(String[] args)throws Throwable {
  int route=Integer.parseInt(args[0]),mode=Integer.parseInt(args[1]);
  int[] masks={0,1,2,3,4,7,Integer.MIN_VALUE,Integer.MAX_VALUE,-1,64};
  for(int attempt=0;attempt<2;attempt++)for(int mask:masks){
   Object seed=mode==1?null:mode==4?new int[]{1,2}:mode==5?new Object[]{new Token()}:new Token();
   Object output=mode==2?null:mode==3?seed:mode==15?new String("result"):mode==19?new Object[]{new Token()}:new Token();
   int supplied=mode==4?Integer.MIN_VALUE:mode==19?Integer.MAX_VALUE:19;
   State state=new State(mode,seed,output,route<2&&(mask&2)!=0?Integer.MIN_VALUE:supplied);
   state.mapper=mode==6||mode==17?null:new Mapping(state);
   Trace.probe=null;state.receiver=mode==16||mode==17?null:CTOR.newInstance(seed);Trace.probe=state;
   Object actual=null;Throwable caught=null;
   try{actual=call(route,state.receiver,state.mapper,supplied,mask,mode==18?new Token():null);}catch(Throwable error){caught=error;}
   boolean missing=mode==16||mode==17,mapperFailure=mode==7||mode==8||mode==9||mode==13;
   int expected=missing?0:state.failPhase==0||state.mapper==null||mapperFailure?1:2;
   require(state.phases==expected);
   require(state.mapper==null||state.mapper.calls==(missing||state.failPhase==0?0:1));
   if(missing||state.mapper==null||mode==9){require(actual==null&&caught instanceof NullPointerException);}
   else if(state.failPhase>=0){require(actual==null&&caught==state.failure);}
   else if(mapperFailure){require(actual==null&&caught==state.mapperFailure);}
   else {require(caught==null&&actual==state.leaked&&actual instanceof Box);require(((Box<?>)actual).item==output&&((Box<?>)actual).limit==state.limit);}
   require((expected==2)==(state.leaked!=null));
   if(mode==5)require(((Object[])seed)[0]==state.mapper);
   if(mode==19)require(((Object[])output)[0]==state);
  }
  System.out.println("attempts=2,masks=10,scoped-callback-carrier=matched");
 }
}
