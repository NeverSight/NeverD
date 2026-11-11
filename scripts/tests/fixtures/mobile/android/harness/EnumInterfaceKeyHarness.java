import fixture.Client;
import fixture.Provider;
import fixture.Source;
import fixture.View;
import fixture.Trace;
import java.lang.invoke.MethodHandles;
import java.lang.invoke.MethodType;
import java.lang.reflect.InvocationTargetException;
import java.util.concurrent.TimeUnit;
public final class EnumInterfaceKeyHarness {
    private static void require(boolean value){if(!value)throw new AssertionError("owned enum interface key");}
    @SuppressWarnings({"rawtypes","unchecked"})
    private static Enum call(int route,int operation,Provider receiver,Class type,Object key,Enum fallback)throws Throwable {
        if(route==0)return operation==0?Client.call(receiver,type,key):Client.fallback(receiver,type,key,fallback);
        if(route==1)return operation==0?EnumInterfaceKeyCaller.call(receiver,type,key):EnumInterfaceKeyCaller.fallback(receiver,type,key,fallback);
        if(route==2){try{return operation==0?(Enum)Client.class.getMethod("call",View.class,Class.class,Object.class).invoke(null,receiver,type,key):(Enum)Client.class.getMethod("fallback",Source.class,Class.class,Object.class,Enum.class).invoke(null,receiver,type,key,fallback);}catch(InvocationTargetException failure){throw failure.getCause();}}
        if(operation==0)return (Enum)MethodHandles.publicLookup().findStatic(Client.class,"call",MethodType.methodType(Enum.class,View.class,Class.class,Object.class)).invokeExact((View)receiver,type,key);
        return (Enum)MethodHandles.publicLookup().findStatic(Client.class,"fallback",MethodType.methodType(Enum.class,Source.class,Class.class,Object.class,Enum.class)).invokeExact((Source)receiver,type,key,fallback);
    }
    @SuppressWarnings("rawtypes")
    public static void main(String[] args)throws Throwable {
        final int route=Integer.parseInt(args[0]),operation=Integer.parseInt(args[1]),mode=Integer.parseInt(args[2]);
        final Provider receiver=mode==1?null:new Provider();
        final Class type=mode==2?null:mode==10?String.class:Thread.State.class;
        final Object[] cycle=new Object[1];cycle[0]=cycle;
        final Object key=mode==3?null:mode==11?cycle:new Object(){public String toString(){throw new AssertionError("owned enum interface key");}};
        final Enum<?> fallback=mode==11?null:TimeUnit.HOURS;
        final Enum<?> desired=mode==8?TimeUnit.SECONDS:mode==9?null:Thread.State.RUNNABLE;
        final RuntimeException actionFailure=new RuntimeException("owned action failure");final Error actionError=new Error("owned action error");
        Trace.failure=new RuntimeException("owned stage failure");Trace.failAt=mode==6?1:mode==7?2:0;
        for(int attempt=0;attempt<2;attempt++){
            final Object sentinel=new Object(),payload=new Object();Trace.events=0;Trace.receiver=sentinel;Trace.type=String.class;Trace.key=sentinel;Trace.fallback=TimeUnit.DAYS;Trace.payload=sentinel;Trace.result=TimeUnit.MINUTES;int calls=Trace.calls;
            Trace.action=new Runnable(){public void run(){require(Trace.events==1&&Trace.receiver==receiver&&Trace.type==type&&Trace.key==key&&Trace.fallback==(operation==0?null:fallback)&&Trace.payload==sentinel);Trace.hit(3);Trace.payload=payload;Trace.result=desired;if(mode==4)throw actionFailure;if(mode==5)throw actionError;}};
            Enum<?> actual=TimeUnit.MINUTES;int ordinal=42;Throwable failure=null;try{actual=call(route,operation,receiver,type,key,fallback);ordinal=((Thread.State)actual).ordinal();}catch(Throwable error){failure=error;}
            if(mode==1){require(failure instanceof NullPointerException&&actual==TimeUnit.MINUTES&&ordinal==42&&Trace.calls==calls&&Trace.events==0&&Trace.receiver==sentinel&&Trace.type==String.class&&Trace.key==sentinel&&Trace.fallback==TimeUnit.DAYS&&Trace.payload==sentinel);}
            else {
                require(Trace.calls==calls+1&&Trace.receiver==receiver&&Trace.type==type&&Trace.key==key&&Trace.fallback==(operation==0?null:fallback)&&Trace.events==(mode==6?1:mode==4||mode==5?13:132));
                Throwable expected=mode==4?actionFailure:mode==5?actionError:mode==6||mode==7?Trace.failure:null;
                if(expected!=null)require(failure==expected&&actual==TimeUnit.MINUTES&&ordinal==42);
                else if(mode==8)require(failure instanceof ClassCastException&&actual==desired&&ordinal==42);
                else if(mode==9)require(failure instanceof NullPointerException&&actual==null&&ordinal==42);
                else require(failure==null&&actual==desired&&ordinal==((Thread.State)desired).ordinal());
                require(Trace.payload==(mode==6?sentinel:payload));
            }
            System.out.println("route="+route+",operation="+operation+",mode="+mode+",attempt="+attempt+",trace="+Trace.events);
        }
    }
}
