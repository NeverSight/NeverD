import fixture.*;
import java.lang.invoke.*;
import java.lang.reflect.*;
public class ConstructorComparisonHarness {
    static void require(boolean value) { if (!value) throw new AssertionError("owned constructor comparisons"); }
    static Core make(int route,Class<?>[] types,Object[] values) throws Throwable {
        if (route==1) return ConstructorComparisonCaller.make(types,values);
        if (route==0) {
            try { return (Core)Core.class.getConstructor(types).newInstance(values); }
            catch (InvocationTargetException failure) { throw failure.getCause(); }
        }
        MethodHandle handle=MethodHandles.publicLookup().findConstructor(Core.class,MethodType.methodType(void.class,types));
        if (route==2) return (Core)handle.invokeWithArguments(values);
        if (types.length==1) return (Core)handle.invokeExact(((Double)values[0]).doubleValue());
        if (types[0]==String.class) return (Core)handle.invokeExact((String)values[0],((Double)values[1]).doubleValue());
        if (types[0]==long.class) return (Core)handle.invokeExact(((Long)values[0]).longValue(),((Long)values[1]).longValue());
        if (types[0]==float.class) return (Core)handle.invokeExact(((Float)values[0]).floatValue(),((Float)values[1]).floatValue());
        return (Core)handle.invokeExact(((Double)values[0]).doubleValue(),((Double)values[1]).doubleValue());
    }
    static void reset() { Trace.events=0;Trace.calls=0;Trace.allocations=0;Trace.partial=null;Trace.created=null;Trace.firstBits=0;Trace.secondBits=0; }
    public static void main(String[] args) throws Throwable {
        int route=Integer.parseInt(args[0]),mode=Integer.parseInt(args[1]);
        if (mode<3) {
            long[][] longs={{Long.MIN_VALUE,Long.MAX_VALUE},{Long.MAX_VALUE,Long.MIN_VALUE},{Long.MIN_VALUE,Long.MIN_VALUE},{0,-1},{-1,0},{Long.MAX_VALUE,Long.MAX_VALUE}};
            int[][] floats={{0x80000000,0},{0,0x80000000},{0x7fc01234,0x3f800000},{0x3f800000,0x7fc04321},{0xff800000,0x7f800000},{0x7f800000,0xff800000},{1,0},{0x80000001,0x80000000},{0x7fc01234,0x7fc04321}};
            long[][] doubles={{0x8000000000000000L,0},{0,0x8000000000000000L},{0x7ff8000000001234L,0x3ff0000000000000L},{0x3ff0000000000000L,0x7ff8000000004321L},{0xfff0000000000000L,0x7ff0000000000000L},{0x7ff0000000000000L,0xfff0000000000000L},{1,0},{0x8000000000000001L,0x8000000000000000L},{0x7ff8000000001234L,0x7ff8000000004321L}};
            int[] expected=mode==0 ? new int[]{-1,1,0,1,-1,0} : new int[]{0,0,1,1,-1,1,1,-1,1};
            Class<?> type=mode==0 ? long.class : mode==1 ? float.class : double.class;
            Trace.fail=0;
            for (int attempt=0;attempt<2;attempt++) {
                for (int i=0;i<expected.length;i++) {
                    reset();Object first,second;
                    if (mode==0) { first=Long.valueOf(longs[i][0]);second=Long.valueOf(longs[i][1]); }
                    else if (mode==1) { first=Float.valueOf(Float.intBitsToFloat(floats[i][0]));second=Float.valueOf(Float.intBitsToFloat(floats[i][1])); }
                    else { first=Double.valueOf(Double.longBitsToDouble(doubles[i][0]));second=Double.valueOf(Double.longBitsToDouble(doubles[i][1])); }
                    Core value=make(route,new Class<?>[]{type,type},new Object[]{first,second});
                    require(value==Trace.partial && value.result==expected[i] && value.selected==null && Trace.events==34 && Trace.calls==0 && Trace.allocations==0);
                }
                System.out.println("route="+route+",mode="+mode+",attempt="+attempt+",events="+Trace.events);
            }
            return;
        }
        int index=mode-3;
        long[] bits={0x4000000000000000L,0,0x8000000000000000L,0x7ff8000000001234L,0x7ff0000000000000L,0xfff0000000000000L,0x3ff0000000000001L,0x4008000000000001L,0x7ff8000000004321L,0x8000000000000000L,0x7ff0000000000000L,0xfff0000000000000L,0x3fd0000000000001L};
        boolean firstEqual=index==0 || index==3 || index>=5;
        boolean secondEqual=index==0 || index==2 || index==5;
        int[] failures={0,0,0,0,0,0,1,2,5,6,3,4,0};
        Trace.fail=failures[index];Trace.failure=index%2==0 ? new IllegalStateException("owned runtime") : new AssertionError("owned error");
        Value previous=null;
        for (int attempt=0;attempt<2;attempt++) {
            reset();Trace.identity=new Object();Trace.first=firstEqual ? 2.0 : Double.NaN;Trace.second=secondEqual ? 2.0 : Double.NaN;
            double input=Double.longBitsToDouble(bits[index]);
            Class<?>[] types=index==12 ? new Class<?>[]{String.class,double.class} : new Class<?>[]{double.class};
            Object[] values=index==12 ? new Object[]{"owned wrapper",input} : new Object[]{input};
            Core value=null;Throwable failure=null;
            try { value=make(route,types,values); } catch(Throwable error) { failure=error; }
            long events=Trace.fail==1 ? 1 : Trace.fail==2 ? 12 : secondEqual ? 1234 : Trace.fail==5 ? 125 : Trace.fail==6 ? 1256 : Trace.fail==3 ? 12563 : 125634;
            require(Trace.events==events && Trace.calls==(Trace.fail==1 ? 1 : 2));
            require(Trace.firstBits==bits[index] && (Trace.fail==1 || Trace.secondBits==bits[index]));
            boolean allocated=!secondEqual && Trace.fail!=1 && Trace.fail!=2;
            require(Trace.allocations==(allocated ? 1 : 0) && (Trace.created!=null)==allocated);
            if (allocated) {
                require(Trace.created!=previous);previous=Trace.created;
                require(Double.doubleToRawLongBits(Trace.created.input)==(Trace.fail==5 ? 0 : bits[index]));
            }
            boolean target=Trace.fail!=1 && Trace.fail!=2 && Trace.fail!=5 && Trace.fail!=6;
            require((Trace.partial!=null)==target);
            if (target) {
                require(Trace.partial.result==(Trace.fail==3 ? 0 : firstEqual ? 7 : 9));
                require(Trace.partial.selected==(Trace.fail==3 ? null : secondEqual ? Trace.identity : Trace.created));
            }
            if (Trace.fail==0) require(failure==null && value==Trace.partial);
            else require(value==null && failure==Trace.failure);
            System.out.println("route="+route+",mode="+mode+",attempt="+attempt+",events="+Trace.events);
        }
    }
}
