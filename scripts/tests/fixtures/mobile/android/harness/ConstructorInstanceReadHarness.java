import fixture.*;
import java.lang.invoke.*;
import java.lang.reflect.*;
public class ConstructorInstanceReadHarness {
    static void require(boolean value) { if (!value) throw new AssertionError("owned constructor instance reads"); }
    static Core empty() { return new Core(null,null,0f,0L,0d,false,(byte)0,(char)0,(short)0,0,null,null,null); }
    static Values values(int id,boolean nullable) {
        Values v=new Values();v.object=nullable ? null : new Object();
        v.small=Float.intBitsToFloat(id<100 ? 0x7f800000 : id<200 ? (id%2==0 ? 0x80000000 : 0x7fc01234) : 0x3f800001+id);
        v.wide=0x1234567800000000L|id;
        v.real=Double.longBitsToDouble((id<200 ? 0x7ff8000000000000L : 0x3ff0000000000000L)|id);
        v.flag=id%2!=0;v.bite=(byte)(0x80|id);v.code=(char)(0xf000|id);v.half=(short)(0x8000|id);v.word=0x81230000|id;
        v.array=nullable ? null : new Object[]{v.object};return v;
    }
    static Values snapshot(Values value) {
        if (value==null) return null;
        Values result=new Values();
        result.object=value.object;
        result.small=value.small;
        result.wide=value.wide;
        result.real=value.real;
        result.flag=value.flag;
        result.bite=value.bite;
        result.code=value.code;
        result.half=value.half;
        result.word=value.word;
        result.array=value.array;
        return result;
    }
    static Core make(int route,boolean foreign,Object seed,Object[] array,Object context) throws Throwable {
        if (route==1) return ConstructorInstanceReadCaller.make(foreign,seed,array,context);
        Class<?> first=foreign ? Values.class : Core.class;
        if (route==0) {
            try { return (Core)Core.class.getConstructor(first,Object[].class,Object.class).newInstance(seed,array,context); }
            catch (InvocationTargetException failure) { throw failure.getCause(); }
        }
        MethodHandle handle=MethodHandles.publicLookup().findConstructor(Core.class,MethodType.methodType(void.class,first,Object[].class,Object.class));
        if (route==2) return (Core)handle.invokeWithArguments(seed,array,context);
        if (foreign) return (Core)handle.invokeExact((Values)seed,array,context);
        return (Core)handle.invokeExact((Core)seed,array,context);
    }
    public static void main(String[] args) throws Throwable {
        int route=Integer.parseInt(args[0]),mode=Integer.parseInt(args[1]);
        boolean foreign=mode>=12;
        Trace.active=false;Core seed=empty(),other=empty();
        for (int attempt=0;attempt<2;++attempt) {
            Trace.active=false;seed.set(values(10+mode,false));other.set(values(300+mode,false));
            Trace.afterGetter=mode==11 ? null : values(100+mode,mode==7);Trace.afterTarget=values(200+mode,false);
            Values expected=snapshot(Trace.afterGetter);
            Trace.name=new String("owned name "+mode);String expectedName=Trace.name;
            Trace.changedName=new String("changed name "+mode);
            Object[] inputArray=mode==9 ? null : new Object[]{new Object()};Object inputContext=mode==9 ? null : new Object();
            Trace.replacement=inputArray;Trace.context=inputContext;Trace.otherReplacement=new Object[]{new Object()};Trace.otherContext=new Object();
            Trace.getter=foreign ? seed : null;Trace.partial=null;Trace.current=seed;Trace.other=other;Trace.events=0;
            Trace.fail=mode==2 ? 1 : mode==3 ? 2 : mode==4 ? 3 : mode==5 ? 4 : mode==6 || mode==10 ? 5 : 0;
            Trace.failure=mode==3 || mode==5 || mode==10 ? new AssertionError("owned error") : new IllegalStateException("owned runtime");
            Trace.active=true;Object source=foreign ? (mode==13 ? null : Trace.afterGetter) : (mode==1 ? null : seed);
            Core value=null;Throwable failure=null;
            try { value=make(route,foreign,source,inputArray,inputContext); } catch(Throwable error) { failure=error; }
            boolean before=mode==1 || mode==2 || mode==3 || mode==11 || mode==13;
            long events=foreign ? (mode==13 ? 0 : 345) : mode==1 ? 0 : mode==2 || mode==11 ? 1 : mode==3 ? 12 : mode==4 ? 123 : mode==5 ? 1234 : 12345;
            require(Trace.events==events && (Trace.partial==null)==before);
            require(Trace.getter==(mode==1 ? null : seed));
            if (before) {
                require(value==null);
                if (mode==2 || mode==3) require(failure==Trace.failure);
                else require(failure instanceof NullPointerException);
                System.out.println("route="+route+",mode="+mode+",attempt="+attempt+",events="+Trace.events);continue;
            }
            Core result=Trace.partial;boolean started=Trace.fail!=3,complete=Trace.fail!=3 && Trace.fail!=4;
            require(result.name==(started ? expectedName : null) && result.object==(started ? expected.object : null));
            require(Float.floatToRawIntBits(result.small)==(complete ? Float.floatToRawIntBits(expected.small) : 0));
            require(result.wide==(complete ? expected.wide : 0));
            require(Double.doubleToRawLongBits(result.real)==(complete ? Double.doubleToRawLongBits(expected.real) : 0L));
            require(result.flag==(complete ? expected.flag : false));
            require(result.bite==(complete ? expected.bite : 0));
            require(result.code==(complete ? expected.code : 0));
            require(result.half==(complete ? expected.half : 0));
            require(result.word==(complete ? expected.word : 0));
            require(result.array==(complete ? expected.array : null));
            require(result.replacement==(complete ? inputArray : null) && result.context==(complete ? inputContext : null));
            if (Trace.fail!=0) require(value==null && failure==Trace.failure);
            else require(failure==null && value==result);
            System.out.println("route="+route+",mode="+mode+",attempt="+attempt+",events="+Trace.events);
        }
    }
}
