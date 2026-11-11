import fixture.*;
import java.util.function.Supplier;
import java.io.IOException;
import java.lang.invoke.MethodHandles;
import java.lang.invoke.MethodType;
import java.lang.reflect.InvocationTargetException;
public final class ThrowableBoundSeedHarness {
    private static void require(boolean ok){if(!ok)throw new AssertionError("owned throwable bound seed");}
    @SuppressWarnings("unchecked")
    private static String call(int route,int operation,Object value,Object errors)throws Throwable {
        String typed=operation==0?(String)value:null;Supplier<RuntimeException> supplier=operation==0?(Supplier<RuntimeException>)errors:null;
        if(route==0)return operation==0?Core.call(typed,supplier):(String)Core.checked(value,errors);
        if(route==1)return operation==0?ThrowableBoundSeedCaller.call(typed,supplier):ThrowableBoundSeedCaller.checked(value,errors);
        if(route==2){try{return (String)(operation==0?Core.class.getMethod("call",CharSequence.class,Supplier.class).invoke(null,typed,errors):Core.class.getMethod("checked",Object.class,Object.class).invoke(null,value,errors));}catch(InvocationTargetException failure){throw failure.getCause();}}
        if(operation==0)return (String)(CharSequence)MethodHandles.publicLookup().findStatic(Core.class,"call",MethodType.methodType(CharSequence.class,CharSequence.class,Supplier.class)).invokeExact((CharSequence)typed,supplier);
        return (String)(CharSequence)MethodHandles.publicLookup().findStatic(Core.class,"checked",MethodType.methodType(CharSequence.class,Object.class,Object.class)).invokeExact(value,errors);
    }
    public static void main(String[] args)throws Throwable {
        final int route=Integer.parseInt(args[0]),operation=Integer.parseInt(args[1]),mode=Integer.parseInt(args[2]);
        final Throwable checked=new IOException("owned checked"),runtime=new IllegalArgumentException("owned runtime"),error=new Error("owned error"),callback=new IllegalArgumentException("owned callback");
        Trace.failure=new IllegalArgumentException("owned stage");Trace.reject=mode>=2&&mode<=7||mode==10&&operation==0;Trace.failAt=mode==11?1:0;
        for(int attempt=0;attempt<2;attempt++) {
            final CharSequence value=mode==1?null:operation==1&&mode==0&&attempt==1?new StringBuilder("first"):mode==9?new String(""):new String("first");
            final Object argument=mode==9&&operation==1?new Object():value;
            final CharSequence desired=mode==8?new StringBuilder("opaque-other-subtype"):mode==9?null:mode==0&&attempt==0?value:new String("opaque-result"+attempt);
            final Object sentinel=new Object(),payload=new Object();final Object selected=mode==2?null:mode==3?new Object():mode==4?checked:mode==6?error:runtime;
            Supplier supplier=new Supplier(){public Object get(){Trace.calls++;Trace.hit(5);require(Trace.marker==payload&&Trace.value==value);if(mode==7)throw (RuntimeException)callback;return selected;}};
            final Object handler=mode==1||mode==10&&operation==0?null:mode==11&&operation==1?new Object():supplier;
            Trace.events=0;Trace.value=new String("warm-value");Trace.provider=sentinel;Trace.marker=sentinel;Trace.result=null;final int calls=Trace.calls,actions=Trace.actions;
            Trace.action=new Runnable(){public void run(){Trace.actions++;require(Trace.events==12&&Trace.value==value&&Trace.provider==handler&&Trace.marker==sentinel);Trace.marker=payload;Trace.result=desired;if(value instanceof StringBuilder)((StringBuilder)value).append('!');if(mode==10&&operation==1)throw (RuntimeException)callback;}};
            String warm=new String("warm"),result=warm;Throwable failure=null;try{result=call(route,operation,argument,handler);}catch(Throwable caught){failure=caught;}
            final boolean bad=mode==9&&operation==1,entered=!bad&&mode!=11;
            long events=bad||mode==11?1:mode>=2&&mode<=7?125:mode==10?12:1234;
            require(Trace.events==events&&Trace.actions==actions+(entered?1:0)&&Trace.calls==calls+(mode>=2&&mode<=7?1:0));
            if(mode==2||mode==10&&operation==0)require(failure instanceof NullPointerException);
            else if(mode==3||mode==8||bad)require(failure instanceof ClassCastException);
            else if(mode==10)require(failure==callback);
            else if(mode>=4&&mode<=7)require(failure==(mode==4?checked:mode==6?error:mode==7?callback:runtime));
            else require(failure==(mode==11?Trace.failure:null));
            require(Trace.marker==(entered?payload:sentinel)&&Trace.result==(entered?desired:null)&&Trace.provider==(entered?handler:sentinel));
            if(entered)require(Trace.value==value);else require(Trace.value!=value);
            if(value instanceof StringBuilder)require(value.toString().equals(entered?"first!":"first"));
            require(result==(failure==null?desired:warm));
            System.out.println("route="+route+",operation="+operation+",mode="+mode+",attempt="+attempt+",trace="+Trace.events);
        }
    }
}
