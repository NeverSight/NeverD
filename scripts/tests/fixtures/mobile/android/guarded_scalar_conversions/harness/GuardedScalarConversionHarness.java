import fixture.*;
import java.lang.reflect.*;
public final class GuardedScalarConversionHarness {
    static void require(boolean value, String detail) { if (!value) throw new AssertionError("guarded scalar conversion contract: " + detail); }
    static Core make(int variant,int route,int seed,float narrow,double number,long wide,int mask,Object marker) throws Throwable {
        if (route==0) return variant==0 ? GuardedScalarConversionCaller.narrow(seed,narrow,mask,marker) : GuardedScalarConversionCaller.wide(number,wide,mask,marker);
        try {
            if (variant==0) return Core.class.getConstructor(int.class,float.class,int.class,Object.class).newInstance(seed,narrow,mask,marker);
            return Core.class.getConstructor(double.class,long.class,int.class,Object.class).newInstance(number,wide,mask,marker);
        } catch (InvocationTargetException failure) { throw failure.getCause(); }
    }
    public static void main(String[] args) throws Throwable {
        int variant=Integer.parseInt(args[0]),route=Integer.parseInt(args[1]),branch=Integer.parseInt(args[2]),mode=Integer.parseInt(args[3]);
        int[] integers={0,1,-1,16777217,Integer.MAX_VALUE,Integer.MIN_VALUE,16777219,-16777219};
        double[] doubles={Double.NaN,Double.POSITIVE_INFINITY,Double.NEGATIVE_INFINITY,0x1.0p63,-0x1.0p63,0x1.fffffffffffffp62,1.999,-1.999};
        float[] floats={Float.intBitsToFloat(0x7fc01234),-0.0f,Float.POSITIVE_INFINITY,Float.NEGATIVE_INFINITY,Float.MIN_VALUE,Float.MAX_VALUE,1.25f,-1.25f};
        long[] longs={Long.MIN_VALUE,Long.MAX_VALUE,-1,0,0x1234567887654321L,-0x1234567887654321L,17,-17};
        boolean initialized=false,failed=false;
        for (int attempt=0;attempt<2;attempt++) {
            int at=(mode+attempt)%8,seed=integers[at];double number=doubles[at];
            float incomingFloat=floats[at];long incomingLong=longs[at];
            int mask=branch==0?2:3;
            Trace.fail=mode>=8?mode-7:0;Trace.events=Trace.reads=Trace.calls=Trace.intInput=0;
            Trace.doubleInput=0;Trace.convertedFloat=0;Trace.convertedLong=0;Trace.partial=null;
            Trace.failure=new IllegalStateException("owned conversion failure");
            int expected=0,reads=0,calls=0;Class<?> expectedError=null;boolean body=false,published=false,stored=false;
            if (branch!=0) {
                expected=1;reads=1;
                if (Trace.fail==1) { expectedError=IllegalStateException.class;body=true; }
                if (expectedError==null) {
                    if (failed) expectedError=NoClassDefFoundError.class;
                    else if (!initialized) {
                        initialized=true;expected=expected*10+2;
                        if (Trace.fail==2) { failed=true;expectedError=ExceptionInInitializerError.class; }
                    }
                }
                if (expectedError==null) {
                    expected=expected*10+3;calls=1;
                    if (Trace.fail==3) { expectedError=IllegalStateException.class;body=true; }
                }
            }
            if (expectedError==null) {
                published=true;expected=expected*10+4;
                if (Trace.fail==4) { expectedError=IllegalStateException.class;body=true; }
            }
            if (expectedError==null) {
                stored=true;expected=expected*10+5;
                if (Trace.fail==5) { expectedError=IllegalStateException.class;body=true; }
            }
            Core result=null;Throwable error=null;
            try { result=make(variant,route,seed,incomingFloat,number,incomingLong,mask,attempt==0?null:new Object()); }
            catch(Throwable failure) { error=failure; }
            require(Trace.events==expected,"effect order");
            require(expectedError==null?error==null:error!=null&&error.getClass()==expectedError,"failure class");
            if(body)require(error==Trace.failure,"failure identity");
            if(expectedError==ExceptionInInitializerError.class)require(error.getCause()==Trace.failure,"initializer cause");
            require(Trace.reads==reads&&Trace.calls==calls,"selected read and provider count");
            if(reads!=0) {
                if(variant==0)require(Trace.intInput==seed,"int input");
                else require(Double.doubleToRawLongBits(Trace.doubleInput)==Double.doubleToRawLongBits(number),"double input bits");
            }
            if(calls!=0) {
                if(variant==0)require(Float.floatToRawIntBits(Trace.convertedFloat)==Float.floatToRawIntBits((float)seed),"rounded float argument");
                else require(Trace.convertedLong==(long)number,"clamped long argument");
            }
            require((Trace.partial!=null)==published,"partial constructor identity");
            if(published) {
                float wantedFloat=stored&&variant==0?(branch!=0?(float)seed:incomingFloat):0;
                long wantedLong=stored&&variant==1?(branch!=0?(long)number:incomingLong):0;
                require(Float.floatToRawIntBits(Trace.partial.narrow)==Float.floatToRawIntBits(wantedFloat),"stored raw float bits");
                require(Trace.partial.wide==wantedLong,"stored wide value");
            }
            if(expectedError==null)require(result==Trace.partial,"result identity");
            System.out.println("variant="+variant+",route="+route+",branch="+branch+",mode="+mode+",attempt="+attempt+",trace="+Trace.events+",error="+(error==null?"none":error.getClass().getSimpleName()));
        }
    }
}
