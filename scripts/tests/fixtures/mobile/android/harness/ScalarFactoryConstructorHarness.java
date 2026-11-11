import fixture.*;
import java.util.*;
import java.util.function.Function;
import java.lang.reflect.*;
import java.lang.invoke.*;
@SuppressWarnings({"rawtypes","unchecked"})
public final class ScalarFactoryConstructorHarness {
    static void require(boolean ok) { if(!ok)throw new AssertionError("owned scalar factory constructor"); }
    static final class OpaqueMap extends AbstractMap {
        Object value;int reads;
        public Object get(Object key) { require(key==Trace.key);reads++;return value; }
        public Set entrySet() { throw new AssertionError("owned scalar factory constructor traversed map"); }
    }
    static final class Callback implements Function<String,String> {
        int calls;Object received;
        public String apply(String value) { Trace.hit(6);calls++;received=value;return (String)Trace.callbackResult; }
    }
    static Target create(int route,int entry,int capacity,boolean flag,Function fn) throws Throwable {
        if(route==0) {
            if(entry==0)return new Target(flag,fn);
            if(entry==1)return new Target(capacity,flag,fn);
            return new Target(fn);
        }
        if(route==1)return ScalarFactoryConstructorCaller.create(entry,capacity,flag,fn);
        if(route==2) {
            try {
                if(entry==0)return (Target)Target.class.getConstructor(boolean.class,Function.class).newInstance(flag,fn);
                if(entry==1)return (Target)Target.class.getConstructor(int.class,boolean.class,Function.class).newInstance(capacity,flag,fn);
                return (Target)Target.class.getConstructor(Function.class).newInstance(fn);
            } catch(InvocationTargetException error) { throw error.getCause(); }
        }
        MethodHandles.Lookup lookup=MethodHandles.publicLookup();
        if(entry==0)return (Target)lookup.findConstructor(Target.class,MethodType.methodType(void.class,boolean.class,Function.class)).invokeExact(flag,fn);
        if(entry==1)return (Target)lookup.findConstructor(Target.class,MethodType.methodType(void.class,int.class,boolean.class,Function.class)).invokeExact(capacity,flag,fn);
        return (Target)lookup.findConstructor(Target.class,MethodType.methodType(void.class,Function.class)).invokeExact(fn);
    }
    public static void main(String[] args) throws Throwable {
        int route=Integer.parseInt(args[0]),operation=Integer.parseInt(args[1]),mode=Integer.parseInt(args[2]),entry=mode%3,capacity=17;boolean flag=(mode&1)!=0;
        Method factory=Box.class.getMethod("create",boolean.class);TypeVariable<?>[] own=factory.getTypeParameters(),owner=Box.class.getTypeParameters();
        require(own.length==2&&owner.length==2&&!own[0].equals(owner[0])&&!own[1].equals(owner[1]));
        require(Arrays.equals(((ParameterizedType)factory.getGenericReturnType()).getActualTypeArguments(),own));
        Trace.failAt=mode>=1&&mode<=6?mode:0;Trace.failure=new RuntimeException("owned factory constructor failure");
        OpaqueMap map=new OpaqueMap();map.value=mode==10?new Object():mode==12?null:new String("owned value");
        Box box=new Box(mode==11?null:map);Trace.result=mode==8?null:mode==9?new Object():box;Trace.key=new Object();
        Trace.callbackResult=mode==10?new Object():mode==12?null:new String("owned callback");
        Callback callback=new Callback();Function fn=mode==7?null:callback;String input=new String("owned argument");Object sentinel=new Object();
        boolean expectedFlag=entry==2?false:flag;int expectedCapacity=entry==1?capacity:37;
        for(int attempt=0;attempt<2;attempt++) {
            Trace.events=0;Trace.partial=Trace.box=sentinel;Trace.capacitySeen=-7;Trace.flagSeen=!expectedFlag;
            int reads=map.reads,calls=callback.calls;Object received=callback.received;
            Target target=null;Object actual=null;Throwable failure=null;
            try { target=create(route,entry,capacity,flag,fn);actual=operation==0?target:operation==1?ScalarFactoryConstructorCaller.value(target):ScalarFactoryConstructorCaller.apply(target,input); }
            catch(Throwable error) { failure=error; }
            boolean constructed=!(mode>=1&&mode<=5)&&mode!=9;
            long expected=mode==1?1:mode==2||mode==9?12:mode==3?123:mode==4?1234:12345;
            if(constructed&&operation==2&&fn!=null)expected=123456;
            require(Trace.events==expected&&Trace.capacitySeen==(mode==1?-7:expectedCapacity)&&Trace.flagSeen==(mode==1?!expectedFlag:expectedFlag));
            boolean entered=mode!=1&&mode!=2&&mode!=3&&mode!=9;
            require((Trace.partial!=sentinel)==entered&&Trace.box==(entered?Trace.result:sentinel));
            if(entered) { Target partial=(Target)Trace.partial;require(partial.box==Trace.result&&partial.mapper==(mode==4?null:fn)); }
            if(constructed)require(target==Trace.partial&&target.box==Trace.result&&target.mapper==fn);
            require(map.reads==reads+(constructed&&operation==1&&mode!=8&&mode!=11?1:0));
            boolean called=constructed&&operation==2&&fn!=null&&mode!=6;
            require(callback.calls==calls+(called?1:0)&&callback.received==(called?input:received));
            if(mode>=1&&mode<=5||mode==6&&operation==2)require(failure==Trace.failure);
            else if(mode==9||constructed&&operation!=0&&mode==10)require(failure instanceof ClassCastException);
            else if(constructed&&operation==1&&(mode==8||mode==11)||constructed&&operation==2&&fn==null)require(failure instanceof NullPointerException);
            else require(failure==null&&actual==(operation==0?target:operation==1?map.value:Trace.callbackResult));
            System.out.println("route="+route+",operation="+operation+",mode="+mode+",attempt="+attempt+",trace="+Trace.events);
        }
    }
}
