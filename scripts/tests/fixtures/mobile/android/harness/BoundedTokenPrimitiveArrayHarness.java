import fixture.Client;
import fixture.Trace;
import java.nio.ByteOrder;
import java.lang.invoke.MethodHandles;
import java.lang.invoke.MethodType;
import java.lang.reflect.InvocationTargetException;
public final class BoundedTokenPrimitiveArrayHarness {
    private static void require(boolean value){if(!value)throw new AssertionError("owned bounded token primitive array");}
    @SuppressWarnings({"rawtypes","unchecked"})
    private static Number call(int route,byte[] data,Class type,ByteOrder order)throws Throwable {
        if(route==0)return Client.call(data,type,order);if(route==1)return BoundedTokenPrimitiveArrayCaller.call(data,type,order);
        if(route==2){try{return (Number)Client.class.getMethod("call",byte[].class,Class.class,ByteOrder.class).invoke(null,data,type,order);}catch(InvocationTargetException failure){throw failure.getCause();}}
        return (Number)MethodHandles.publicLookup().findStatic(Client.class,"call",MethodType.methodType(Number.class,byte[].class,Class.class,ByteOrder.class)).invokeExact(data,type,order);
    }
    @SuppressWarnings("rawtypes")
    public static void main(String[] args)throws Throwable {
        final int route=Integer.parseInt(args[0]),operation=Integer.parseInt(args[1]),mode=Integer.parseInt(args[2]);
        final byte[] data=mode==1?null:mode==2?new byte[0]:new byte[]{Byte.MIN_VALUE,Byte.MAX_VALUE};
        final Class type=mode==3?null:mode==8?Double.class:mode==10?String.class:Integer.class;
        final ByteOrder order=mode==11?null:operation==0?ByteOrder.BIG_ENDIAN:ByteOrder.LITTLE_ENDIAN;
        final Number desired=mode==8?(Number)Double.valueOf(2.5):mode==9?null:Integer.valueOf(-37);
        final RuntimeException actionFailure=new IllegalArgumentException("owned action failure");final Error actionError=new Error("owned action error");
        Trace.failure=new IllegalArgumentException("owned stage failure");Trace.failAt=mode==6?1:mode==7?2:0;
        for(int attempt=0;attempt<2;attempt++) {
            if(data!=null&&data.length!=0)data[0]=Byte.MIN_VALUE;
            final Object sentinel=new Object(),payload=new Object();Trace.events=0;Trace.type=null;Trace.data=null;Trace.order=null;Trace.payload=sentinel;Trace.result=Integer.valueOf(19);int calls=Trace.calls;
            Trace.action=new Runnable(){public void run(){require(Trace.events==1&&Trace.type==type&&Trace.data==data&&Trace.order==order&&Trace.payload==sentinel);Trace.hit(3);Trace.payload=payload;Trace.result=desired;if(data!=null&&data.length!=0)data[0]=(byte)(operation==0?-23:45);if(mode==4)throw actionFailure;if(mode==5)throw actionError;}};
            Number actual=Integer.valueOf(19);int value=42;Throwable failure=null;try{actual=call(route,data,type,order);value=((Integer)actual).intValue();}catch(Throwable error){failure=error;}
            require(Trace.calls==calls+1&&Trace.type==type&&Trace.data==data&&Trace.order==order&&Trace.events==(mode==6?1:mode==4||mode==5?13:132));
            Throwable expected=mode==4?actionFailure:mode==5?actionError:mode==6||mode==7?Trace.failure:null;
            if(expected!=null)require(failure==expected&&actual.intValue()==19&&value==42);
            else if(mode==8)require(failure instanceof ClassCastException&&actual==desired&&value==42);
            else if(mode==9)require(failure instanceof NullPointerException&&actual==null&&value==42);
            else require(failure==null&&actual==desired&&value==-37);
            require(Trace.payload==(mode==6?sentinel:payload));
            if(data!=null&&data.length!=0)require(data[0]==(mode==6?Byte.MIN_VALUE:operation==0?-23:45)&&data[1]==Byte.MAX_VALUE);
            System.out.println("route="+route+",operation="+operation+",mode="+mode+",attempt="+attempt+",trace="+Trace.events);
        }
    }
}
