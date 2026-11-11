package fixture;
import kotlin.jvm.internal.DefaultConstructorMarker;
import java.lang.invoke.*;
import java.lang.reflect.*;
public final class FinalFixedMarkerHarness {
    static int calls;
    static void check(boolean ok) { if(!ok)throw new AssertionError("owned final fixed marker behavior"); }
    static Core make(int route,Value value,long bits,boolean enabled,Sink sink,DefaultConstructorMarker marker) throws Throwable {
        if(route==0)return Client.make(value,bits,enabled,sink,marker);
        if(route==1)return InheritedFixedMarkerCaller.make(value,bits,enabled,sink,marker);
        if(route==2)try {
            return Core.class.getConstructor(Value.class,long.class,boolean.class,Sink.class,DefaultConstructorMarker.class).newInstance(value,bits,enabled,sink,marker);
        } catch(InvocationTargetException failure) { throw failure.getCause(); }
        return (Core)MethodHandles.lookup().findConstructor(Core.class,MethodType.methodType(void.class,Value.class,long.class,boolean.class,Sink.class,DefaultConstructorMarker.class)).invokeExact(value,bits,enabled,sink,marker);
    }
    public static void main(String[] args) throws Throwable {
        int route=Integer.parseInt(args[0]),mode=Integer.parseInt(args[1]);boolean synthetic=Boolean.parseBoolean(args[2]);
        check(Modifier.isFinal(Core.class.getModifiers())&&!Modifier.isPublic(Core.class.getModifiers())&&!Modifier.isAbstract(Core.class.getModifiers()));
        check(Core.class.getSuperclass()==Base.class&&Core.class.getTypeParameters().length==0&&Core.class.getInterfaces()[0]==Tagged.class);
        Constructor<?> target=Core.class.getDeclaredConstructor(Value.class,long.class,boolean.class,Sink.class);
        check(Modifier.isPrivate(target.getModifiers()));
        Type sinkType=target.getGenericParameterTypes()[3];
        check(sinkType instanceof ParameterizedType&&((ParameterizedType)sinkType).getRawType()==Sink.class&&((ParameterizedType)sinkType).getActualTypeArguments()[0]==Value.class);
        check(Core.class.getConstructor(Value.class,long.class,boolean.class,Sink.class,DefaultConstructorMarker.class).isSynthetic()==synthetic);
        int[] failures={0,0,0,0,1,2,3,6,8,9,0,0,0,0};
        for(int attempt=0;attempt<2;attempt++) {
            Value token=mode==1?null:new Value();long bits=mode==10?Long.MIN_VALUE:mode==11?Long.MAX_VALUE:0x8123456789abcdefL+attempt;
            boolean enabled=(mode+attempt)%2!=0;
            Trace.base=null;Trace.partial=null;Trace.events=0;Trace.failAt=failures[mode];Trace.failure=new RuntimeException("owned constructor failure");calls=0;
            Sink sink=mode==2?null:new Sink<Value>() {
                public void accept(Value value) {
                    calls++;Core core=(Core)Trace.partial;
                    check(value==token&&core!=null&&core.getClass()==Core.class&&Trace.base==core&&core.baseStamp==42);
                    check(core.value==token&&core.bits==bits&&core.enabled==enabled&&core.sink==this);Trace.hit(6);
                }
            };
            DefaultConstructorMarker marker=null;
            if(mode==3) {
                Constructor<DefaultConstructorMarker> constructor=DefaultConstructorMarker.class.getDeclaredConstructor();
                constructor.setAccessible(true);marker=constructor.newInstance();
            }
            Core result=null;Throwable caught=null;
            try { result=make(route,token,bits,enabled,sink,marker); }catch(Throwable failure) { caught=failure; }
            if(mode==2)check(caught instanceof NullPointerException);
            else if(failures[mode]!=0)check(caught==Trace.failure);else check(caught==null&&result==Trace.partial&&result.current()==token);
            long expected=mode==8?8:mode==9?89:mode==4?891:mode==2||mode==5?8912:mode==7?89126:891263;
            check(Trace.events==expected&&calls==((mode==2||mode==4||mode==5||mode==8||mode==9)?0:1));
            check(Trace.base instanceof Core);Core partial=(Core)Trace.base;
            check(partial.baseStamp==(mode==8?0:42));
            check(Trace.partial==((mode==8||mode==9)?null:partial));
            boolean assigned=mode!=4&&mode!=8&&mode!=9;
            check(partial.value==(assigned?token:null)&&partial.bits==(assigned?bits:0)&&partial.enabled==(assigned&&enabled)&&partial.sink==(assigned?sink:null));
            System.out.println("route="+route+",mode="+mode+",attempt="+attempt+",trace="+expected);
        }
    }
}
