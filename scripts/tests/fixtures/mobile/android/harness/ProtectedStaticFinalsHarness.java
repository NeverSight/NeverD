import fixture.Core;
import fixture.Trace;
import java.lang.invoke.MethodHandles;
import java.lang.invoke.MethodType;
import java.lang.reflect.InvocationTargetException;
public final class ProtectedStaticFinalsHarness {
    private static void require(boolean ok){if(!ok)throw new AssertionError("owned protected static finals");}
    private static Object call(int route,int operation)throws Throwable {
        if(route==0)return operation==0?Core.read():Core.touch();
        if(route==1)return operation==0?ProtectedStaticFinalsCaller.read():ProtectedStaticFinalsCaller.touch();
        String name=operation==0?"read":"touch";
        if(route==2){try{return Core.class.getMethod(name).invoke(null);}catch(InvocationTargetException failure){throw failure.getCause();}}
        return (Object)MethodHandles.publicLookup().findStatic(Core.class,name,MethodType.methodType(Object.class)).invokeExact();
    }
    public static void main(String[] args)throws Throwable {
        final int route=Integer.parseInt(args[0]),operation=Integer.parseInt(args[1]),mode=Integer.parseInt(args[2]);
        Trace.calls=Trace.stage=Trace.observed=0;Trace.events=0;Trace.results=new Object[]{mode==1?null:new Object(),mode==2?null:new Object(),mode==3?null:new Object()};
        Trace.checked=new NoSuchMethodException("owned checked");Trace.failAt=mode==7?1:mode==4?2:mode==8?3:0;
        final RuntimeException runtime=new IllegalArgumentException("owned runtime");final Error error=new Error("owned error");
        Trace.action=new Runnable(){public void run(){try{int index=Trace.stage;Trace.observed++;require(Trace.calls==index&&Trace.observed==index);require(Core.class.getField("FIRST").get(null)==(index>1?Trace.results[0]:null));require(Core.class.getField("SECOND").get(null)==(index>2?Trace.results[1]:null));require(Core.class.getField("THIRD").get(null)==null);if((mode==5&&index==2)||(mode==10&&index==1))throw runtime;if((mode==6&&index==2)||(mode==11&&index==3))throw error;if(mode==9&&index==1)Trace.action=null;}catch(ReflectiveOperationException failure){throw new AssertionError(failure);}}};
        final boolean failed=mode>=4;final int step=mode==7||mode==10?1:mode==8||mode==11?3:2;
        for(int attempt=0;attempt<2;attempt++) {
            Object actual=null;Throwable failure=null;try{actual=call(route,operation);}catch(Throwable caught){failure=caught;}
            require(Trace.calls==(failed?step:3)&&Trace.observed==(mode==9?1:failed?step:3));
            if(failed){require(actual==null);if(attempt==1)require(failure instanceof NoClassDefFoundError&&failure.getCause() instanceof ExceptionInInitializerError);
                else if(mode==6||mode==11)require(failure==error);
                else {require(failure instanceof ExceptionInInitializerError);Throwable cause=failure.getCause();if(mode==4||mode==7||mode==8)require(cause instanceof IllegalStateException&&cause.getCause()==Trace.checked&&cause.getMessage().equals("owned lookup"));else if(mode==5||mode==10)require(cause==runtime);else require(cause instanceof NullPointerException);}}
            else require(failure==null&&actual==Trace.results[operation==0?2:0]);
            long prefix=failed?step==1?1:step==2?12:123:123;long expected=failed?prefix:attempt==0?prefix*10+(operation==0?8:9):(prefix*10+(operation==0?8:9))*10+(operation==0?8:9);require(Trace.events==expected);
            System.out.println("route="+route+",operation="+operation+",mode="+mode+",attempt="+attempt+",trace="+Trace.events);
        }
    }
}
