import fixture.*;
import java.lang.reflect.*;

public final class SourceDefaultReturnHarness {
    private static int values, identities, failures;
    private static boolean overriding;
    private static void check(boolean condition) { if (!condition) throw new AssertionError(); }
    private static final class Child extends Receiver<Object> {
        public double read() { calls += 2; Trace.mark(3); return value; }
    }
    private static Double call(Receiver<Object> receiver, int mode) throws Exception {
        if (mode == 0) return receiver.getValue();
        if (mode == 1) return ((Getter) receiver).getValue();
        if (mode == 2) return ((Value<Double>) receiver).getValue();
        if (mode == 3) return (Double)((Value) receiver).getValue();
        if (mode == 4) return ((Mirror<Double>) receiver).getValue();
        for (Method method : Getter.class.getDeclaredMethods())
            if (method.getName().equals("getValue") && method.getReturnType() == Double.class)
                return (Double)method.invoke(receiver);
        throw new AssertionError();
    }
    private static void exercise(Receiver<Object> receiver, int mode, double value) throws Exception {
        receiver.value = value;
        Trace.events = 0;
        int before = receiver.calls;
        int increment = receiver instanceof Child ? 2 : 1;
        int trace = (overriding ? 40 : 10) + (receiver instanceof Child ? 3 : 2);
        Double first = call(receiver,mode);
        check(receiver.calls == before + increment && Trace.events == trace);
        Trace.events = 0;
        Double second = call(receiver,mode);
        check(receiver.calls == before + increment * 2 && Trace.events == trace);
        check(first != second); identities++;
        check(Double.doubleToRawLongBits(first) == Double.doubleToRawLongBits(value)); values++;
        check(Double.doubleToRawLongBits(second) == Double.doubleToRawLongBits(value)); values++;
    }
    public static void main(String[] args) throws Exception {
        boolean disjoint = args.length != 0 && args[0].equals("disjoint");
        overriding = args.length != 0 && (args[0].equals("override") || disjoint);
        Receiver<Object> receiver = new Receiver<>();
        double[] inputs = {-0.0,0.0,Double.MIN_NORMAL,Double.MIN_VALUE,Double.MAX_VALUE,
            -Double.MAX_VALUE,Math.PI,Double.POSITIVE_INFINITY,Double.NEGATIVE_INFINITY,
            Double.longBitsToDouble(0x7ff8000000000042L)};
        for (double value : inputs) for (int mode = 0; mode < 6; mode++) exercise(receiver,mode,value);
        Child child = new Child();
        for (int mode = 0; mode < 6; mode++) exercise(child,mode,-0.0);
        RuntimeException sentinel = new IllegalStateException("owned");
        receiver.failure = sentinel;
        for (int mode = 0; mode < 6; mode++) {
            Trace.events = 0;
            int before = receiver.calls;
            try { call(receiver,mode); throw new AssertionError(); }
            catch (Throwable actual) {
                if (actual instanceof InvocationTargetException) actual = actual.getCause();
                check(actual == sentinel); identities++; failures++;
            }
            check(receiver.calls == before + 1 && Trace.events == (overriding ? 42 : 12));
        }
        check(Getter.class.getGenericInterfaces()[0].getTypeName().equals("fixture.Value<java.lang.Double>"));
        check(Receiver.class.getGenericInterfaces()[1].getTypeName().equals("fixture.Mirror<java.lang.Double>"));
        int reads = 0;
        for (Method method : Receiver.class.getDeclaredMethods()) {
            if (method.getName().equals("read")) { reads++; continue; }
            check(method.getName().equals("__neverdThrow") && Modifier.isPrivate(method.getModifiers()) &&
                Modifier.isStatic(method.getModifiers()) && method.getReturnType() == RuntimeException.class &&
                java.util.Arrays.equals(method.getParameterTypes(),new Class<?>[] {Throwable.class}));
        }
        check(reads == 1);
        Class<?> selected = overriding ? Class.forName("fixture.Refined") : Getter.class;
        check(Receiver.class.getMethod("getValue").getDeclaringClass() == selected);
        int bridges = 0;
        for (Method method : Getter.class.getDeclaredMethods())
            if (method.getName().equals("getValue")) {
                check(method.isDefault());
                if (method.getReturnType() == Object.class) {
                    check(method.isBridge() && method.isSynthetic()); bridges++;
                } else check(method.getGenericReturnType() == Double.class);
            }
        check(bridges == 1);
        if (overriding) {
            int refinedBridges = 0;
            for (Method method : selected.getDeclaredMethods()) {
                if (method.getName().equals("__neverdThrow")) {
                    check(Modifier.isPrivate(method.getModifiers()) && Modifier.isStatic(method.getModifiers()) &&
                        method.getReturnType() == RuntimeException.class &&
                        java.util.Arrays.equals(method.getParameterTypes(),new Class<?>[] {Throwable.class}));
                    continue;
                }
                check(method.getName().equals("getValue") && method.isDefault());
                if (method.getReturnType() == Object.class) {
                    check(method.isBridge() && method.isSynthetic()); refinedBridges++;
                } else check(method.getGenericReturnType() == Double.class);
            }
            check(refinedBridges == 1);
        }
        if (disjoint) {
            receiver.failure = null;
            Class<?> parent = Class.forName("fixture.Parent");
            Class<?> observer = Class.forName("fixture.ObserverRoot");
            Method event = observer.getMethod("event", int.class);
            Method adjust = observer.getMethod("adjust", int.class);
            check(parent.getMethod("event",int.class).getDeclaringClass() == parent);
            check(parent.getMethod("adjust",int.class).getDeclaringClass() == observer);
            for (int value : new int[] {-3,0,1,17,Integer.MIN_VALUE,Integer.MAX_VALUE}) {
                Trace.events = 0;
                event.invoke(receiver,value);
                check(parent.getField("observed").getInt(receiver) == value && Trace.events == 8); values++;
                Trace.events = 0;
                check(((Integer)adjust.invoke(receiver,value)).intValue() == value + 7 && Trace.events == 7); values++;
            }
        }
        System.out.println("values=" + values + ",identities=" + identities + ",failures=" + failures);
        System.out.println("source-default-owner=retained,full-result-substitution=retained,dispatch-and-effects=matched");
    }
}
