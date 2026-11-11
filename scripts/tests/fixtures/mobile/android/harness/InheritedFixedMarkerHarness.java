import fixture.*;
import kotlin.jvm.internal.DefaultConstructorMarker;
import java.lang.reflect.*;
import java.lang.invoke.*;
public final class InheritedFixedMarkerHarness {
    static int calls;
    static void require(boolean ok){if(!ok)throw new AssertionError("owned inherited fixed marker");}
    static Child make(int route,Value value,long bits,boolean enabled,Sink sink,DefaultConstructorMarker marker) throws Throwable {
        if(route==0)return Client.make(value,bits,enabled,sink,marker);
        if(route==1)return InheritedFixedMarkerCaller.make(value,bits,enabled,sink,marker);
        if(route==2){try{return Child.class.getConstructor(Value.class,long.class,boolean.class,Sink.class,DefaultConstructorMarker.class).newInstance(value,bits,enabled,sink,marker);}catch(InvocationTargetException f){throw f.getCause();}}
        return (Child)MethodHandles.publicLookup().findConstructor(Child.class,MethodType.methodType(void.class,Value.class,long.class,boolean.class,Sink.class,DefaultConstructorMarker.class)).invokeExact(value,bits,enabled,sink,marker);
    }
    public static void main(String[] args) throws Throwable {
        int route=Integer.parseInt(args[0]),mode=Integer.parseInt(args[1]);boolean synthetic=Boolean.parseBoolean(args[2]);
        require(Modifier.isAbstract(Core.class.getModifiers()) && !Modifier.isFinal(Core.class.getModifiers()));
        require(Core.class.getSuperclass()==Base.class && Core.class.getTypeParameters().length==0 && Core.class.getInterfaces().length==1 && Core.class.getInterfaces()[0]==Tagged.class);
        Constructor<?> target=Core.class.getDeclaredConstructor(Value.class,long.class,boolean.class,Sink.class);
        require(Modifier.isPrivate(target.getModifiers()) && target.getGenericParameterTypes()[0]==Value.class);
        Type sinkType=target.getGenericParameterTypes()[3];
        require(sinkType instanceof ParameterizedType && ((ParameterizedType)sinkType).getRawType()==Sink.class && ((ParameterizedType)sinkType).getActualTypeArguments()[0]==Value.class);
        require(Core.class.getConstructor(Value.class,long.class,boolean.class,Sink.class,DefaultConstructorMarker.class).isSynthetic()==synthetic);
        int[] fail={0,0,0,0,1,2,4,3,5,0,0,6,8,9};
        for(int attempt=0;attempt<2;attempt++){
            Value token=mode==1?null:new Value();long bits=mode==9?Long.MIN_VALUE:mode==10?Long.MAX_VALUE:0x1234567887654321L+attempt;
            boolean enabled=(mode+attempt)%2!=0;
            Trace.base=null;Trace.partial=null;Trace.events=0;Trace.failAt=fail[mode];Trace.failure=new RuntimeException("owned constructor failure");calls=0;
            Sink sink=mode==2?null:new Sink<Value>() { public void accept(Value value){calls++;require(value==token && Trace.partial instanceof Child);Child c=(Child)Trace.partial;require(c.value==token && c.bits==bits && c.enabled==enabled && c.stamp==0 && c.sink==this && c.baseStamp==42 && Trace.base==c);Trace.hit(6);} };
            DefaultConstructorMarker marker=null;
            if(mode==3){Constructor<DefaultConstructorMarker> c=DefaultConstructorMarker.class.getDeclaredConstructor();c.setAccessible(true);marker=c.newInstance();}
            Child result=null;Throwable failure=null;try{result=make(route,token,bits,enabled,sink,marker);}catch(Throwable f){failure=f;}
            if(mode==2)require(failure instanceof NullPointerException);else if(fail[mode]!=0)require(failure==Trace.failure);else require(failure==null && result==Trace.partial);
            long expected=mode==12?8:mode==13?89:mode==4?891:mode==5?8912:mode==2||mode==6?89124:mode==11?891246:mode==7?8912463:89124635;
            require(Trace.events==expected && calls==((mode==2||mode==4||mode==5||mode==6||mode>=12)?0:1));
            require(Trace.base instanceof Child);Child c=(Child)Trace.base;
            require(c.baseStamp==(mode==12?0:42));
            require(Trace.partial==(mode>=12?null:c));
            boolean assigned=mode!=4&&mode<12;
            require(c.value==(assigned?token:null) && c.bits==(assigned?bits:0) && c.enabled==(assigned&&enabled) && c.sink==(assigned?sink:null));
            require(c.stamp==((mode==0||mode==1||mode==3||mode==8||mode==9||mode==10)?bits:0));
            System.out.println("route="+route+",mode="+mode+",attempt="+attempt+",trace="+expected);
        }
    }
}
