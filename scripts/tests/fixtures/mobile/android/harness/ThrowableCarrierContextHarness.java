import fixture.*;
import java.util.function.Supplier;
import java.io.IOException;
import java.lang.invoke.MethodHandles;
import java.lang.invoke.MethodType;
import java.lang.reflect.InvocationTargetException;
public final class ThrowableCarrierContextHarness {
    private static void require(boolean ok){if(!ok)throw new AssertionError("owned throwable carrier context");}
    @SuppressWarnings("unchecked")
    private static void call(int route,int operation,Object left,Object right,Object errors)throws Throwable {
        Supplier<RuntimeException> typed=operation==0?(Supplier<RuntimeException>)errors:null;
        if(route==0){if(operation==0)Core.call(left,right,typed);else Core.checked(left,right,errors);return;}
        if(route==1){if(operation==0)ThrowableCarrierContextCaller.call(left,right,typed);else ThrowableCarrierContextCaller.checked(left,right,errors);return;}
        if(route==2){try{Core.class.getMethod(operation==0?"call":"checked",Object.class,Object.class,operation==0?Supplier.class:Object.class).invoke(null,left,right,errors);return;}catch(InvocationTargetException failure){throw failure.getCause();}}
        if(operation==0)MethodHandles.publicLookup().findStatic(Core.class,"call",MethodType.methodType(void.class,Object.class,Object.class,Supplier.class)).invokeExact(left,right,typed);
        else MethodHandles.publicLookup().findStatic(Core.class,"checked",MethodType.methodType(void.class,Object.class,Object.class,Object.class)).invokeExact(left,right,errors);
    }
    public static void main(String[] args)throws Throwable {
        final int route=Integer.parseInt(args[0]),operation=Integer.parseInt(args[1]),mode=Integer.parseInt(args[2]);
        final Throwable checked=new IOException("owned checked"),runtime=new IllegalArgumentException("owned runtime"),error=new Error("owned error"),callback=new IllegalArgumentException("owned callback");
        Trace.failure=new IllegalArgumentException("owned stage");Trace.reject=mode>=1&&mode<=7;Trace.failAt=mode==8&&operation==0?4:mode>=9?mode-8:0;
        for(int attempt=0;attempt<2;attempt++) {
            final Object sentinel=new Object(),payload=new Object(),rightPayload=new Object();final Object[] first={sentinel},second={sentinel};
            final Object left=attempt==1&&mode%3==1?null:first,right=attempt==0&&mode%3==0?left:attempt==1&&mode%3==2?null:second;
            final Object selected=mode==2?null:mode==3?new Object():mode==4?checked:mode==6?error:runtime;
            Supplier supplier=new Supplier(){public Object get(){Trace.calls++;Trace.hit(5);require(Trace.marker==payload);if(mode==7)throw (RuntimeException)callback;return selected;}};
            final Object argument=mode==1?null:mode==8&&operation==1?new Object():supplier;
            Trace.events=0;Trace.left=Trace.right=Trace.provider=Trace.marker=sentinel;final int calls=Trace.calls,actions=Trace.actions;
            Trace.action=new Runnable(){public void run(){Trace.actions++;require(Trace.events==12&&Trace.provider==argument&&Trace.left==left&&Trace.right==right);Trace.marker=payload;if(left!=null)((Object[])left)[0]=payload;if(right!=null)((Object[])right)[0]=rightPayload;}};
            boolean done=false;Throwable failure=null;try{call(route,operation,left,right,argument);done=true;}catch(Throwable caught){failure=caught;}
            final boolean bad=mode==8&&operation==1,entered=!bad&&mode!=9&&mode!=10;
            final long events=bad||mode==9?1:mode==1||mode==10?12:mode>=2&&mode<=7?125:mode==11?123:1234;
            require(Trace.events==events&&Trace.actions==actions+(entered?1:0)&&Trace.calls==calls+(mode>=2&&mode<=7?1:0));
            if(mode==1||mode==2)require(failure instanceof NullPointerException);
            else if(mode==3||bad)require(failure instanceof ClassCastException);
            else if(mode>=4&&mode<=7)require(failure==(mode==4?checked:mode==6?error:mode==7?callback:runtime));
            else if(mode>=8)require(failure==Trace.failure);else require(failure==null);
            require(done==(failure==null)&&Trace.provider==(entered?argument:sentinel)&&Trace.marker==(entered?payload:sentinel));
            require(Trace.left==(entered?left:sentinel)&&Trace.right==(entered?right:sentinel));
            require(first[0]==(!entered?sentinel:right==first?rightPayload:left==first?payload:sentinel));
            require(second[0]==(!entered?sentinel:right==second?rightPayload:left==second?payload:sentinel));
            System.out.println("route="+route+",operation="+operation+",mode="+mode+",attempt="+attempt+",trace="+Trace.events);
        }
    }
}
