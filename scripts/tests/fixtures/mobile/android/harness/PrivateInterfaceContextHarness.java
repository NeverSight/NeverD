import fixture.Core;
import fixture.Trace;
import java.util.Calendar;
import java.util.Locale;
import java.util.TimeZone;
import java.lang.invoke.MethodHandles;
import java.lang.invoke.MethodType;
import java.lang.reflect.InvocationTargetException;
public final class PrivateInterfaceContextHarness {
    private static void require(boolean value){if(!value)throw new AssertionError("owned private interface context");}
    private static final class Opaque implements Appendable {
        public Appendable append(CharSequence text){return this;}
        public Appendable append(CharSequence text,int start,int end){return this;}
        public Appendable append(char text){return this;}
        public String toString(){throw new AssertionError("owned private interface context");}
    }
    private static Appendable call(int route,int operation,Core receiver,Calendar context,Appendable value)throws Throwable {
        if(route==0)return operation==0?receiver.call(context,value):receiver.boxed(context,(StringBuilder)value);
        if(route==1)return operation==0?PrivateInterfaceContextCaller.call(receiver,context,value):PrivateInterfaceContextCaller.boxed(receiver,context,(StringBuilder)value);
        String name=operation==0?"call":"boxed";Class<?> parameter=operation==0?Appendable.class:StringBuilder.class;
        if(route==2){try{return (Appendable)Core.class.getMethod(name,Calendar.class,parameter).invoke(receiver,context,value);}catch(InvocationTargetException failure){throw failure.getCause();}}
        return (Appendable)MethodHandles.publicLookup().findVirtual(Core.class,name,MethodType.methodType(parameter,Calendar.class,parameter)).asType(MethodType.methodType(Appendable.class,Core.class,Calendar.class,Appendable.class)).invokeExact(receiver,context,value);
    }
    public static void main(String[] args)throws Throwable {
        final int route=Integer.parseInt(args[0]),operation=Integer.parseInt(args[1]),mode=Integer.parseInt(args[2]);final Core receiver=new Core();
        final Calendar context=mode==1?null:Calendar.getInstance(TimeZone.getTimeZone(mode==2?"GMT+09:00":"UTC"),Locale.ROOT),other=Calendar.getInstance(TimeZone.getTimeZone("UTC"),Locale.ROOT);
        if(context!=null)context.setTimeInMillis(13579L);
        final StringBuilder value=mode==2?null:new StringBuilder("seed"),desiredText=new StringBuilder("opaque result "+mode);final Appendable desired=mode==8?new Opaque():mode==9?null:desiredText;
        final RuntimeException actionFailure=new IllegalArgumentException("owned action failure");final Error actionError=new Error("owned action error");
        Trace.failure=new IllegalArgumentException("owned stage failure");Trace.failAt=mode==6?1:mode==7?2:0;
        for(int attempt=0;attempt<2;attempt++) {
            final long millis=24680L+attempt;final Object sentinel=new Object(),payload=new Object();final StringBuilder sentinelResult=new StringBuilder("sentinel");Trace.events=0;Trace.receiver=sentinel;Trace.calendar=other;Trace.value=sentinelResult;Trace.result=sentinelResult;Trace.payload=sentinel;int calls=Trace.calls;
            Trace.action=mode==3?null:new Runnable(){public void run(){require(Trace.events==1&&Trace.receiver==receiver&&Trace.calendar==context&&Trace.value==value&&Trace.payload==sentinel);Trace.hit(3);Trace.payload=payload;Trace.result=desired;if(context!=null)context.setTimeInMillis(millis);if(value!=null)value.append('!');if(mode==10)Trace.calendar=other;if(mode==11)Trace.receiver=sentinel;if(mode==4)throw actionFailure;if(mode==5)throw actionError;}};
            Appendable actual=sentinelResult;int length=42;Throwable failure=null;try{actual=call(route,operation,receiver,context,value);length=((StringBuilder)actual).length();}catch(Throwable error){failure=error;}
            require(Trace.calls==calls+1&&Trace.events==(mode==3||mode==6?1:mode==4||mode==5?13:132));
            Throwable expected=mode==4?actionFailure:mode==5?actionError:mode==6||mode==7?Trace.failure:null;
            if(expected!=null)require(failure==expected&&actual==sentinelResult&&length==42);
            else if(mode==3)require(failure instanceof NullPointerException&&actual==sentinelResult&&length==42);
            else if(mode==8)require(failure instanceof ClassCastException&&actual==(operation==0?desired:sentinelResult)&&length==42);
            else if(mode==9)require(failure instanceof NullPointerException&&actual==null&&length==42);
            else require(failure==null&&actual==desired&&length==desiredText.length());
            require(Trace.payload==(mode==3||mode==6?sentinel:payload)&&Trace.calendar==(mode==10?other:context)&&Trace.receiver==(mode==11?sentinel:receiver)&&Trace.value==value);
            if(context!=null&&mode!=3&&mode!=6)require(context.getTimeInMillis()==millis);
            System.out.println("route="+route+",operation="+operation+",mode="+mode+",attempt="+attempt+",trace="+Trace.events);
        }
    }
}
