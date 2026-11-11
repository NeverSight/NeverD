import fixture.Client;
import fixture.Trace;
import java.lang.invoke.MethodHandles;
import java.lang.invoke.MethodType;
import java.lang.reflect.InvocationTargetException;
import java.util.concurrent.TimeUnit;
public final class EnumTokenDefaultHarness {
    private static void require(boolean value) {if(!value)throw new AssertionError("owned enum token default");}
    private static final class Trap {
        public boolean equals(Object value){throw new AssertionError("owned unexpected equality");}
        public int hashCode(){throw new AssertionError("owned unexpected hash");}
        public String toString(){throw new AssertionError("owned unexpected text");}
    }
    @SuppressWarnings({"rawtypes","unchecked"})
    private static Enum call(int route,int kind,Class type,Object value,Enum fallback) throws Throwable {
        if(route==0)return kind==0?Client.defaults(type,value):Client.explicit(type,value,fallback);
        if(route==1)return kind==0?EnumTokenDefaultCaller.defaults(type,value):EnumTokenDefaultCaller.explicit(type,value,fallback);
        if(route==2){try{return (Enum)(kind==0?Client.class.getMethod("defaults",Class.class,Object.class).invoke(null,type,value):Client.class.getMethod("explicit",Class.class,Object.class,Enum.class).invoke(null,type,value,fallback));}catch(InvocationTargetException failure){throw failure.getCause();}}
        if(kind==0)return (Enum)MethodHandles.publicLookup().findStatic(Client.class,"defaults",MethodType.methodType(Enum.class,Class.class,Object.class)).invokeExact(type,value);
        return (Enum)MethodHandles.publicLookup().findStatic(Client.class,"explicit",MethodType.methodType(Enum.class,Class.class,Object.class,Enum.class)).invokeExact(type,value,fallback);
    }
    @SuppressWarnings("rawtypes")
    public static void main(String[] args) throws Throwable {
        final int route=Integer.parseInt(args[0]),kind=Integer.parseInt(args[1]),mode=Integer.parseInt(args[2]);
        final Class type=mode==1?null:mode==2?TimeUnit.class:Thread.State.class;
        final Enum fallback=kind==2?(mode==10?TimeUnit.DAYS:Thread.State.NEW):null;
        final Object value=mode==3?fallback:new Trap(),payload=new Object();
        final Enum<?> desired=mode==8?TimeUnit.SECONDS:mode==9?null:mode==11?fallback:Thread.State.RUNNABLE;
        final RuntimeException actionFailure=new RuntimeException("owned action failure");final Error actionError=new Error("owned action error");
        Trace.failure=new RuntimeException("owned stage failure");Trace.failAt=mode==6?1:mode==7?2:0;
        for(int attempt=0;attempt<2;attempt++) {
            final Object sentinel=new Object();Trace.events=0;Trace.type=null;Trace.value=Trace.payload=sentinel;Trace.fallback=Trace.result=TimeUnit.HOURS;int calls=Trace.calls;
            Trace.action=new Runnable(){public void run(){
                require(Trace.events==1&&Trace.type==type&&Trace.value==value&&Trace.fallback==fallback&&Trace.payload==sentinel);Trace.hit(3);Trace.payload=payload;Trace.result=desired;
                if(mode==4)throw actionFailure;if(mode==5)throw actionError;
            }};
            Enum<?> actual=TimeUnit.HOURS;int ordinal=42;Throwable failure=null;try{actual=call(route,kind,type,value,fallback);ordinal=((Thread.State)actual).ordinal();}catch(Throwable error){failure=error;}
            require(Trace.calls==calls+1&&Trace.type==type&&Trace.value==value&&Trace.fallback==fallback&&Trace.events==(mode==6?1:mode==4||mode==5?13:132));
            Throwable expected=mode==4?actionFailure:mode==5?actionError:mode==6||mode==7?Trace.failure:null;
            if(expected!=null)require(failure==expected&&actual==TimeUnit.HOURS&&ordinal==42);
            else if(mode==8)require(failure instanceof ClassCastException&&actual==desired&&ordinal==42);
            else if(desired==null)require(failure instanceof NullPointerException&&actual==null&&ordinal==42);
            else require(failure==null&&actual==desired&&ordinal==((Thread.State)desired).ordinal());
            require(Trace.payload==(mode==6?sentinel:payload));
            System.out.println("route="+route+",operation="+kind+",mode="+mode+",attempt="+attempt+",trace="+Trace.events);
        }
    }
}
