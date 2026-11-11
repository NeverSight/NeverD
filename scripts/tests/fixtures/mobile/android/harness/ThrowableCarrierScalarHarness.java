import fixture.*;
import java.util.function.Supplier;
import java.io.IOException;
import java.lang.invoke.MethodHandles;
import java.lang.invoke.MethodType;
import java.lang.reflect.InvocationTargetException;
public final class ThrowableCarrierScalarHarness {
    private static void require(boolean ok){if(!ok)throw new AssertionError("owned throwable carrier scalar");}
    private static long bits(double value){return Double.doubleToRawLongBits(value);}
    @SuppressWarnings("unchecked")
    private static double call(int route,int operation,double value,double lower,double upper,Object errors)throws Throwable {
        Supplier<? extends RuntimeException> typed=operation==0?(Supplier<? extends RuntimeException>)errors:null;
        if(route==0)return operation==0?Core.call(value,lower,upper,typed):Core.checked(value,lower,upper,errors);
        if(route==1)return operation==0?ThrowableCarrierScalarCaller.call(value,lower,upper,typed):ThrowableCarrierScalarCaller.checked(value,lower,upper,errors);
        if(route==2){try{return ((Double)Core.class.getMethod(operation==0?"call":"checked",double.class,double.class,double.class,operation==0?Supplier.class:Object.class).invoke(null,value,lower,upper,errors)).doubleValue();}catch(InvocationTargetException failure){throw failure.getCause();}}
        if(operation==0)return (double)MethodHandles.publicLookup().findStatic(Core.class,"call",MethodType.methodType(double.class,double.class,double.class,double.class,Supplier.class)).invokeExact(value,lower,upper,typed);
        return (double)MethodHandles.publicLookup().findStatic(Core.class,"checked",MethodType.methodType(double.class,double.class,double.class,double.class,Object.class)).invokeExact(value,lower,upper,errors);
    }
    public static void main(String[] args)throws Throwable {
        final int route=Integer.parseInt(args[0]),operation=Integer.parseInt(args[1]),mode=Integer.parseInt(args[2]);
        final Throwable checked=new IOException("owned checked"),runtime=new IllegalArgumentException("owned runtime"),error=new Error("owned error"),callback=new IllegalArgumentException("owned callback");
        Trace.failure=new IllegalArgumentException("owned stage");Trace.reject=mode>=1&&mode<=7;Trace.failAt=mode==8&&operation==0?4:mode>=9?mode-8:0;
        for(int attempt=0;attempt<2;attempt++) {
            final double value=attempt==0?-0.0:Double.longBitsToDouble(0x7ff8000000000123L+mode),lower=attempt==0?Double.NEGATIVE_INFINITY:Double.longBitsToDouble(0x8000000000000001L),upper=attempt==0?Double.POSITIVE_INFINITY:Double.MAX_VALUE;
            final double desired=attempt==0?Double.longBitsToDouble(0x8000000000000001L):Double.longBitsToDouble(0x7ff8000000000567L+mode);
            final Object payload=new Object(),sentinel=new Object();final Object selected=mode==2?null:mode==3?new Object():mode==4?checked:mode==6?error:runtime;
            Supplier supplier=new Supplier(){public Object get(){Trace.calls++;Trace.hit(5);require(Trace.marker==payload);if(mode==7)throw (RuntimeException)callback;return selected;}};
            final Object argument=mode==1?null:mode==8&&operation==1?new Object():supplier;
            Trace.events=0;Trace.value=Trace.lower=Trace.upper=17.0;Trace.provider=sentinel;Trace.marker=sentinel;Trace.result=19.0;final int calls=Trace.calls,actions=Trace.actions;
            Trace.action=new Runnable(){public void run(){Trace.actions++;require(Trace.events==12&&Trace.provider==argument&&bits(Trace.value)==bits(value)&&bits(Trace.lower)==bits(lower)&&bits(Trace.upper)==bits(upper));Trace.result=desired;Trace.marker=payload;}};
            double result=23.0;Throwable failure=null;try{result=call(route,operation,value,lower,upper,argument);}catch(Throwable caught){failure=caught;}
            final boolean bad=mode==8&&operation==1,entered=!bad&&mode!=9&&mode!=10;
            final long events=bad||mode==9?1:mode==1||mode==10?12:mode>=2&&mode<=7?125:mode==11?123:1234;
            require(Trace.events==events&&Trace.actions==actions+(entered?1:0)&&Trace.calls==calls+(mode>=2&&mode<=7?1:0));
            if(mode==1||mode==2)require(failure instanceof NullPointerException);
            else if(mode==3||bad)require(failure instanceof ClassCastException);
            else if(mode>=4&&mode<=7)require(failure==(mode==4?checked:mode==6?error:mode==7?callback:runtime));
            else if(mode>=8)require(failure==Trace.failure);else require(failure==null);
            require(Trace.provider==(entered?argument:sentinel)&&Trace.marker==(entered?payload:sentinel));
            require(bits(Trace.value)==bits(entered?value:17.0)&&bits(Trace.lower)==bits(entered?lower:17.0)&&bits(Trace.upper)==bits(entered?upper:17.0));
            require(bits(Trace.result)==bits(entered?desired:19.0)&&bits(result)==bits(failure==null?desired:23.0));
            System.out.println("route="+route+",operation="+operation+",mode="+mode+",attempt="+attempt+",trace="+Trace.events);
        }
    }
}
