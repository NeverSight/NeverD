import fixture.*;
import java.lang.invoke.*;
import java.lang.reflect.*;
@SuppressWarnings({"rawtypes","unchecked"})
public final class PrivateSeededResultHarness {
    static void require(boolean ok) { if(!ok)throw new AssertionError("owned private seeded result"); }
    static final class Consumer implements Callback {
        final boolean fail;Consumer(boolean fail){this.fail=fail;}
        public Object apply(Object seed) {
            Trace.callbacks++;Trace.seenCallback=seed;Trace.hit(8);
            if(fail)throw Trace.failure;
            return Trace.callbackValue;
        }
    }
    static final class Producer implements Factory<Object> {
        final boolean fail;Producer(boolean fail){this.fail=fail;}
        public Object get() {
            Trace.productions++;Trace.hit(9);
            if(fail)throw Trace.failure;
            Object result=Trace.value;Trace.value=Trace.next;return result;
        }
    }
    static Object call(int route,int kind,Core peer,Object seed,Callback callback,Factory producer) throws Throwable {
        if(route==0){if(kind==0)return Core.checked(peer,seed,callback,producer);if(kind==1)return Core.raw(peer,seed,callback,producer);Core.discard(peer,seed,callback,producer);return null;}
        if(route==1)return PrivateSeededResultCaller.call(kind,peer,seed,callback,producer);
        String name=kind==0?"checked":kind==1?"raw":"discard";
        if(route==2)try{return Core.class.getMethod(name,Core.class,Object.class,Callback.class,Factory.class).invoke(null,peer,seed,callback,producer);}catch(InvocationTargetException e){throw e.getCause();}
        MethodHandle handle=MethodHandles.publicLookup().findStatic(Core.class,name,MethodType.methodType(kind==0?Value.class:kind==1?Object.class:void.class,Core.class,Object.class,Callback.class,Factory.class));
        if(kind==0)return (Value)handle.invokeExact(peer,seed,callback,producer);
        if(kind==1)return (Object)handle.invokeExact(peer,seed,callback,producer);
        handle.invokeExact(peer,seed,callback,producer);return null;
    }
    public static void main(String[] args) throws Throwable {
        int route=Integer.parseInt(args[0]),mode=Integer.parseInt(args[1]);boolean isStatic=Boolean.parseBoolean(args[2]);
        Method method=Core.class.getDeclaredMethod("read",Object.class,Callback.class,Factory.class);
        require(Modifier.isPrivate(method.getModifiers())&&Modifier.isFinal(method.getModifiers())&&Modifier.isStatic(method.getModifiers())==isStatic&&!method.isBridge()&&!method.isSynthetic());
        TypeVariable<Method>[] vars=method.getTypeParameters();require(vars.length==2&&vars[0].getName().equals("R")&&vars[1].getName().equals("S")&&vars[0].getBounds()[0]==Object.class&&vars[1].getBounds()[0]==Object.class);
        Type[] params=method.getGenericParameterTypes();require(params[0].equals(vars[1])&&method.getGenericReturnType().equals(vars[0]));
        Type[] cb=((ParameterizedType)params[1]).getActualTypeArguments(),factory=((ParameterizedType)params[2]).getActualTypeArguments();
        require(((ParameterizedType)params[1]).getRawType()==Callback.class&&((ParameterizedType)params[2]).getRawType()==Factory.class&&cb[1]==Token.class);
        require(cb[0] instanceof WildcardType&&((WildcardType)cb[0]).getLowerBounds()[0].equals(vars[1])&&factory[0] instanceof WildcardType&&((WildcardType)factory[0]).getUpperBounds()[0].equals(vars[0]));
        RuntimeException failure=new IllegalArgumentException("owned private seeded failure");
        for(int attempt=0;attempt<2;attempt++) {
            Value value=new Value(-31-attempt);Object seed=mode==19?null:new Value(17+attempt);
            Object produced=mode==13||mode==16||mode==17?new Object():mode==14?null:mode==18?seed:value;
            Core peer=mode==10?null:new Core();Callback callback=mode==11?null:new Consumer(mode==8);Factory producer=mode==12?null:new Producer(mode==9);
            Object sentinel=new Object(),next=new Object(),extra=new Object();Token token=mode==19?null:new Token();
            Trace.events=0;Trace.failAt=mode>=1&&mode<=7?mode:0;Trace.callbacks=0;Trace.productions=0;Trace.failure=failure;
            Trace.seenReceiver=sentinel;Trace.seenSeed=sentinel;Trace.seenCallback=sentinel;Trace.saved=sentinel;Trace.token=null;
            Trace.value=produced;Trace.next=next;Trace.extra=extra;Trace.callbackValue=mode==15?new Object():token;
            Trace.observer=event->{if(event==3){require(Trace.saved==sentinel);Trace.value=extra;}};
            int kind=mode==16?1:mode==17?2:0;Object result=sentinel;Throwable caught=null;
            try{result=call(route,kind,peer,seed,callback,producer);}catch(Throwable e){caught=e;}
            int[] full={5,1,8,2,9,3,4,6,7};long events=0;boolean entered=false,called=false,read=false,stored=false,callbackDone=false,mutated=false;
            for(int event:full) {
                events=events*10+event;
                if(event==1)entered=true;if(event==8)called=true;if(event==2)callbackDone=true;if(event==9)read=true;if(event==3)mutated=true;if(event==4)stored=true;
                if(event==Trace.failAt||event==5&&mode==10&&!isStatic||event==1&&mode==11||event==2&&mode==12||event==8&&(mode==8||mode==15)||event==9&&mode==9||event==6&&mode==13)break;
            }
            require(Trace.events==events&&Trace.callbacks==(called?1:0)&&Trace.productions==(read?1:0));
            if(mode>=1&&mode<=9)require(caught==failure);
            else if(mode==10&&!isStatic||mode==11||mode==12)require(caught instanceof NullPointerException);
            else if(mode==13||mode==15)require(caught instanceof ClassCastException);
            else require(caught==null&&result==(kind==2?null:produced));
            require(Trace.seenReceiver==(entered?(isStatic?null:peer):sentinel)&&Trace.seenSeed==(entered?seed:sentinel));
            require(Trace.seenCallback==(called?seed:sentinel)&&Trace.saved==(stored?produced:sentinel));
            require(Trace.token==(callbackDone?token:null));
            require(Trace.value==(mutated?extra:read&&mode!=9?next:produced));
            System.out.println("route="+route+",mode="+mode+",attempt="+attempt+",trace="+Trace.events);
        }
    }
}
