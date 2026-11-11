import fixture.Client;
import fixture.Trace;
import java.lang.invoke.MethodHandles;
import java.lang.invoke.MethodType;
import java.lang.reflect.InvocationTargetException;
import java.util.concurrent.TimeUnit;
public final class EnumClosedContextHarness {
    private static void require(boolean value){if(!value)throw new AssertionError("owned enum closed context");}
    @SuppressWarnings({"rawtypes","unchecked"})
    private static Enum call(int route,int operation,Class type,String text)throws Throwable {
        if(route==0)return operation==0?Client.call(type,text):Client.lookup(type,text);
        if(route==1)return operation==0?EnumClosedContextCaller.call(type,text):EnumClosedContextCaller.lookup(type,text);
        String name=operation==0?"call":"lookup";
        if(route==2){try{return (Enum)Client.class.getMethod(name,Class.class,String.class).invoke(null,type,text);}catch(InvocationTargetException failure){throw failure.getCause();}}
        return (Enum)MethodHandles.publicLookup().findStatic(Client.class,name,MethodType.methodType(Enum.class,Class.class,String.class)).invokeExact(type,text);
    }
    @SuppressWarnings("rawtypes")
    public static void main(String[] args)throws Throwable {
        final int route=Integer.parseInt(args[0]),operation=Integer.parseInt(args[1]),mode=Integer.parseInt(args[2]);
        if(operation==1){
            final Class type=mode==1?null:mode==2||mode==8?TimeUnit.class:mode==9?String.class:Thread.State.class;
            final String[] names={new String("RUNNABLE"),"RUNNABLE",new String("SECONDS"),"absent",null,""," RUNNABLE","runnable","HOURS","x","RUNNABLE\0","é"};
            final Enum<?> desired=mode==0?Thread.State.RUNNABLE:mode==2?TimeUnit.SECONDS:mode==8?TimeUnit.HOURS:null;
            for(int attempt=0;attempt<2;attempt++){
                int calls=Trace.calls;Trace.events=0;Trace.action=new Runnable(){public void run(){throw new AssertionError("owned enum closed context");}};
                Enum<?> actual=null;Throwable failure=null;try{actual=call(route,operation,type,names[mode]);}catch(Throwable error){failure=error;}
                require(Trace.calls==calls&&Trace.events==0);
                if(desired!=null)require(failure==null&&actual==desired);
                else require(actual==null&&failure!=null&&failure.getClass()==(mode==1||mode==4?NullPointerException.class:IllegalArgumentException.class));
                System.out.println("route="+route+",operation="+operation+",mode="+mode+",attempt="+attempt+",trace="+Trace.events);
            }
            return;
        }
        final String text=mode==3?null:mode==11?"":new String(" owned-\0-é-"+mode+" ");
        final Class type=mode==1?null:mode==2?TimeUnit.class:mode==10?String.class:Thread.State.class;
        final Enum<?> desired=mode==8?TimeUnit.SECONDS:mode==9?null:Thread.State.RUNNABLE;
        final RuntimeException actionFailure=new RuntimeException("owned action failure");final Error actionError=new Error("owned action error");
        Trace.failure=new RuntimeException("owned stage failure");Trace.failAt=mode==6?1:mode==7?2:0;
        for(int attempt=0;attempt<2;attempt++) {
            final Object sentinel=new Object(),payload=new Object();Trace.events=0;Trace.type=null;Trace.text="sentinel";Trace.payload=sentinel;Trace.result=TimeUnit.HOURS;int calls=Trace.calls;
            Trace.action=new Runnable(){public void run(){require(Trace.events==1&&Trace.type==type&&Trace.text==text&&Trace.payload==sentinel);Trace.hit(3);Trace.payload=payload;Trace.result=desired;if(mode==4)throw actionFailure;if(mode==5)throw actionError;}};
            Enum<?> actual=TimeUnit.HOURS;int ordinal=42;Throwable failure=null;try{actual=call(route,operation,type,text);ordinal=((Thread.State)actual).ordinal();}catch(Throwable error){failure=error;}
            require(Trace.calls==calls+1&&Trace.type==type&&Trace.text==text&&Trace.events==(mode==6?1:mode==4||mode==5?13:132));
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
