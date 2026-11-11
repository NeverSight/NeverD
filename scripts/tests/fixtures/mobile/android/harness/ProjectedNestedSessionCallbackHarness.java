import fixture.*;
import java.lang.reflect.*;
import java.util.concurrent.atomic.AtomicReference;
import kotlin.coroutines.*;
import kotlin.coroutines.intrinsics.IntrinsicsKt;
import kotlin.jvm.functions.*;
public final class ProjectedNestedSessionCallbackHarness {
    static int mode,attempt,producerCalls,blockCalls,firstCalls,lastCalls;
    static Object element,token,producerContext,blockElement;
    static Continuation blockFrame;
    static Throwable failure,frameFailure;
    static void require(boolean value){if(!value)throw new AssertionError("owned projected nested session callback");}
    @SuppressWarnings("unchecked") static <E extends Throwable> void raise(Throwable error)throws E{throw (E)error;}
    static class Seed {public int hashCode(){throw new AssertionError("unexpected element hash");}public String toString(){throw new AssertionError("unexpected element text");}}
    static final class Wrong {public int hashCode(){throw new AssertionError("unexpected raw state hash");}}
    static final class GuardedContext extends Context {public int hashCode(){throw new AssertionError("unexpected context hash");}}
    static final class Frame implements Continuation<Object> {
        int resumes;Object resumed;
        public CoroutineContext getContext(){throw new AssertionError("unexpected frame context");}
        public void resumeWith(Object value){resumes++;resumed=value;if(mode==9&&attempt==1)ProjectedNestedSessionCallbackHarness.<RuntimeException>raise(frameFailure);}
    }
    static final class Hook implements Runnable {
        final int stage;Hook(int stage){this.stage=stage;}
        public void run(){if(stage==1)firstCalls++;else lastCalls++;if(stage==3&&mode==7)ProjectedNestedSessionCallbackHarness.<RuntimeException>raise(failure);}
    }
    static final class Producer implements Function1<Context,Object> {
        public Object invoke(Context context){require(Trace.phase==1);producerCalls++;producerContext=context;if(mode==5)ProjectedNestedSessionCallbackHarness.<RuntimeException>raise(failure);return element;}
        public int hashCode(){throw new AssertionError("unexpected producer hash");}
    }
    static Object block(Object value,Continuation frame){require(Trace.phase==2);blockCalls++;blockElement=value;blockFrame=frame;if(mode==6)ProjectedNestedSessionCallbackHarness.<RuntimeException>raise(failure);if(mode==11)throw (RuntimeException)null;if(mode==9)frame.resumeWith(value);return mode==8?IntrinsicsKt.getCOROUTINE_SUSPENDED():token;}
    static final class Raw implements Function2<Object,Continuation,Object>{public Object invoke(Object value,Continuation frame){return block(value,frame);}public int hashCode(){throw new AssertionError("unexpected block hash");}}
    static final class Typed implements Function2<Seed,Continuation,Object>{public Object invoke(Seed value,Continuation frame){return block(value,frame);}}
    static Type lower(Type type){WildcardType w=(WildcardType)type;require(w.getLowerBounds().length==1&&w.getUpperBounds()[0]==Object.class);return w.getLowerBounds()[0];}
    static Type upper(Type type){WildcardType w=(WildcardType)type;require(w.getLowerBounds().length==0&&w.getUpperBounds().length==1);return w.getUpperBounds()[0];}
    static Type[] arguments(Type type,Class<?> raw,int count){ParameterizedType p=(ParameterizedType)type;require(p.getRawType()==raw&&p.getActualTypeArguments().length==count);return p.getActualTypeArguments();}
    static Method metadata(String form)throws Exception{
        Method m=Carrier.class.getDeclaredMethod("process",AtomicReference.class,Function1.class,Function2.class,Continuation.class);require(Modifier.isPublic(m.getModifiers())&&Modifier.isStatic(m.getModifiers())&&!m.isSynthetic()&&!m.isBridge());TypeVariable<Method>[] own=m.getTypeParameters();TypeVariable<?>[] cls=Carrier.class.getTypeParameters();require(own.length==1&&cls.length==1&&own[0].getGenericDeclaration().equals(m)&&own[0].getBounds()[0]==Object.class&&cls[0].getGenericDeclaration()==Carrier.class&&!own[0].equals(cls[0]));
        Type value=form.equals("input")?cls[0]:Object.class;Type[] params=m.getGenericParameterTypes();Type slot=arguments(params[0],AtomicReference.class,1)[0];ParameterizedType nested=(ParameterizedType)slot;Class<?> raw=(Class<?>)nested.getRawType();require(raw.getDeclaringClass()==Carrier.class&&Modifier.isPrivate(raw.getModifiers())&&Modifier.isStatic(raw.getModifiers())&&nested.getActualTypeArguments()[0].equals(value));require(raw.getTypeParameters().length==1&&!raw.getTypeParameters()[0].equals(cls[0])&&!raw.getTypeParameters()[0].equals(own[0]));
        Type[] producer=arguments(params[1],Function1.class,2);require(lower(producer[0])==Context.class&&upper(producer[1]).equals(value));Type[] action=arguments(params[2],Function2.class,3);require(lower(action[0]).equals(value)&&upper(action[2])==Object.class);Type nestedFrame=lower(action[1]);require(nestedFrame.equals(params[3])&&lower(arguments(params[3],Continuation.class,1)[0]).equals(own[0])&&m.getGenericReturnType()==Object.class);
        boolean encoded=form.equals("input");require(Carrier.class.getDeclaredConstructor(AtomicReference.class).isSynthetic()==encoded&&Carrier.class.getDeclaredMethod("wrap",AtomicReference.class).isSynthetic()==encoded&&Carrier.class.getDeclaredMethod("unwrap").isSynthetic()==encoded);return m;
    }
    @SuppressWarnings({"rawtypes","unchecked"}) public static void main(String[] args)throws Throwable{
        int route=Integer.parseInt(args[0]),kind=Integer.parseInt(args[1]);mode=Integer.parseInt(args[2]);Method method=metadata(args[3]);Throwable previous=null;
        for(attempt=0;attempt<2;attempt++){
            producerCalls=blockCalls=firstCalls=lastCalls=0;producerContext=blockElement=null;blockFrame=null;Trace.calls=Trace.phase=0;Trace.seenState=null;Trace.seenFrame=null;Trace.produced=Trace.returned=null;
            failure=attempt==1?new Exception("owned checked failure"):mode==6?new AssertionError("owned exact error"):new IllegalArgumentException("owned exact runtime");frameFailure=new Exception("owned frame failure");element=mode==10?new Wrong():new Seed();token=new Object();Trace.context=mode==12?null:new GuardedContext();Trace.first=new Hook(1);Trace.last=new Hook(3);
            AtomicReference state=mode==1?null:new AtomicReference(mode==13?new Wrong():null);Object stateValue=state==null?null:state.get();Frame originalFrame=new Frame();Continuation frame=mode==4?null:originalFrame;Function1 producer=mode==2?null:new Producer();Function2 action=mode==3?null:kind==0?new Raw():new Typed();require(Carrier.wrap(state).unwrap()==state);
            Object result=null;Throwable error=null;try{if(route==0)result=Client.send(state,producer,action,frame);else if(route==1)result=Carrier.process(state,producer,action,frame);else try{result=method.invoke(null,state,producer,action,frame);}catch(InvocationTargetException e){throw e.getCause();}}catch(Throwable e){error=e;}
            boolean noProducer=mode==2,producerFailure=mode==5,bridge=kind==1&&mode==10,noBlock=mode==3||bridge,frameError=mode==9&&attempt==1,blockFailure=mode==6||mode==11||frameError;
            int phase=noProducer||producerFailure?1:noBlock||blockFailure?2:3;require(Trace.calls==phase&&Trace.phase==phase&&Trace.seenState==state&&Trace.seenFrame==frame&&firstCalls==1&&lastCalls==(phase==3?1:0));require(producerCalls==(noProducer?0:1));if(!noProducer)require(producerContext==Trace.context);require(Trace.produced==(phase>=2?element:null));boolean called=phase>=2&&!noBlock;require(blockCalls==(called?1:0));if(called)require(blockElement==element&&blockFrame==frame);else require(blockElement==null&&blockFrame==null);
            Object output=mode==8?IntrinsicsKt.getCOROUTINE_SUSPENDED():token;require(Trace.returned==(phase==3?output:null));require(originalFrame.resumes==(mode==9?1:0));if(mode==9)require(originalFrame.resumed==element);if(state!=null)require(state.get()==stateValue);
            if(noProducer||mode==3||mode==11){require(error!=null&&error.getClass()==NullPointerException.class&&error!=previous&&result==null);}else if(producerFailure||mode==6||mode==7)require(error==failure&&result==null);else if(bridge)require(error!=null&&error.getClass()==ClassCastException.class&&result==null);else if(frameError)require(error==frameFailure&&result==null);else require(error==null&&result==output);previous=error;
        }
        System.out.println("attempts=2,projected-nested-session-callback=matched");
    }
}
