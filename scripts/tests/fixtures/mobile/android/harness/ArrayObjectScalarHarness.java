import fixture.Client;
import fixture.Provider;
import fixture.Trace;
import java.lang.invoke.MethodHandles;
import java.lang.invoke.MethodType;
import java.lang.reflect.InvocationTargetException;
public final class ArrayObjectScalarHarness {
    private static void require(boolean value) {if(!value)throw new AssertionError("owned array object scalar");}
    private static final class Trap {
        public boolean equals(Object value) {throw new AssertionError("owned unexpected equals");}
        public int hashCode() {throw new AssertionError("owned unexpected hash");}
        public String toString() {throw new AssertionError("owned unexpected text");}
    }
    private static int call(int route,int kind,Object[] values,Object key) throws Throwable {
        if(route==0)return kind==0?Provider.<Object>score(values,key):new Client<Object>(values,kind==2).lookup(key);
        if(route==1)return kind==0?ArrayObjectScalarCaller.direct(values,key):ArrayObjectScalarCaller.lookup(values,key,kind==2);
        if(route==2) {
            try {return ((Integer)(kind==0?Provider.class.getMethod("score",Object[].class,Object.class).invoke(null,values,key):Client.class.getMethod("lookup",Object.class).invoke(new Client<Object>(values,kind==2),key))).intValue();}
            catch(InvocationTargetException failure) {throw failure.getCause();}
        }
        if(kind==0)return (int)MethodHandles.publicLookup().findStatic(Provider.class,"score",MethodType.methodType(int.class,Object[].class,Object.class)).invokeExact(values,key);
        return (int)MethodHandles.publicLookup().findVirtual(Client.class,"lookup",MethodType.methodType(int.class,Object.class)).invokeExact(new Client<Object>(values,kind==2),key);
    }
    public static void main(String[] args) throws Throwable {
        final int route=Integer.parseInt(args[0]),kind=Integer.parseInt(args[1]),mode=Integer.parseInt(args[2]);
        final Object[] values=mode==9?null:mode==10?new String[0]:mode==11?new String[]{new String("owned before")}:new Object[2];
        final Object initial=mode==1?values:mode==11?values[0]:new Trap(),after=mode==11?new String("owned after"):new Trap();
        final Object key=mode==1?values:mode==2||mode==10?null:new Trap(),payload=new Object();
        final int desired=mode==1||mode==11?Integer.MAX_VALUE-37:mode==2?-37:mode==9?-17:Integer.MIN_VALUE+37+mode;
        final RuntimeException actionFailure=new RuntimeException("owned action failure");final Error actionError=new Error("owned action error");
        Trace.failure=new RuntimeException("owned stage failure");Trace.failAt=mode==5?1:mode==6?2:0;
        for(int attempt=0;attempt<2;attempt++) {
            final Object sentinel=new Object();Trace.events=0;Trace.values=Trace.key=Trace.result=Trace.payload=sentinel;int calls=Trace.calls;
            if(values!=null&&values.length>0)values[0]=initial;
            Trace.action=new Runnable(){public void run(){
                require(Trace.events==1&&Trace.values==values&&Trace.key==key&&Trace.payload==sentinel);Trace.hit(3);
                if(values!=null&&values.length>0){require(values[0]==initial);values[0]=after;}
                Trace.payload=payload;Trace.result=mode==7?new Object():mode==8?null:Integer.valueOf(desired);
                if(mode==3)throw actionFailure;if(mode==4)throw actionError;
            }};
            int actual=42;Throwable failure=null;try {actual=call(route,kind,values,key);}catch(Throwable error){failure=error;}
            require(Trace.calls==calls+1&&Trace.values==values&&Trace.key==key&&Trace.events==(mode==5?1:mode==3||mode==4?13:132));
            Throwable expected=mode==3?actionFailure:mode==4?actionError:mode==5||mode==6?Trace.failure:null;
            if(expected!=null)require(failure==expected&&actual==42);
            else if(mode==7)require(failure instanceof ClassCastException&&actual==42);
            else if(mode==8||mode==9&&kind==2)require(failure instanceof NullPointerException&&actual==42);
            else require(failure==null&&actual==(kind==0||desired>=0?desired:kind==2?values.length:-1));
            require(Trace.payload==(mode==5?sentinel:payload));
            if(values!=null&&values.length>0)require(values[0]==(mode==5?initial:after));
            System.out.println("route="+route+",operation="+kind+",mode="+mode+",attempt="+attempt+",trace="+Trace.events);
        }
    }
}
