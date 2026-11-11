import fixture.*;
import java.lang.reflect.*;
import java.lang.invoke.*;
public final class ProjectedInterfaceSuperHarness {
    static Sink seen;
    static int calls;
    static void require(boolean ok){if(!ok)throw new AssertionError("owned projected interface super");}
    static Receiver make(int route,Sink sink) throws Throwable {
        if(route==0)return Client.make(sink);
        if(route==1)return ProjectedInterfaceSuperCaller.make(sink);
        if(route==2){try{return Receiver.class.getConstructor(Sink.class).newInstance(sink);}catch(InvocationTargetException f){throw f.getCause();}}
        return (Receiver)MethodHandles.publicLookup().findConstructor(Receiver.class,MethodType.methodType(void.class,Sink.class)).invokeExact(sink);
    }
    public static void main(String[] args) throws Throwable {
        int route=Integer.parseInt(args[0]),mode=Integer.parseInt(args[1]);
        require(Receiver.class.getSuperclass()==Base.class && Receiver.class.getGenericInterfaces().length==1);
        Constructor<?> c=Receiver.class.getDeclaredConstructor(Sink.class);ParameterizedType p=(ParameterizedType)c.getGenericParameterTypes()[0];
        require(p.getRawType()==Sink.class && ((WildcardType)p.getActualTypeArguments()[0]).getLowerBounds()[0]==Receiver.class);
        int[] failures={0,0,1,2,6,3,4,0,0};
        for(int attempt=0;attempt<2;attempt++){
            Trace.partial=null;Trace.events=0;Trace.failAt=failures[mode];Trace.failure=new RuntimeException("owned parent failure");calls=0;seen=null;
            long wide=mode==7?Long.MIN_VALUE:mode==8?Long.MAX_VALUE:0x1234567887654321L+attempt;
            Sink sink=mode==1?null:new Sink<Object>(){public void accept(Object value){calls++;seen=this;require(value==Trace.partial && value instanceof Receiver);Receiver r=(Receiver)value;require(r.label==0 && r.sink==null);Trace.hit(2);require(r.apply(91,wide)==wide);}};
            Receiver result=null;Throwable failure=null;
            try{result=make(route,sink);}catch(Throwable f){failure=f;}
            if(mode==1)require(failure instanceof NullPointerException);else if(mode>=2&&mode<=6)require(failure==Trace.failure);else require(failure==null && result==Trace.partial);
            long expected=mode==1||mode==2?1:mode==3?12:mode==4?126:mode==5?1263:12634;
            require(Trace.events==expected && calls==((mode==1||mode==2)?0:1));
            Receiver partial=(Receiver)Trace.partial;require(partial!=null);
            require(partial.label==((mode>=1&&mode<=4)?0:2) && partial.sink==((mode>=1&&mode<=4)?null:sink));
            if(calls!=0)require(seen==sink);
            if(result!=null){Trace.events=0;Trace.failAt=0;Function raw=result;CarrierB b=(CarrierB)raw.invoke(new CarrierA(7),new CarrierB(wide));require(b.value==wide && Trace.events==1264);}
            System.out.println("route="+route+",mode="+mode+",attempt="+attempt+",trace="+expected);
        }
    }
}
