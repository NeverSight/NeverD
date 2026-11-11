import fixture.*;
import java.util.Iterator;
import java.util.function.Supplier;
import java.io.IOException;
import java.lang.invoke.MethodHandles;
import java.lang.invoke.MethodType;
import java.lang.reflect.InvocationTargetException;
public final class DependentSeedBoundHarness {
    private static void require(boolean ok){if(!ok)throw new AssertionError("owned dependent seed bound");}
    private static final class Other implements Iterable<String>{public Iterator<String> iterator(){Trace.iterators++;throw new AssertionError("unexpected owned iteration");}}
    @SuppressWarnings("unchecked")
    private static DependentSeedBoundCaller.Box call(int route,int operation,DependentSeedBoundCaller.Box value,Object errors)throws Throwable {
        Supplier<RuntimeException> supplier=(Supplier<RuntimeException>)errors;
        if(route==0)return operation==0?Core.context(value):Core.throwing(value,supplier);
        if(route==1)return operation==0?DependentSeedBoundCaller.context(value):DependentSeedBoundCaller.throwing(value,supplier);
        if(route==2){try{return (DependentSeedBoundCaller.Box)(operation==0?Core.class.getMethod("context",Iterable.class).invoke(null,value):Core.class.getMethod("throwing",Iterable.class,Supplier.class).invoke(null,value,errors));}catch(InvocationTargetException failure){throw failure.getCause();}}
        if(operation==0)return (DependentSeedBoundCaller.Box)(Iterable)MethodHandles.publicLookup().findStatic(Core.class,"context",MethodType.methodType(Iterable.class,Iterable.class)).invokeExact((Iterable)value);
        return (DependentSeedBoundCaller.Box)(Iterable)MethodHandles.publicLookup().findStatic(Core.class,"throwing",MethodType.methodType(Iterable.class,Iterable.class,Supplier.class)).invokeExact((Iterable)value,supplier);
    }
    public static void main(String[] args)throws Throwable {
        final int route=Integer.parseInt(args[0]),operation=Integer.parseInt(args[1]),mode=Integer.parseInt(args[2]);
        final Throwable checked=new IOException("owned checked"),runtime=new IllegalArgumentException("owned runtime"),error=new Error("owned error"),callback=new IllegalArgumentException("owned callback");
        Trace.failure=new IllegalArgumentException("owned stage");Trace.reject=operation==1&&(mode>=2&&mode<=7||mode==10);Trace.failAt=mode==11?1:mode==10&&operation==0?2:0;
        Object[] previous=null;
        for(int attempt=0;attempt<2;attempt++) {
            final DependentSeedBoundCaller.Box value=mode==1?null:new DependentSeedBoundCaller.Box();
            final Iterable<?> desired=mode==8?new Other():mode==9?null:mode==0&&attempt==0?value:new DependentSeedBoundCaller.Box();
            final Object sentinel=new Object(),payload=new Object();final Object selected=mode==2?null:mode==3?new Object():mode==4?checked:mode==6?error:runtime;
            Supplier supplier=new Supplier(){public Object get(){Trace.calls++;Trace.hit(5);require(Trace.marker==payload&&Trace.value==value);if(mode==7)throw (RuntimeException)callback;return selected;}};
            final Object handler=mode==1||mode==10?null:supplier;
            Trace.events=0;Trace.value=new DependentSeedBoundCaller.Box();Trace.provider=sentinel;Trace.message=null;Trace.data=null;Trace.marker=sentinel;Trace.result=null;
            final int calls=Trace.calls,actions=Trace.actions,iterators=Trace.iterators;
            Trace.action=new Runnable(){public void run(){Trace.actions++;require(Trace.events==12&&Trace.value==value&&Trace.marker==sentinel);
                if(operation==0)require("owned-message".equals(Trace.message)&&Trace.data!=null&&Trace.data.length==0&&Trace.provider==sentinel);
                else require(Trace.provider==handler&&Trace.data==null&&Trace.message==null);
                Trace.marker=payload;Trace.result=desired;if(value!=null)value.marker++;
                if(operation==0&&mode==4)throw (RuntimeException)runtime;if(operation==0&&mode==5)throw (Error)error;
            }};
            DependentSeedBoundCaller.Box warm=new DependentSeedBoundCaller.Box(),result=warm;Throwable failure=null;try{result=call(route,operation,value,handler);}catch(Throwable caught){failure=caught;}
            boolean stage1=mode==11,stage2=mode==10&&operation==0,entered=!stage1&&!stage2;
            long events=stage1?1:stage2||mode==10||operation==0&&(mode==4||mode==5)?12:operation==1&&mode>=2&&mode<=7?125:1234;
            require(Trace.events==events&&Trace.actions==actions+(entered?1:0)&&Trace.calls==calls+(operation==1&&mode>=2&&mode<=7?1:0)&&Trace.iterators==iterators);
            if(operation==1&&(mode==2||mode==10))require(failure instanceof NullPointerException);
            else if(mode==8||operation==1&&mode==3)require(failure instanceof ClassCastException);
            else if(mode==4)require(failure==(operation==0?runtime:checked));
            else if(mode==5)require(failure==(operation==0?error:runtime));
            else if(operation==1&&(mode==6||mode==7))require(failure==(mode==6?error:callback));
            else require(failure==(stage1||stage2?Trace.failure:null));
            require(Trace.marker==(entered?payload:sentinel)&&Trace.result==(entered?desired:null));
            if(entered){require(Trace.value==value);if(operation==0){require(Trace.data!=previous);previous=Trace.data;}}else require(Trace.value!=value&&Trace.provider==sentinel&&Trace.data==null);
            if(value!=null)require(value.marker==(entered?1:0));require(result==(failure==null?desired:warm));
            System.out.println("route="+route+",operation="+operation+",mode="+mode+",attempt="+attempt+",trace="+Trace.events);
        }
    }
}
