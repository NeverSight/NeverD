import fixture.*;
import java.lang.invoke.*;
import java.lang.reflect.*;
public final class BoundedSeedScalarHarness {
    static void require(boolean ok) { if(!ok)throw new AssertionError("owned bounded seed scalar"); }
    static final class Derived extends Bound { Derived(long value){super(value);} }
    static final class Output implements Result { final Object token=new Object();public Object value(){return token;} }
    static Result call(int route,Bound seed,float first,float second)throws Throwable {
        if(route==0)return Client.call(seed,first,second);
        if(route==1)return BoundedSeedScalarCaller.call(seed,first,second);
        if(route==2||route==3) {
            Method m=route==2?Provider.class.getMethod("access",Bound.class,float.class,float.class):Provider.class.getDeclaredMethod("make",Bound.class,float.class,float.class);
            if(route==3)m.setAccessible(true);
            try{return(Result)m.invoke(null,seed,first,second);}catch(InvocationTargetException f){throw f.getCause();}
        }
        MethodHandle m=MethodHandles.lookup().findStatic(Provider.class,"access",MethodType.methodType(Result.class,Bound.class,float.class,float.class));
        return(Result)m.invokeExact(seed,first,second);
    }
    public static void main(String[] args)throws Throwable {
        int route=Integer.parseInt(args[0]),mode=Integer.parseInt(args[1]);
        Method method=Provider.class.getDeclaredMethod("make",Bound.class,float.class,float.class);
        TypeVariable<?>[] params=method.getTypeParameters();require(Modifier.isPrivate(method.getModifiers())&&Modifier.isStatic(method.getModifiers())&&params.length==1&&params[0].getBounds()[0]==Bound.class);
        Type[] inputs=method.getGenericParameterTypes();require(inputs[0].equals(params[0])&&inputs[1]==float.class&&inputs[2]==float.class&&method.getGenericReturnType()==Result.class);
        int[] bits={0x7fc12345,0xffc54321,0x80000000,0x00000000,0x7f800000,0xff800000,0x7f7fffff,0x00000001,0x3f800001};
        for(int attempt=0;attempt<2;attempt++) {
            Bound seed=mode==1||mode==2?null:mode==10?new Derived(Long.MIN_VALUE+attempt):new Bound(Long.MAX_VALUE-attempt);
            float first=Float.intBitsToFloat(bits[(mode+attempt)%bits.length]),second=Float.intBitsToFloat(bits[(mode+attempt+3)%bits.length]);
            Result expected=mode==7?null:new Output();Provider.output=expected;
            Trace.events=0;Trace.seed=Trace.result=null;Trace.first=Trace.second=0;Trace.failAt=mode==2?2:mode>=3&&mode<=6?mode-2:0;Trace.failure=new RuntimeException("owned scalar failure");
            Throwable caught=null;Result actual=null;try{actual=call(route,seed,first,second);}catch(Throwable f){caught=f;}
            long events=Trace.failAt==1?1:seed==null?12:Trace.failAt==3?13:134;
            require(Trace.events==events&&Trace.seed==seed&&Float.floatToRawIntBits(Trace.first)==Float.floatToRawIntBits(first)&&Float.floatToRawIntBits(Trace.second)==Float.floatToRawIntBits(second));
            boolean failed=Trace.failAt!=0&&(seed!=null?Trace.failAt!=2:true);
            if(failed)require(caught==Trace.failure);else require(caught==null&&actual==(seed==null?null:expected));
            require(Trace.result==((seed==null||Trace.failAt==1||Trace.failAt==3)?null:expected));
            if(actual!=null)require(actual.value()==((Output)expected).token);
            System.out.println("route="+route+",mode="+mode+",attempt="+attempt+",trace="+events);
        }
    }
}
