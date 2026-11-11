import fixture.*;
import java.io.IOException;
import java.lang.reflect.Type;
import java.lang.invoke.MethodHandles;
import java.lang.invoke.MethodType;
import java.lang.reflect.InvocationTargetException;
public final class TokenSeedUncheckedHarness {
    private static void require(boolean ok){if(!ok)throw new AssertionError("owned token seed unchecked");}
    @SuppressWarnings("unchecked") private static <X extends Throwable> void fail(Throwable failure)throws X{throw (X)failure;}
    @SuppressWarnings({"rawtypes","unchecked"})
    private static Object call(int route,int operation,Type type,Object value,Object fallback)throws Throwable {
        if(route==0)return operation==0?Client.call((Class)type,value,fallback):Client.full(type,value,fallback);
        if(route==1)return operation==0?TokenSeedUncheckedCaller.call((Class)type,value,fallback):TokenSeedUncheckedCaller.full(type,value,fallback);
        if(route==2){try{return Client.class.getMethod(operation==0?"call":"full",operation==0?Class.class:Type.class,Object.class,Object.class).invoke(null,type,value,fallback);}catch(InvocationTargetException failure){throw failure.getCause();}}
        if(operation==0)return (Object)MethodHandles.publicLookup().findStatic(Client.class,"call",MethodType.methodType(Object.class,Class.class,Object.class,Object.class)).invokeExact((Class)type,value,fallback);
        return (Object)MethodHandles.publicLookup().findStatic(Client.class,"full",MethodType.methodType(Object.class,Type.class,Object.class,Object.class)).invokeExact(type,value,fallback);
    }
    public static void main(String[] args)throws Throwable {
        final int route=Integer.parseInt(args[0]),operation=Integer.parseInt(args[1]),mode=Integer.parseInt(args[2]);
        final Failure runtime=new Failure("owned failure");final Error error=new Error("owned error");final IOException checked=new IOException("owned checked");Trace.failure=new Failure("owned stage");Trace.failAt=mode==6?(operation==0?1:2):mode==7?3:0;
        for(int attempt=0;attempt<2;attempt++) {
            final int[] typeReads={0};final Type hostile=new Type(){public String getTypeName(){typeReads[0]++;throw new AssertionError("unrequested type interpretation");}};
            final Type type=mode==1?null:mode==8?(operation==0?Integer.class:hostile):String.class;
            final Object value=mode==2?null:new Object(),fallback=mode==3?null:new String("fallback"),sentinel=new Object(),payload=new Object();
            final Object desired=mode==9?Integer.valueOf(17):mode==10?null:new String("owned result");
            Trace.events=0;Trace.token=sentinel;Trace.type=sentinel;Trace.value=sentinel;Trace.fallback=sentinel;Trace.payload=sentinel;Trace.result=sentinel;
            final int tokenCalls=Trace.tokenCalls,calls=Trace.calls,actions=Trace.actions;
            Trace.action=new Runnable(){public void run(){Trace.actions++;require(Trace.events==(operation==0?12:2)&&Trace.token==(operation==0?type:sentinel)&&Trace.type==type&&Trace.value==value&&Trace.fallback==fallback);Trace.hit(4);Trace.payload=payload;Trace.result=desired;Trace.value=payload;Trace.fallback=payload;Trace.token=Integer.class;if(mode==4)throw runtime;if(mode==5)throw error;if(mode==11)TokenSeedUncheckedHarness.<RuntimeException>fail(checked);}};
            String actual="initial";Integer length=null;Throwable failure=null;
            try{actual=(String)call(route,operation,type,value,fallback);length=Integer.valueOf(actual.length());}catch(Throwable caught){failure=caught;}
            boolean entered=!(mode==6&&operation==0),actionReached=mode!=6,finished=mode!=6&&mode!=4&&mode!=5&&mode!=11;
            long events=mode==6?(operation==0?1:2):operation==0?(finished?1243:124):(finished?243:24);
            require(Trace.tokenCalls==tokenCalls+(operation==0?1:0)&&Trace.calls==calls+(entered?1:0)&&Trace.actions==actions+(actionReached?1:0)&&Trace.events==events&&typeReads[0]==0);
            require(Trace.type==(entered?type:sentinel)&&Trace.value==(actionReached?payload:entered?value:sentinel)&&Trace.fallback==(actionReached?payload:entered?fallback:sentinel)&&Trace.payload==(actionReached?payload:sentinel)&&Trace.result==(actionReached?desired:sentinel));
            require(Trace.token==(actionReached?Integer.class:operation==0?type:sentinel));
            Throwable expected=mode==4?runtime:mode==5?error:mode==6||mode==7?Trace.failure:mode==11?checked:null;
            if(expected!=null)require(failure==expected&&actual.equals("initial")&&length==null);
            else if(mode==9)require(failure instanceof ClassCastException&&actual.equals("initial")&&length==null);
            else if(mode==10)require(failure instanceof NullPointerException&&actual==null&&length==null);
            else require(failure==null&&actual==desired&&length.intValue()==actual.length());
            System.out.println("route="+route+",operation="+operation+",mode="+mode+",attempt="+attempt+",trace="+Trace.events);
        }
    }
}
