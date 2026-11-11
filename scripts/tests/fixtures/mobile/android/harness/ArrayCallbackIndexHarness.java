import fixture.*;
import java.lang.invoke.MethodHandles;
import java.lang.invoke.MethodType;
import java.lang.reflect.InvocationTargetException;
@SuppressWarnings({"rawtypes","unchecked"})
public final class ArrayCallbackIndexHarness {
    private static void require(boolean value){if(!value)throw new AssertionError("owned array callback index");}
    private static int call(int route,int operation,Filter filter,int start,Object[] values)throws Throwable {
        if(route==0)return operation==0?Client.call(filter,values):Client.from(filter,start,values);
        if(route==1)return operation==0?ArrayCallbackIndexCaller.call(filter,values):ArrayCallbackIndexCaller.from(filter,start,values);
        String name=operation==0?"call":"from";
        if(route==2){try{return (Integer)(operation==0?Client.class.getMethod(name,Filter.class,Object[].class).invoke(null,filter,values):Client.class.getMethod(name,Filter.class,int.class,Object[].class).invoke(null,filter,start,values));}catch(InvocationTargetException failure){throw failure.getCause();}}
        if(operation==0)return (int)MethodHandles.publicLookup().findStatic(Client.class,name,MethodType.methodType(int.class,Filter.class,Object[].class)).invokeExact(filter,values);
        return (int)MethodHandles.publicLookup().findStatic(Client.class,name,MethodType.methodType(int.class,Filter.class,int.class,Object[].class)).invokeExact(filter,start,values);
    }
    public static void main(String[] args)throws Throwable {
        final int route=Integer.parseInt(args[0]),operation=Integer.parseInt(args[1]),mode=Integer.parseInt(args[2]);
        final int start=operation==0?0:mode==8?-1:mode==9?Integer.MAX_VALUE:3,desired=mode==0?Integer.MIN_VALUE:mode==1?Integer.MAX_VALUE:-10000-mode;
        final RuntimeException actionFailure=new IllegalArgumentException("owned action failure"),callbackFailure=new IllegalArgumentException("owned callback failure");final Error actionError=new Error("owned action error");
        Trace.failure=new IllegalArgumentException("owned stage failure");Trace.failAt=mode==6?1:mode==7?2:0;
        for(int attempt=0;attempt<2;attempt++){
            final Object sentinel=new Object(),payload=new Object(),replacement=new Object(),token=new Object();final Object[] values=mode==1?null:mode==8?new Object[0]:mode==10?new String[]{new String("covariant")}:new Object[]{token};if(mode==9)values[0]=values;
            final Object original=values==null||values.length==0?null:values[0];Trace.events=0;Trace.filter=sentinel;Trace.values=null;Trace.start=42;Trace.result=-77;Trace.payload=sentinel;Trace.argument=sentinel;int calls=Trace.calls,callbacks=Trace.callbackCalls;
            final Filter filter=mode==2?null:new Filter(){public boolean test(Object value){require(value==original&&Trace.payload==payload);Trace.callbackCalls++;Trace.argument=value;values[0]=replacement;if(mode==11)throw callbackFailure;return mode%2==0;}};
            Trace.action=mode==3?null:new Runnable(){public void run(){require(Trace.events==1&&Trace.filter==filter&&Trace.values==values&&Trace.start==start&&Trace.payload==sentinel);Trace.hit(3);Trace.payload=payload;if(values!=null&&values.length!=0)require(((Filter)Trace.filter).test(values[0])==(mode%2==0));Trace.result=desired;if(mode==4)throw actionFailure;if(mode==5)throw actionError;}};
            int value=42;Throwable failure=null;try{value=call(route,operation,filter,start,values);}catch(Throwable error){failure=error;}
            boolean called=mode!=1&&mode!=2&&mode!=3&&mode!=6&&mode!=8,early=mode==2||mode==10||mode==11;
            require(Trace.calls==calls+1&&Trace.filter==filter&&Trace.values==values&&Trace.start==start&&Trace.callbackCalls==callbacks+(called?1:0)&&Trace.events==(mode==3||mode==6?1:mode==4||mode==5||early?13:132));
            Throwable expected=mode==4?actionFailure:mode==5?actionError:mode==6||mode==7?Trace.failure:mode==11?callbackFailure:null;
            if(expected!=null)require(failure==expected&&value==42);else if(mode==2||mode==3)require(failure instanceof NullPointerException&&value==42);else if(mode==10)require(failure instanceof ArrayStoreException&&value==42);else require(failure==null&&value==desired);
            require(Trace.payload==(mode==3||mode==6?sentinel:payload)&&Trace.argument==(called?original:sentinel)&&Trace.result==(mode==3||mode==6||early?-77:desired));
            if(values!=null&&values.length!=0)require(values[0]==(called&&mode!=10?replacement:original));
            System.out.println("route="+route+",operation="+operation+",mode="+mode+",attempt="+attempt+",trace="+Trace.events);
        }
    }
}
