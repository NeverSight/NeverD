import fixture.*;
import java.util.function.Supplier;
import java.io.IOException;
import java.lang.invoke.MethodHandles;
import java.lang.invoke.MethodType;
import java.lang.reflect.InvocationTargetException;
public final class ThrowableArrayCarrierHarness {
    private static void require(boolean ok){if(!ok)throw new AssertionError("owned throwable array carrier");}
    @SuppressWarnings("unchecked")
    private static String[] call(int route,int operation,Object values,Object errors)throws Throwable {
        String[] typed=operation==0?(String[])values:null;Supplier<RuntimeException> supplier=operation==0?(Supplier<RuntimeException>)errors:null;
        if(route==0)return operation==0?Core.call(typed,supplier):(String[])Core.checked(values,errors);
        if(route==1)return operation==0?ThrowableArrayCarrierCaller.call(typed,supplier):ThrowableArrayCarrierCaller.checked(values,errors);
        if(route==2){try{return (String[])(operation==0?Core.class.getMethod("call",Object[].class,Supplier.class).invoke(null,typed,errors):Core.class.getMethod("checked",Object.class,Object.class).invoke(null,values,errors));}catch(InvocationTargetException failure){throw failure.getCause();}}
        if(operation==0)return (String[])(Object[])MethodHandles.publicLookup().findStatic(Core.class,"call",MethodType.methodType(Object[].class,Object[].class,Supplier.class)).invokeExact((Object[])typed,supplier);
        return (String[])(Object[])MethodHandles.publicLookup().findStatic(Core.class,"checked",MethodType.methodType(Object[].class,Object.class,Object.class)).invokeExact(values,errors);
    }
    public static void main(String[] args)throws Throwable {
        final int route=Integer.parseInt(args[0]),operation=Integer.parseInt(args[1]),mode=Integer.parseInt(args[2]);
        final Throwable checked=new IOException("owned checked"),runtime=new IllegalArgumentException("owned runtime"),error=new Error("owned error"),callback=new IllegalArgumentException("owned callback");
        Trace.failure=new IllegalArgumentException("owned stage");Trace.reject=mode>=2&&mode<=7||mode==10&&operation==0;Trace.failAt=mode==11?1:0;
        for(int attempt=0;attempt<2;attempt++) {
            final String first=new String("first"),replacement=new String("replacement");final String[] values=mode==1?null:mode==9?new String[0]:new String[]{first};
            final Object argument=mode==9&&operation==1?new Object():values;
            final Object[] desired=mode==8?new Object[]{new Object()}:mode==9?null:mode==0&&attempt==0?values:new String[]{new String("opaque-result"+attempt)};
            final Object sentinel=new Object(),payload=new Object();final Object selected=mode==2?null:mode==3?new Object():mode==4?checked:mode==6?error:runtime;
            Supplier supplier=new Supplier(){public Object get(){Trace.calls++;Trace.hit(5);require(Trace.marker==payload&&Trace.values==values);if(mode==7)throw (RuntimeException)callback;return selected;}};
            final Object handler=mode==1||mode==10&&operation==0?null:mode==11&&operation==1?new Object():supplier;
            Trace.events=0;Trace.values=new Object[0];Trace.provider=sentinel;Trace.marker=sentinel;Trace.result=null;final int calls=Trace.calls,actions=Trace.actions;
            Trace.action=new Runnable(){public void run(){Trace.actions++;require(Trace.events==12&&Trace.values==values&&Trace.provider==handler&&Trace.marker==sentinel);Trace.marker=payload;Trace.result=desired;if(values!=null&&values.length!=0)Trace.values[0]=mode==10&&operation==1?payload:replacement;}};
            String[] warm={"warm"},result=warm;Throwable failure=null;try{result=call(route,operation,argument,handler);}catch(Throwable caught){failure=caught;}
            final boolean bad=mode==9&&operation==1,entered=!bad&&mode!=11;
            long events=bad||mode==11?1:mode>=2&&mode<=7?125:mode==10?12:1234;
            require(Trace.events==events&&Trace.actions==actions+(entered?1:0)&&Trace.calls==calls+(mode>=2&&mode<=7?1:0));
            if(mode==2||mode==10&&operation==0)require(failure instanceof NullPointerException);
            else if(mode==3||mode==8||bad)require(failure instanceof ClassCastException);
            else if(mode==10)require(failure instanceof ArrayStoreException);
            else if(mode>=4&&mode<=7)require(failure==(mode==4?checked:mode==6?error:mode==7?callback:runtime));
            else require(failure==(mode==11?Trace.failure:null));
            require(Trace.marker==(entered?payload:sentinel)&&Trace.result==(entered?desired:null)&&Trace.provider==(entered?handler:sentinel));
            if(entered)require(Trace.values==values);else require(Trace.values!=values);
            if(values!=null&&values.length!=0)require(values[0]==(entered&&!(mode==10&&operation==1)?replacement:first));
            require(result==(failure==null?desired:warm));
            System.out.println("route="+route+",operation="+operation+",mode="+mode+",attempt="+attempt+",trace="+Trace.events);
        }
    }
}
