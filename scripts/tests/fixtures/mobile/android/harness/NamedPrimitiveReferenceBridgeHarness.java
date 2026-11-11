import fixture.*;
import java.lang.reflect.*;

public final class NamedPrimitiveReferenceBridgeHarness {
    private static int values, identities, failures;
    private static void identity(Object actual, Object expected) {
        if (actual != expected) throw new AssertionError("named bridge reference changed");
        identities++;
    }
    private static void effects(int expected, float first, float second) {
        if (Trace.events != expected || Trace.first != Float.floatToRawIntBits(first)
                || Trace.second != Float.floatToRawIntBits(second))
            throw new AssertionError("named bridge bits or effects changed");
        values++;
        Trace.events = 0;
    }
    private static void failure(Runnable call, Class<? extends Throwable> kind,
                                RuntimeException same, int expected) {
        try { call.run(); throw new AssertionError("failure missing"); }
        catch (Throwable e) {
            if (e.getClass() != kind || (same != null && e != same))
                throw new AssertionError("named bridge exception changed", e);
            if (same != null) identities++;
            failures++;
        }
        if (Trace.events != expected) throw new AssertionError("failure order changed");
        Trace.events = 0;
    }
    private static final class CountingNumber extends Number {
        final int event; final float value;
        CountingNumber(int event, float value) { this.event = event; this.value = value; }
        public float floatValue() {
            Trace.events = Trace.events * 10 + event;
            if (event == 1 && Trace.firstFailure != null) throw Trace.firstFailure;
            if (event == 2 && Trace.secondFailure != null) throw Trace.secondFailure;
            return value;
        }
        public int intValue() { throw new AssertionError("wrong integer conversion"); }
        public long longValue() { throw new AssertionError("wrong wide conversion"); }
        public double doubleValue() { return value; }
    }
    private static final class Sink implements Callback<Float> {
        public void accept(Float value) {
            if (Float.floatToRawIntBits(value.floatValue()) != Trace.first)
                throw new AssertionError("callback argument changed");
            Trace.events = Trace.events * 10 + 4;
            if (Trace.boxFailure != null) throw Trace.boxFailure;
        }
    }
    @SuppressWarnings({"rawtypes", "unchecked"})
    public static void main(String[] args) throws Exception {
        Receiver<Object> receiver = new Receiver<>(); Function api = receiver;
        float nan = Float.intBitsToFloat(0x7fc01234);
        float[][] pairs = {{0,-0.0f},{Float.MIN_VALUE,Float.MAX_VALUE},
                           {Float.POSITIVE_INFINITY,Float.NEGATIVE_INFINITY},{nan,73.5f},
                           {-91.75f,12.5f}};
        for (Object output : new Object[]{new Object(), "text", null})
            for (Context context : new Context[]{new Context(), null})
                for (Callback sink : new Callback[]{new Sink(), null})
                    for (Box box : new Box[]{new Box<Float,Vector>(Float.valueOf(5),new Vector()),null})
                        for (float[] pair : pairs) {
                            receiver.result = output;
                            CountingNumber first = new CountingNumber(1,pair[0]);
                            CountingNumber second = new CountingNumber(2,pair[1]);
                            identity(receiver.apply(context,pair[0],pair[1],sink,box),output);
                            identity(receiver.context,context); identity(receiver.callback,sink);
                            identity(receiver.box,box); effects(sink == null ? 3 : 34,pair[0],pair[1]);
                            identity(receiver.apply(context,(Object)first,(Object)second,sink,box),output);
                            identity(receiver.context,context); identity(receiver.callback,sink);
                            identity(receiver.box,box); effects(sink == null ? 123 : 1234,pair[0],pair[1]);
                            identity(api.apply(context,first,second,sink,box),output);
                            identity(receiver.context,context); identity(receiver.callback,sink);
                            identity(receiver.box,box); effects(sink == null ? 123 : 1234,pair[0],pair[1]);
                        }
        CountingNumber first = new CountingNumber(1,7), second = new CountingNumber(2,9);
        failure(() -> api.apply(null,new Object(),second,null,null),ClassCastException.class,null,0);
        failure(() -> api.apply(null,null,second,null,null),NullPointerException.class,null,0);
        failure(() -> api.apply(null,first,new Object(),null,null),ClassCastException.class,null,1);
        failure(() -> api.apply(null,first,null,null,null),NullPointerException.class,null,1);
        RuntimeException problem = new IllegalStateException("owned named conversion failure");
        Trace.firstFailure = problem;
        failure(() -> api.apply(null,first,second,null,null),IllegalStateException.class,problem,1);
        Trace.firstFailure = null; Trace.secondFailure = problem;
        failure(() -> api.apply(null,first,second,null,null),IllegalStateException.class,problem,12);
        Trace.secondFailure = null; Trace.targetFailure = problem;
        failure(() -> api.apply(null,first,second,null,null),IllegalStateException.class,problem,123);
        Trace.targetFailure = null; Trace.boxFailure = problem;
        failure(() -> api.apply(null,first,second,new Sink(),null),IllegalStateException.class,problem,1234);
        Trace.boxFailure = null;
        Function absent = null;
        failure(() -> absent.apply(null,null,null,null,null),NullPointerException.class,null,0);
        Method target = Receiver.class.getMethod("apply",Context.class,float.class,float.class,Callback.class,Box.class);
        Type[] boxArgs = ((ParameterizedType) target.getGenericParameterTypes()[4]).getActualTypeArguments();
        if (boxArgs[0] != Float.class || boxArgs[1] != Vector.class)
            throw new AssertionError("complete bounded target parameter changed");
        if (Function.class.getTypeParameters()[1].getBounds()[0] != Bound.class)
            throw new AssertionError("interface bound changed");
        boolean original = args.length != 0 && args[0].equals("original");
        Method bridge = Receiver.class.getMethod("apply",Context.class,Object.class,Object.class,Callback.class,Box.class);
        if (bridge.isBridge() != original || bridge.isSynthetic() != original)
            throw new AssertionError("bridge flags differ from report");
        Type parent = Receiver.class.getGenericInterfaces()[0];
        if (original) {
            Type[] arguments = ((ParameterizedType) parent).getActualTypeArguments();
            if (arguments[0] != Float.class || arguments[1] != Vector.class)
                throw new AssertionError("original bounded interface changed");
        } else if (parent != Function.class) throw new AssertionError("reported raw interface missing");
        System.out.println("values=" + values + ",identities=" + identities + ",failures=" + failures);
        System.out.println(original ? "reflection=original" : "reflection=reported-raw-interface");
    }
}
