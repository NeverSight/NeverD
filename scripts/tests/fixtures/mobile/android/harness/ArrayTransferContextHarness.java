import fixture.*;
import java.lang.invoke.MethodHandles;
import java.lang.invoke.MethodType;
import java.lang.reflect.InvocationTargetException;
public final class ArrayTransferContextHarness {
    private static void require(boolean ok){if(!ok)throw new AssertionError("owned array transfer context");}
    private static String[] call(int route,int operation,Object values,String message,Object[] data)throws Throwable {
        String[] typed=operation==0?(String[])values:null;
        if(route==0)return operation==0?Core.call(typed):(String[])Core.checked(values,message,data);
        if(route==1)return operation==0?ArrayTransferContextCaller.call(typed):ArrayTransferContextCaller.checked(values,message,data);
        if(route==2){try{return (String[])(operation==0?Core.class.getMethod("call",Object[].class).invoke(null,(Object)typed):Core.class.getMethod("checked",Object.class,String.class,Object[].class).invoke(null,values,message,data));}catch(InvocationTargetException failure){throw failure.getCause();}}
        if(operation==0)return (String[])(Object[])MethodHandles.publicLookup().findStatic(Core.class,"call",MethodType.methodType(Object[].class,Object[].class)).invokeExact((Object[])typed);
        return (String[])(Object[])MethodHandles.publicLookup().findStatic(Core.class,"checked",MethodType.methodType(Object[].class,Object.class,String.class,Object[].class)).invokeExact(values,message,data);
    }
    public static void main(String[] args)throws Throwable {
        final int route=Integer.parseInt(args[0]),operation=Integer.parseInt(args[1]),mode=Integer.parseInt(args[2]);
        final RuntimeException runtime=new IllegalArgumentException("owned callback");final Error error=new Error("owned error");
        Trace.failure=new IllegalArgumentException("owned stage");Trace.failAt=mode==9&&operation==0?2:mode==10?1:mode==11?3:0;
        Object[] previous=null;
        for(int attempt=0;attempt<2;attempt++) {
            final String first=new String("first"),replacement=new String("replacement"),extra=new String("extra");
            final String[] values=mode==1?null:mode==2?new String[0]:new String[]{first};
            final Object argument=mode==9&&operation==1?new Object():values;
            final String message=mode==3?null:new String("owned-context");
            final Object[] data=mode==3?null:mode==2?new Object[0]:mode==6?new String[]{first}:mode==0&&attempt==0?values:new Object[]{first};
            final Object[] desired=mode==7?new Object[]{new Object()}:mode==8?null:new String[]{new String("opaque-result"+attempt)};
            final Object sentinel=new Object(),payload=new Object();final String[] warm={"warm"};
            Trace.events=0;Trace.input=new Object[0];Trace.data=null;Trace.message=null;Trace.marker=sentinel;Trace.result=null;final int calls=Trace.calls,actions=Trace.actions;
            Trace.action=new Runnable(){public void run(){Trace.actions++;require(Trace.events==12&&Trace.input==values&&Trace.marker==sentinel);
                if(operation==0)require("owned-message".equals(Trace.message)&&Trace.data!=null&&Trace.data.length==0&&Trace.data!=values);
                else require(Trace.message==message&&Trace.data==data);
                Trace.marker=payload;Trace.result=desired;
                if(values!=null&&values.length!=0)Trace.input[0]=mode==6&&operation==0?payload:replacement;
                if(operation==1&&data!=null&&data.length!=0)data[0]=data==values?extra:payload;
                if(mode==4)throw runtime;if(mode==5)throw error;
            }};
            String[] result=warm;Throwable failure=null;try{result=call(route,operation,argument,message,data);}catch(Throwable caught){failure=caught;}
            boolean bad=mode==9&&operation==1,stage1=mode==10,stage2=mode==9&&operation==0,entered=!bad&&!stage1&&!stage2;
            long events=bad||stage1?1:mode==4||mode==5||mode==6||stage2?12:mode==11?123:1234;
            require(Trace.events==events&&Trace.calls==calls+(!bad&&!stage1?1:0)&&Trace.actions==actions+(entered?1:0));
            if(bad||mode==7)require(failure instanceof ClassCastException);
            else if(mode==6)require(failure instanceof ArrayStoreException);
            else require(failure==(mode==4?runtime:mode==5?error:stage1||stage2||mode==11?Trace.failure:null));
            require(Trace.marker==(entered?payload:sentinel)&&Trace.result==(entered?desired:null));
            if(entered){require(Trace.input==values);if(operation==1)require(Trace.message==message&&Trace.data==data);else {require(Trace.data!=previous);previous=Trace.data;}}
            else require(Trace.input!=values&&Trace.data==null&&Trace.message==null);
            if(values!=null&&values.length!=0)require(values[0]==(!entered||mode==6&&operation==0?first:operation==1&&data==values?extra:replacement));
            if(operation==1&&data!=null&&data.length!=0&&data!=values)require(data[0]==(entered&&mode!=6?payload:first));
            require(result==(failure==null?desired:warm));
            System.out.println("route="+route+",operation="+operation+",mode="+mode+",attempt="+attempt+",trace="+Trace.events);
        }
    }
}
