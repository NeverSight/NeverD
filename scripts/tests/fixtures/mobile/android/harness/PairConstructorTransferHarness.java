import fixture.*;
import java.util.*;
import java.util.function.Function;
import java.lang.reflect.*;
import java.lang.invoke.*;
@SuppressWarnings({"rawtypes","unchecked"})
public final class PairConstructorTransferHarness {
    static void require(boolean ok) { if(!ok)throw new AssertionError("owned pair constructor transfer"); }
    static final class OpaqueMap extends AbstractMap {
        Object value;int reads;
        public Object get(Object key) { require(key==Trace.key);reads++;return value; }
        public Set entrySet() { throw new AssertionError("owned pair constructor transfer traversed map"); }
    }
    static final class Callback implements Function<String,String> {
        int calls;Object received;
        public String apply(String value) { Trace.hit(6);calls++;received=value;return (String)Trace.callbackResult; }
    }
    static Target create(int route,int capacity,float load,Function fn) throws Throwable {
        if(route==0)return new Target<String,String>(capacity,load,fn);
        if(route==1)return PairConstructorTransferCaller.create(capacity,load,fn);
        if(route==2) {
            try { return (Target)Target.class.getConstructor(int.class,float.class,Function.class).newInstance(capacity,load,fn); }
            catch(InvocationTargetException error) { throw error.getCause(); }
        }
        return (Target)MethodHandles.publicLookup().findConstructor(Target.class,MethodType.methodType(void.class,int.class,float.class,Function.class)).invokeExact(capacity,load,fn);
    }
    public static void main(String[] args) throws Throwable {
        int route=Integer.parseInt(args[0]),operation=Integer.parseInt(args[1]),mode=Integer.parseInt(args[2]);
        Method factory=Box.class.getMethod("wrap",Map.class);TypeVariable<?>[] own=factory.getTypeParameters(),owner=Box.class.getTypeParameters();
        require(own.length==2&&owner.length==2&&!own[0].equals(owner[0])&&!own[1].equals(owner[1]));
        require(Arrays.equals(((ParameterizedType)factory.getGenericParameterTypes()[0]).getActualTypeArguments(),own));
        require(Arrays.equals(((ParameterizedType)factory.getGenericReturnType()).getActualTypeArguments(),own));
        Trace.failAt=mode>=1&&mode<=6?mode:0;Trace.failure=new RuntimeException("owned constructor failure");
        int capacity=mode==7?-1:13;float load=mode==8?Float.NaN:mode==9?0f:0.65f;
        OpaqueMap map=new OpaqueMap();map.value=mode==13?new Object():mode==14?null:new String("owned value");
        Box box=new Box(map);Trace.result=mode==10?null:mode==11?new Object():box;Trace.key=new Object();
        Trace.callbackResult=mode==15?new Object():mode==16?null:new String("owned callback");
        Callback callback=new Callback();Function fn=mode==12?null:callback;String input=new String("owned argument");
        Object sentinel=new Object(),previousInput=sentinel;
        for(int attempt=0;attempt<2;attempt++) {
            Trace.events=0;Trace.input=Trace.partial=Trace.box=sentinel;
            int reads=map.reads,calls=callback.calls;Object received=callback.received;
            Target target=null;Object actual=null;Throwable failure=null;
            try { target=create(route,capacity,load,fn);actual=operation==0?target:operation==1?PairConstructorTransferCaller.value(target):PairConstructorTransferCaller.apply(target,input); }
            catch(Throwable error) { failure=error; }
            boolean allocationFailure=mode==7||mode==8||mode==9;
            boolean constructed=!allocationFailure&&!(mode>=1&&mode<=5)&&mode!=11;
            long expected=allocationFailure?0:mode==1?1:mode==2||mode==11?12:mode==3?123:mode==4?1234:12345;
            if(constructed&&operation==2&&fn!=null)expected=123456;
            require(Trace.events==expected);
            if(allocationFailure||mode==1)require(Trace.input==sentinel);
            else { require(Trace.input instanceof HashMap&&Trace.input!=previousInput&&Trace.input!=map);previousInput=Trace.input; }
            boolean entered=!allocationFailure&&mode!=1&&mode!=2&&mode!=3&&mode!=11;
            require((Trace.partial!=sentinel)==entered&&Trace.box==(entered?Trace.result:sentinel));
            if(entered) { Target partial=(Target)Trace.partial;require(partial.box==Trace.result&&partial.mapper==(mode==4?null:fn)); }
            if(constructed)require(target==Trace.partial&&target.box==Trace.result&&target.mapper==fn);
            require(map.reads==reads+(constructed&&operation==1&&mode!=10?1:0));
            boolean callbackDone=constructed&&operation==2&&fn!=null&&mode!=6;
            require(callback.calls==calls+(callbackDone?1:0)&&callback.received==(callbackDone?input:received));
            if(allocationFailure)require(failure instanceof IllegalArgumentException);
            else if(mode>=1&&mode<=5||mode==6&&operation==2&&fn!=null)require(failure==Trace.failure);
            else if(mode==11||constructed&&operation==1&&mode==13||constructed&&operation==2&&mode==15)require(failure instanceof ClassCastException);
            else if(constructed&&operation==1&&mode==10||constructed&&operation==2&&fn==null)require(failure instanceof NullPointerException);
            else require(failure==null&&actual==(operation==0?target:operation==1?map.value:Trace.callbackResult));
            System.out.println("route="+route+",operation="+operation+",mode="+mode+",attempt="+attempt+",trace="+Trace.events);
        }
    }
}
