import fixture.*;
import kotlin.jvm.internal.DefaultConstructorMarker;
import java.lang.reflect.*;
import java.lang.invoke.*;
public final class InheritedInterfaceMarkerHarness {
    static int calls;
    static void require(boolean ok){if(!ok)throw new AssertionError("owned inherited interface marker");}
    static Core make(int route,Object value,long bits,Sink sink,DefaultConstructorMarker marker) throws Throwable {
        if(route==0)return Client.make(value,bits,sink,marker);
        if(route==1)return InheritedInterfaceMarkerCaller.make(value,bits,sink,marker);
        if(route==2){try{return Core.class.getConstructor(Object.class,long.class,Sink.class,DefaultConstructorMarker.class).newInstance(value,bits,sink,marker);}catch(InvocationTargetException f){throw f.getCause();}}
        return (Core)MethodHandles.publicLookup().findConstructor(Core.class,MethodType.methodType(void.class,Object.class,long.class,Sink.class,DefaultConstructorMarker.class)).invokeExact(value,bits,sink,marker);
    }
    public static void main(String[] args) throws Throwable {
        int route=Integer.parseInt(args[0]),mode=Integer.parseInt(args[1]);boolean synthetic=Boolean.parseBoolean(args[2]);
        require(!Modifier.isAbstract(Core.class.getModifiers()) && Modifier.isFinal(Core.class.getModifiers()));
        ParameterizedType edge=(ParameterizedType)Core.class.getGenericInterfaces()[0];require(edge.getRawType()==View.class && edge.getActualTypeArguments()[0].equals(Core.class.getTypeParameters()[0]));
        ParameterizedType parent=(ParameterizedType)View.class.getGenericInterfaces()[0];require(parent.getRawType()==Root.class && parent.getActualTypeArguments()[0].equals(View.class.getTypeParameters()[0]));
        Constructor<?> target=Core.class.getDeclaredConstructor(Object.class,long.class,Sink.class);
        require(Modifier.isPrivate(target.getModifiers()) && target.getGenericParameterTypes()[0].equals(Core.class.getTypeParameters()[0]));
        require(Core.class.getConstructor(Object.class,long.class,Sink.class,DefaultConstructorMarker.class).isSynthetic()==synthetic);
        int[] fail={0,0,0,0,1,2,4,3,5,0,0,6};
        for(int attempt=0;attempt<2;attempt++){
            Object token=mode==1?null:new Object();long bits=mode==9?Long.MIN_VALUE:mode==10?Long.MAX_VALUE:0x1234567887654321L+attempt;
            Trace.partial=Trace.parent=null;Trace.events=0;Trace.failAt=fail[mode];Trace.failure=new RuntimeException("owned constructor failure");calls=0;
            Sink sink=mode==2?null:new Sink<Object>() { public void accept(Object value){calls++;require(value==token && Trace.partial instanceof Core);Core c=(Core)Trace.partial;require(c.value==token && c.bits==bits && c.stamp==0 && c.sink==this);Trace.hit(6);} };
            DefaultConstructorMarker marker=null;
            if(mode==3){Constructor<DefaultConstructorMarker> c=DefaultConstructorMarker.class.getDeclaredConstructor();c.setAccessible(true);marker=c.newInstance();}
            Core result=null;Throwable failure=null;try{result=make(route,token,bits,sink,marker);}catch(Throwable f){failure=f;}
            if(mode==2)require(failure instanceof NullPointerException);else if(fail[mode]!=0)require(failure==Trace.failure);else require(failure==null && result==Trace.partial);
            long expected=mode==4?1:mode==5?12:mode==2||mode==6?124:mode==11?1246:mode==7?12463:124635;
            require(Trace.partial instanceof Root);
            require(Trace.events==expected && calls==((mode==2||mode==4||mode==5||mode==6)?0:1));
            Core c=(Core)Trace.partial;require(c!=null && c.value==(mode==4?null:token) && c.bits==(mode==4?0:bits) && c.sink==(mode==4?null:sink));
            require(c.stamp==((mode==2||mode==4||mode==5||mode==6||mode==7||mode==11)?0:bits));
            System.out.println("route="+route+",mode="+mode+",attempt="+attempt+",trace="+expected);
        }
    }
}
