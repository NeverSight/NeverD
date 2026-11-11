import fixture.Client;
import fixture.Trace;
import java.lang.invoke.MethodHandles;
import java.lang.invoke.MethodType;
import java.lang.reflect.InvocationTargetException;
public final class ArrayBooleanVarargsHarness {
    private static void require(boolean value){if(!value)throw new AssertionError("owned array boolean varargs");}
    private static boolean call(int route,int operation,Object[] values)throws Throwable {
        if(route==0)return operation==0?Client.call(values):Client.decide(values);
        if(route==1)return operation==0?ArrayBooleanVarargsCaller.call(values):ArrayBooleanVarargsCaller.decide(values);
        String name=operation==0?"call":"decide";
        if(route==2){try{return (Boolean)Client.class.getMethod(name,Object[].class).invoke(null,(Object)values);}catch(InvocationTargetException failure){throw failure.getCause();}}
        return (boolean)MethodHandles.publicLookup().findStatic(Client.class,name,MethodType.methodType(boolean.class,Object[].class)).invokeExact(values);
    }
    public static void main(String[] args)throws Throwable {
        final int route=Integer.parseInt(args[0]),operation=Integer.parseInt(args[1]),mode=Integer.parseInt(args[2]);
        final Object[] cycle=new Object[1];cycle[0]=cycle;
        final Object[] values=mode==1?null:mode==2?new Object[0]:mode==3?cycle:mode==8?new String[]{"owned"}:new Object[]{new Object(),null};
        final Object first=values==null||values.length==0?null:values[0];
        final boolean desired=mode==10?false:mode%2==0||mode==11;
        final RuntimeException actionFailure=new RuntimeException("owned action failure");final Error actionError=new Error("owned action error");
        Trace.failure=new RuntimeException("owned stage failure");Trace.failAt=mode==6?1:mode==7?2:0;
        for(int attempt=0;attempt<2;attempt++){
            if(values!=null&&values.length>0)values[0]=first;
            final Object sentinel=new Object(),payload=new Object();Trace.events=0;Trace.values=new Object[0];Trace.payload=sentinel;Trace.result=!desired;int calls=Trace.calls;
            Trace.action=mode==9?null:new Runnable(){public void run(){require(Trace.events==1&&Trace.values==values&&Trace.payload==sentinel);Trace.hit(3);Trace.payload=payload;Trace.result=desired;if(values!=null&&values.length>0)values[0]=payload;if(mode==4)throw actionFailure;if(mode==5)throw actionError;}};
            boolean actual=!desired;Throwable failure=null;try{actual=call(route,operation,values);}catch(Throwable error){failure=error;}
            boolean entered=mode!=6&&mode!=9;boolean early=mode==4||mode==5||mode==6||mode==7||mode==8||mode==9;
            require(Trace.calls==calls+1&&Trace.values==values&&Trace.events==(mode==6||mode==9?1:mode==4||mode==5||mode==8?13:132));
            Throwable expected=mode==4?actionFailure:mode==5?actionError:mode==6||mode==7?Trace.failure:null;
            if(expected!=null)require(failure==expected&&actual==!desired);
            else if(mode==8)require(failure instanceof ArrayStoreException&&actual==!desired);
            else if(mode==9)require(failure instanceof NullPointerException&&actual==!desired);
            else require(failure==null&&actual==desired);
            require(Trace.payload==(entered?payload:sentinel)&&Trace.result==(entered?desired:!desired));
            if(values!=null&&values.length>0)require(values[0]==(entered&&mode!=8?payload:first));
            require(early==(failure!=null));
            System.out.println("route="+route+",operation="+operation+",mode="+mode+",attempt="+attempt+",trace="+Trace.events);
        }
    }
}
