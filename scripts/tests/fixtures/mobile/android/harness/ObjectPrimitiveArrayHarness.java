import fixture.Client;
import fixture.Trace;
import java.lang.invoke.MethodHandles;
import java.lang.invoke.MethodType;
import java.lang.reflect.InvocationTargetException;
public final class ObjectPrimitiveArrayHarness {
    private static void require(boolean value){if(!value)throw new AssertionError("owned object primitive array");}
    private static final class Trap {
        public String toString(){throw new AssertionError("owned unexpected text");}
        public boolean equals(Object value){throw new AssertionError("owned unexpected equality");}
        public int hashCode(){throw new AssertionError("owned unexpected hash");}
    }
    private static byte[] call(int route,Object value)throws Throwable {
        if(route==0)return Client.call(value);if(route==1)return ObjectPrimitiveArrayCaller.call(value);
        if(route==2){try{return (byte[])Client.class.getMethod("call",Object.class).invoke(null,value);}catch(InvocationTargetException failure){throw failure.getCause();}}
        return (byte[])MethodHandles.publicLookup().findStatic(Client.class,"call",MethodType.methodType(byte[].class,Object.class)).invokeExact(value);
    }
    public static void main(String[] args)throws Throwable {
        final int route=Integer.parseInt(args[0]),operation=Integer.parseInt(args[1]),mode=Integer.parseInt(args[2]);
        final RuntimeException actionFailure=new RuntimeException("owned action failure");final Error actionError=new Error("owned action error");
        Trace.failure=new RuntimeException("owned stage failure");Trace.failAt=mode==6?1:mode==7?2:0;
        for(int attempt=0;attempt<2;attempt++) {
            final byte[] desired=mode==8?null:mode==3?new byte[0]:mode==10?new byte[]{37}:new byte[]{(byte)(-128+attempt),0,127};
            final Object value=mode==1?null:mode==2?desired:new Trap(),sentinel=new Object(),payload=new Object();final byte[] old=new byte[]{55};
            Trace.events=0;Trace.input=Trace.payload=sentinel;Trace.result=old;int calls=Trace.calls;
            Trace.action=new Runnable(){public void run(){require(Trace.events==1&&Trace.input==value&&Trace.payload==sentinel);Trace.hit(3);Trace.payload=payload;Trace.result=desired;if(mode==11)desired[0]=37;if(mode==4)throw actionFailure;if(mode==5)throw actionError;}};
            byte[] actual=old;int first=42;Throwable failure=null;try{actual=call(route,value);require(actual==desired);if(operation==1){first=actual[0];actual[0]=(byte)(first^127);}}catch(Throwable error){failure=error;}
            require(Trace.calls==calls+1&&Trace.input==value&&Trace.events==(mode==6?1:mode==4||mode==5?13:132));
            Throwable expected=mode==4?actionFailure:mode==5?actionError:mode==6||mode==7?Trace.failure:null;
            if(expected!=null)require(failure==expected&&actual==old&&first==42);
            else if(operation==1&&mode==3)require(failure instanceof ArrayIndexOutOfBoundsException&&actual==desired&&first==42);
            else if(operation==1&&mode==8)require(failure instanceof NullPointerException&&actual==null&&first==42);
            else {require(failure==null&&actual==desired);if(operation==1){int initial=mode==11||mode==10?37:-128+attempt;require(first==initial&&desired[0]==(byte)(initial^127));}else require(first==42);}
            require(Trace.payload==(mode==6?sentinel:payload));
            System.out.println("route="+route+",operation="+operation+",mode="+mode+",attempt="+attempt+",trace="+Trace.events);
        }
    }
}
