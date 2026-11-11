import fixture.*;
import kotlin.jvm.internal.DefaultConstructorMarker;
import java.lang.invoke.*;
import java.lang.reflect.*;
public final class OpenFixedMarkerDirectHarness {
    static void check(boolean ok) { if(!ok)throw new AssertionError("owned concrete fixed marker behavior"); }
    static Core make(int route,Value value,long bits,boolean enabled,Sink sink,DefaultConstructorMarker marker) throws Throwable {
        if(route==0)return new Core(value,bits,enabled,sink,marker);
        if(route==1)return InheritedFixedMarkerCaller.makeCore(value,bits,enabled,sink,marker);
        if(route==2)try {
            return Core.class.getConstructor(Value.class,long.class,boolean.class,Sink.class,DefaultConstructorMarker.class).newInstance(value,bits,enabled,sink,marker);
        } catch(InvocationTargetException failure) { throw failure.getCause(); }
        MethodHandle handle=MethodHandles.publicLookup().findConstructor(Core.class,MethodType.methodType(void.class,Value.class,long.class,boolean.class,Sink.class,DefaultConstructorMarker.class));
        return (Core)handle.invokeExact(value,bits,enabled,sink,marker);
    }
    public static void main(String[] args) throws Throwable {
        int route=Integer.parseInt(args[0]);boolean synthetic=Boolean.parseBoolean(args[1]);
        check(!Modifier.isAbstract(Core.class.getModifiers())&&!Modifier.isFinal(Core.class.getModifiers()));
        check(Core.class.getConstructor(Value.class,long.class,boolean.class,Sink.class,DefaultConstructorMarker.class).isSynthetic()==synthetic);
        for(int attempt=0;attempt<2;attempt++) {
            Value token=attempt==0?new Value():null;long bits=attempt==0?Long.MIN_VALUE:Long.MAX_VALUE;boolean enabled=attempt!=0;
            Trace.events=0;Trace.failAt=0;Trace.base=null;Trace.partial=null;int[] calls={0};
            Sink<Value> sink=new Sink<Value>() {
                public void accept(Value value) {
                    calls[0]++;Core core=(Core)Trace.partial;
                    check(value==token&&core!=null&&core.getClass()==Core.class&&Trace.base==core&&core.baseStamp==42);
                    check(core.value==token&&core.bits==bits&&core.enabled==enabled&&core.sink==this);
                    Trace.hit(6);
                }
            };
            DefaultConstructorMarker marker=null;
            if(attempt!=0) {
                Constructor<DefaultConstructorMarker> constructor=DefaultConstructorMarker.class.getDeclaredConstructor();
                constructor.setAccessible(true);marker=constructor.newInstance();
            }
            Core result=make(route,token,bits,enabled,sink,marker);
            check(result==Trace.partial&&result.current()==token&&calls[0]==1&&Trace.events==891263);
            System.out.println("direct-route="+route+",attempt="+attempt+",trace="+Trace.events);
        }
    }
}
