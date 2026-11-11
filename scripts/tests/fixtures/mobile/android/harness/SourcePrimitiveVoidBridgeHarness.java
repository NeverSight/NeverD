import fixture.*;
import java.lang.reflect.*;

@SuppressWarnings({"rawtypes", "unchecked"})
public final class SourcePrimitiveVoidBridgeHarness {
    private static int values, identities, failures;
    private static void check(boolean condition) { if (!condition) throw new AssertionError(); }
    private static final class Child extends Receiver<Object> {
        public void set(double value) { Trace.mark(3); super.set(value); }
    }
    private static final class Observed extends Number {
        final double value;
        final RuntimeException failure;
        Observed(double value, RuntimeException failure) { this.value = value; this.failure = failure; }
        public double doubleValue() { Trace.mark(9); if (failure != null) throw failure; return value; }
        public int intValue() { throw new AssertionError(); }
        public long longValue() { throw new AssertionError(); }
        public float floatValue() { throw new AssertionError(); }
    }
    private static void call(Receiver<Object> receiver, Object value, int mode) throws Exception {
        if (mode == 0) receiver.set(value);
        else if (mode == 1) ((Setter)receiver).set(value);
        else if (mode == 2) ((Value)receiver).set(value);
        else if (mode == 3) ((Mirror)receiver).set(value);
        else if (mode == 4) Setter.class.getMethod("set", Object.class).invoke(receiver, value);
        else Value.class.getMethod("set", Object.class).invoke(receiver, value);
    }
    private static void exercise(Receiver<Object> receiver, Object input, double expected, int mode) throws Exception {
        Trace.events = 0;
        int before = receiver.calls;
        call(receiver, input, mode);
        int trace = receiver instanceof Child ? 312 : 12;
        if (input instanceof Observed) trace += receiver instanceof Child ? 9000 : 900;
        check(Trace.events == trace && receiver.calls == before + 1);
        check(Double.doubleToRawLongBits(receiver.value) == Double.doubleToRawLongBits(expected)); values++;
        Double first = receiver.get(), second = ((Value<Double>)receiver).get();
        check(Double.doubleToRawLongBits(first) == Double.doubleToRawLongBits(expected)); values++;
        check(Double.doubleToRawLongBits(second) == Double.doubleToRawLongBits(expected)); values++;
        check(first != second); identities++;
    }
    private static void fails(Receiver<Object> receiver, Object input, int mode, Class<?> type,
                              Throwable sentinel, int trace, int increment) throws Exception {
        receiver.value = -0.0;
        Trace.events = 0;
        int before = receiver.calls;
        try { call(receiver,input,mode); throw new AssertionError(); }
        catch (Throwable actual) {
            if (actual instanceof InvocationTargetException) actual = actual.getCause();
            check(type.isInstance(actual));
            if (sentinel != null) { check(actual == sentinel); identities++; }
            failures++;
        }
        check(receiver.calls == before + increment && Trace.events == trace);
        check(Double.doubleToRawLongBits(receiver.value) == Double.doubleToRawLongBits(-0.0)); values++;
    }
    public static void main(String[] args) throws Exception {
        Receiver<Object> receiver = new Receiver<>();
        Child child = new Child();
        double[] inputs = {-0.0,0.0,Double.MIN_VALUE,Double.MIN_NORMAL,Double.MAX_VALUE,
            Math.PI,Double.POSITIVE_INFINITY,Double.NEGATIVE_INFINITY,
            Double.longBitsToDouble(0x7ff8000000000042L)};
        for (double value : inputs) for (int mode = 0; mode < 6; mode++) {
            exercise(receiver, Double.valueOf(value), value, mode);
            exercise(child, new Observed(value,null), value, mode);
        }
        Number[] broad = {Integer.valueOf(17),Long.valueOf(9007199254740991L),
            Float.valueOf(-0.0f),Short.valueOf((short)-5),Byte.valueOf((byte)6)};
        for (Number value : broad) for (int mode = 0; mode < 6; mode++)
            exercise(receiver,value,value.doubleValue(),mode);
        RuntimeException conversion = new IllegalArgumentException("conversion");
        RuntimeException sink = new IllegalStateException("sink");
        for (int mode = 0; mode < 6; mode++) {
            fails(receiver,null,mode,NullPointerException.class,null,0,0);
            fails(receiver,new Object(),mode,ClassCastException.class,null,0,0);
            fails(child,new Observed(3.0,conversion),mode,conversion.getClass(),conversion,9,0);
            Trace.failure = sink;
            fails(child,new Observed(3.0,null),mode,sink.getClass(),sink,9312,1);
            Trace.failure = null;
        }
        Typed typed = new Typed();
        Double boxed = Double.valueOf(29);
        Trace.events = 0;
        ((Value)typed).set(boxed);
        check(typed.value == boxed && Trace.events == 4); identities++;
        Trace.events = 0;
        try { ((Value)typed).set(Integer.valueOf(29)); throw new AssertionError(); }
        catch (ClassCastException expected) { failures++; }
        check(typed.value == boxed && Trace.events == 0); identities++;
        GenericReceiver<Object> generic = new GenericReceiver<>();
        Object token = new Object();
        Trace.events = 0;
        ((Value)generic).set(token);
        check(generic.value == token && generic.get() == token && Trace.events == 5); identities++;
        check(Setter.class.getGenericInterfaces()[0].getTypeName().equals("fixture.Value<java.lang.Double>"));
        check(Receiver.class.getGenericInterfaces()[1].getTypeName().equals("fixture.Mirror<java.lang.Double>"));
        check(Value.class.getMethod("get").getGenericReturnType() instanceof TypeVariable);
        boolean projected = args.length != 0 && args[0].equals("recovered");
        check((Value.class.getMethod("set",Object.class).getGenericParameterTypes()[0] == Object.class) == projected);
        check((GenericReceiver.class.getMethod("set",Object.class).getGenericParameterTypes()[0] == Object.class) == projected);
        Method setter = Setter.class.getMethod("set",Object.class);
        check(setter.isDefault() && setter.isBridge() != projected && setter.isSynthetic() != projected);
        check(Receiver.class.getMethod("set",Object.class).getDeclaringClass() == Setter.class);
        System.out.println("values=" + values + ",identities=" + identities + ",failures=" + failures);
        System.out.println("broad-number-unboxing=retained,wide-default-dispatch-and-effects=matched,parameter-reflection=reported");
    }
}
