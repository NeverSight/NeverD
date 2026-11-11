import fixture.Core;
import fixture.Failure;
import fixture.Trace;
import java.lang.invoke.MethodHandle;
import java.lang.invoke.MethodHandles;
import java.lang.reflect.Constructor;
import java.lang.reflect.Field;
import java.lang.reflect.InvocationTargetException;
import java.lang.reflect.Modifier;

public class AbruptBlankFinalConstructorHarness {
    static void check(boolean condition) {
        if (!condition) throw new AssertionError("owned abrupt blank final constructor invariant");
    }

    @SuppressWarnings("unchecked")
    static <T extends Throwable> void raise(Throwable error) throws T { throw (T) error; }

    static Core invoke(int route, int shape, Throwable failure, Object value,
                       long number, boolean enabled) throws Throwable {
        if (route == 0 && shape == 2) return new Core(value, number, enabled);
        if (route == 1)
            return AbruptBlankFinalConstructorCaller.call(shape, failure, value, number, enabled);
        if (route == 2) {
            try {
                return (Core) AbruptBlankFinalConstructorCaller.class.getMethod("call",
                    int.class, Throwable.class, Object.class, long.class, boolean.class)
                    .invoke(null, shape, failure, value, number, enabled);
            } catch (InvocationTargetException error) { throw error.getCause(); }
        }
        Constructor<Core> constructor = shape == 0 ? Core.class.getDeclaredConstructor()
            : shape == 1 ? Core.class.getDeclaredConstructor(Throwable.class, long.class)
            : Core.class.getDeclaredConstructor(Object.class, long.class, boolean.class);
        constructor.setAccessible(true);
        if (route == 3) {
            MethodHandle handle = MethodHandles.lookup().unreflectConstructor(constructor);
            if (shape == 0) return (Core) handle.invokeExact();
            if (shape == 1) return (Core) handle.invokeExact(failure, number);
            return (Core) handle.invokeExact(value, number, enabled);
        }
        try {
            return shape == 0 ? constructor.newInstance()
                : constructor.newInstance(failure, number);
        } catch (InvocationTargetException error) { throw error.getCause(); }
    }

    public static void main(String[] args) throws Throwable {
        final int route = Integer.parseInt(args[0]);
        final int shape = Integer.parseInt(args[1]);
        final int mode = Integer.parseInt(args[2]);
        final Field valueField = Core.class.getField("value");
        final Field numberField = Core.class.getField("number");
        final Field enabledField = Core.class.getField("enabled");
        for (Field field : new Field[] {valueField, numberField, enabledField}) {
            check(Modifier.isFinal(field.getModifiers()) && !Modifier.isStatic(field.getModifiers()));
            field.setAccessible(true);
        }
        for (int attempt = 0; attempt != 2; ++attempt) {
            final Object value = mode == 1 ? null : new Object();
            final Object changed = new Object();
            final long number = mode == 9 ? Long.MIN_VALUE : 0x7f0135799abcdeffL + attempt;
            final boolean enabled = mode % 2 == 0;
            final Throwable supplied = mode == 1 ? null : mode == 2
                ? new IllegalArgumentException("owned input") : mode == 3
                ? new AssertionError("owned input") : new Exception("owned input");
            final Throwable callback = mode == 5 ? new IllegalStateException("owned callback")
                : mode == 6 ? new AssertionError("owned callback") : new Exception("owned callback");
            final int stop = mode >= 4 && mode <= 7 ? mode - 3 : -1;
            final Object[] expectedValue = {null};
            final long[] expectedNumber = {0};
            final boolean[] expectedEnabled = {false};
            final int[] seen = {0};
            Trace.owner = null;
            Trace.failure = null;
            Trace.events = 0;
            Trace.action = new Runnable() {
                public void run() {
                    int event = (int) (Trace.events % 10);
                    check(Trace.owner instanceof Core);
                    int expectedEvent = seen[0] == 0 ? 1 : seen[0] == 1 ? 2 : shape == 0 ? 3 : 4;
                    check(event == expectedEvent && seen[0] < (shape == 1 ? 2 : 3));
                    ++seen[0];
                    if (event == 2 && shape == 1) expectedNumber[0] = number;
                    if (event == 4) {
                        expectedValue[0] = value;
                        expectedNumber[0] = number;
                        expectedEnabled[0] = enabled;
                    }
                    Core owner = (Core) Trace.owner;
                    check(owner.value == expectedValue[0] && owner.number == expectedNumber[0]
                        && owner.enabled == expectedEnabled[0]);
                    check(event == 3 ? Trace.failure instanceof Failure : Trace.failure == null);
                    if (event == 1 && mode == 11) {
                        try {
                            valueField.set(owner, changed);
                            numberField.setLong(owner, -17L);
                            enabledField.setBoolean(owner, true);
                        } catch (ReflectiveOperationException error) { throw new AssertionError(error); }
                        expectedValue[0] = changed;
                        expectedNumber[0] = -17L;
                        expectedEnabled[0] = true;
                    }
                    if (event == stop) AbruptBlankFinalConstructorHarness.<RuntimeException>raise(callback);
                }
            };
            Core result = null;
            Throwable caught = null;
            try { result = invoke(route, shape, supplied, value, number, enabled); }
            catch (Throwable error) { caught = error; }
            boolean stopped = stop == 1 || stop == 2 || (stop == 3 && shape == 0) || (stop == 4 && shape == 2);
            if (stopped) check(caught == callback && result == null);
            else if (shape == 0) check(caught == Trace.failure && caught instanceof Failure && result == null);
            else if (shape == 1) check(result == null && (supplied == null
                ? caught != null && caught.getClass() == NullPointerException.class : caught == supplied));
            else check(caught == null && result == Trace.owner);
            long events = stop == 1 ? 1 : stop == 2 ? 12 : shape == 0 ? 123 : shape == 1 ? 12 : 124;
            check(Trace.events == events);
            Core owner = (Core) Trace.owner;
            check(owner.value == expectedValue[0] && owner.number == expectedNumber[0]
                && owner.enabled == expectedEnabled[0]);
            System.out.println("route=" + route + ",shape=" + shape + ",mode=" + mode
                + ",attempt=" + attempt + ",events=" + Trace.events);
        }
    }
}
