import fixture.Client;
import fixture.Trace;
import java.lang.invoke.MethodHandles;
import java.lang.invoke.MethodType;
import java.lang.reflect.InvocationTargetException;
import java.util.concurrent.TimeUnit;
public final class EnumTokenScalarHarness {
    private static void require(boolean value){if(!value)throw new AssertionError("owned enum token scalar");}
    @SuppressWarnings({"rawtypes","unchecked"})
    private static Enum call(int route,Class type,int index)throws Throwable {
        if(route==0)return Client.call(type,index);if(route==1)return EnumTokenScalarCaller.call(type,index);
        if(route==2){try{return (Enum)Client.class.getMethod("call",Class.class,int.class).invoke(null,type,index);}catch(InvocationTargetException failure){throw failure.getCause();}}
        return (Enum)MethodHandles.publicLookup().findStatic(Client.class,"call",MethodType.methodType(Enum.class,Class.class,int.class)).invokeExact(type,index);
    }
    @SuppressWarnings("rawtypes")
    public static void main(String[] args)throws Throwable {
        final int route=Integer.parseInt(args[0]),operation=Integer.parseInt(args[1]),mode=Integer.parseInt(args[2]),index=operation==0?Integer.MIN_VALUE+mode:Integer.MAX_VALUE-mode;
        final Class type=mode==1?null:mode==2?TimeUnit.class:mode==10?String.class:Thread.State.class;
        final Enum<?> desired=mode==8?TimeUnit.SECONDS:mode==9?null:Thread.State.RUNNABLE;
        final RuntimeException actionFailure=new RuntimeException("owned action failure");final Error actionError=new Error("owned action error");
        Trace.failure=new RuntimeException("owned stage failure");Trace.failAt=mode==6?1:mode==7?2:0;
        for(int attempt=0;attempt<2;attempt++) {
            final Object sentinel=new Object(),payload=new Object();Trace.events=0;Trace.type=null;Trace.index=42;Trace.payload=sentinel;Trace.result=TimeUnit.HOURS;int calls=Trace.calls;
            Trace.action=new Runnable(){public void run(){require(Trace.events==1&&Trace.type==type&&Trace.index==index&&Trace.payload==sentinel);Trace.hit(3);Trace.payload=payload;Trace.result=desired;if(mode==4)throw actionFailure;if(mode==5)throw actionError;}};
            Enum<?> actual=TimeUnit.HOURS;int ordinal=42;Throwable failure=null;try{actual=call(route,type,index);ordinal=((Thread.State)actual).ordinal();}catch(Throwable error){failure=error;}
            require(Trace.calls==calls+1&&Trace.type==type&&Trace.index==index&&Trace.events==(mode==6?1:mode==4||mode==5?13:132));
            Throwable expected=mode==4?actionFailure:mode==5?actionError:mode==6||mode==7?Trace.failure:null;
            if(expected!=null)require(failure==expected&&actual==TimeUnit.HOURS&&ordinal==42);
            else if(mode==8)require(failure instanceof ClassCastException&&actual==desired&&ordinal==42);
            else if(mode==9)require(failure instanceof NullPointerException&&actual==null&&ordinal==42);
            else require(failure==null&&actual==desired&&ordinal==((Thread.State)desired).ordinal());
            require(Trace.payload==(mode==6?sentinel:payload));
            System.out.println("route="+route+",operation="+operation+",mode="+mode+",attempt="+attempt+",trace="+Trace.events);
        }
    }
}
