import fixture.*;
import java.lang.invoke.MethodHandles;
import java.lang.invoke.MethodType;
import java.lang.reflect.InvocationTargetException;
public final class InstanceArrayTransferHarness {
    private static void require(boolean ok){if(!ok)throw new AssertionError("owned instance array transfer");}
    private static final class Override extends Derived<String> {
        public <T> T[] transfer(T[] values){Trace.dispatches++;return super.transfer(values);}
    }
    private static String[] call(int route,int operation,Derived<?> receiver,Object values)throws Throwable {
        String[] typed=operation==0?(String[])values:null;
        if(route==0)return operation==0?Core.call(receiver,typed):(String[])Core.checked(receiver,values);
        if(route==1)return operation==0?InstanceArrayTransferCaller.call(receiver,typed):InstanceArrayTransferCaller.checked(receiver,values);
        if(route==2){try{return (String[])(operation==0?Core.class.getMethod("call",Derived.class,Object[].class).invoke(null,receiver,typed):Core.class.getMethod("checked",Derived.class,Object.class).invoke(null,receiver,values));}catch(InvocationTargetException failure){throw failure.getCause();}}
        if(operation==0)return (String[])(Object[])MethodHandles.publicLookup().findStatic(Core.class,"call",MethodType.methodType(Object[].class,Derived.class,Object[].class)).invokeExact((Derived)receiver,(Object[])typed);
        return (String[])(Object[])MethodHandles.publicLookup().findStatic(Core.class,"checked",MethodType.methodType(Object[].class,Derived.class,Object.class)).invokeExact((Derived)receiver,values);
    }
    public static void main(String[] args)throws Throwable {
        final int route=Integer.parseInt(args[0]),operation=Integer.parseInt(args[1]),mode=Integer.parseInt(args[2]);
        final RuntimeException runtime=new IllegalArgumentException("owned callback");final Error error=new Error("owned error");
        Trace.failure=new IllegalArgumentException("owned stage");Trace.failAt=mode==9&&operation==0?2:mode==10?1:mode==11?3:0;
        for(int attempt=0;attempt<2;attempt++) {
            final String first=new String("first"),replacement=new String("replacement");
            final String[] values=mode==1?null:mode==2?new String[0]:new String[]{first};
            final Object argument=mode==9&&operation==1?new Object():values;
            final Derived<?> receiver=mode==12?null:attempt==0?new Derived<Integer>():new Override();
            final Object[] desired=mode==7?new Object[]{new Object()}:mode==8?null:new String[]{new String("opaque-result"+attempt)};
            final Object sentinel=new Object(),payload=new Object();final String[] warm={"warm"};
            Trace.events=0;Trace.input=new Object[0];Trace.receiver=null;Trace.marker=sentinel;Trace.result=null;final int calls=Trace.calls,actions=Trace.actions,dispatches=Trace.dispatches;
            Trace.action=new Runnable(){public void run(){Trace.actions++;require(Trace.events==12&&Trace.input==values&&Trace.receiver==receiver&&Thread.holdsLock(receiver)&&Trace.marker==sentinel);
                Trace.marker=payload;Trace.result=desired;
                if(values!=null&&values.length!=0)Trace.input[0]=mode==6?payload:replacement;
                if(mode==4)throw runtime;if(mode==5)throw error;
            }};
            String[] result=warm;Throwable failure=null;try{result=call(route,operation,receiver,argument);}catch(Throwable caught){failure=caught;}
            boolean bad=mode==9&&operation==1,stage1=mode==10,stage2=mode==9&&operation==0,invoked=!bad&&!stage1&&receiver!=null,entered=invoked&&!stage2;
            long events=!invoked?1:mode==4||mode==5||mode==6||stage2?12:mode==11?123:1234;
            require(Trace.events==events&&Trace.calls==calls+(invoked?1:0)&&Trace.actions==actions+(entered?1:0)&&Trace.dispatches==dispatches+(invoked&&attempt==1?1:0));
            if(bad||mode==7)require(failure instanceof ClassCastException);
            else if(mode==6)require(failure instanceof ArrayStoreException);
            else if(mode==12)require(failure instanceof NullPointerException);
            else require(failure==(mode==4?runtime:mode==5?error:stage1||stage2||mode==11?Trace.failure:null));
            require(Trace.marker==(entered?payload:sentinel)&&Trace.result==(entered?desired:null));
            if(entered)require(Trace.input==values&&Trace.receiver==receiver);
            else require(Trace.input!=values&&Trace.receiver==null);
            if(values!=null&&values.length!=0)require(values[0]==(!entered||mode==6?first:replacement));
            require(result==(failure==null?desired:warm));
            System.out.println("route="+route+",operation="+operation+",mode="+mode+",attempt="+attempt+",trace="+Trace.events);
        }
    }
}
