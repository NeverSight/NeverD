import fixture.*;
import java.lang.invoke.MethodHandles;
import java.lang.invoke.MethodType;
import java.lang.reflect.InvocationTargetException;
import java.util.Arrays;
public class GenericVoidArrayWritersHarness {
    static void require(boolean value) {
        if (!value) throw new AssertionError("owned generic void array writer contract");
    }
    static Object[] call(int route, Object[] values, Object value, int begin, int end) throws Throwable {
        if (route == 0) return Core.fill(values, value, begin, end);
        if (route == 1) return GenericVoidArrayWritersCaller.fill(values, value, begin, end);
        if (route == 2) {
            try { return (Object[]) Core.class.getMethod("fill", Object[].class, Object.class, int.class, int.class)
                    .invoke(null, values, value, begin, end); }
            catch (InvocationTargetException failure) { throw failure.getCause(); }
        }
        return (Object[]) MethodHandles.publicLookup().findStatic(Core.class, "fill", MethodType.methodType(Object[].class,
                Object[].class, Object.class, int.class, int.class)).invokeExact(values, value, begin, end);
    }
    public static void main(String[] args) throws Throwable {
        int route = Integer.parseInt(args[0]), mode = Integer.parseInt(args[1]);
        Trace.fail = mode >= 1 && mode <= 4 ? mode : 0;
        Trace.failure = new IllegalArgumentException("owned void array writer failure");
        Object a = new String("a"), b = new String("b"), c = new String("c"), marker = new Object();
        Object[] values = mode == 5 ? null : mode == 11 ? new String[]{(String)a, (String)b, (String)c}
                : new Object[]{a, b, c};
        Object value = mode == 6 ? null : mode == 10 ? values : mode == 11 ? marker : new String("filled");
        int begin = mode == 7 ? -1 : mode == 9 ? 2 : 0, end = mode == 8 ? 4 : mode == 9 ? 1 : mode == 12 ? 0 : 3;
        for (int attempt = 0; attempt < 2; attempt++) {
            Trace.events = 0; Trace.source = marker; Trace.destination = marker; Trace.result = marker;
            Object[] expected = values == null ? null : values.clone();
            Throwable expectedFailure = null;
            if (mode == 0 || mode >= 5) {
                try { Arrays.fill(expected, begin, end, value); }
                catch (Throwable failure) { expectedFailure = failure; }
            } else if (mode == 3 || mode == 4) Arrays.fill(expected, begin, end, value);
            Object[] result = null; Throwable failure = null;
            try { result = call(route, values, value, begin, end); } catch (Throwable error) { failure = error; }
            long events = attempt == 0 ? 1 : 0;
            boolean body = mode != 1;
            if (body) events = events * 11 + 2;
            boolean filled = body && mode != 2 && expectedFailure == null;
            if (filled) events = events * 11 + 3;
            boolean returned = filled && mode != 3;
            if (returned) events = events * 11 + 4;
            require(Trace.events == events && Trace.source == (body ? values : marker)
                    && Trace.destination == (body ? value : marker));
            if (values != null) require(Arrays.equals(values, expected));
            require(Trace.result == (returned ? values : marker));
            if (mode == 1) {
                if (attempt == 0) require(failure instanceof ExceptionInInitializerError && failure.getCause() == Trace.failure);
                else require(failure instanceof NoClassDefFoundError);
            } else if (mode >= 2 && mode <= 4) require(failure == Trace.failure);
            else if (expectedFailure != null) require(failure != null && failure.getClass() == expectedFailure.getClass());
            else require(failure == null && result == values);
            System.out.println("route=" + route + ",mode=" + mode + ",attempt=" + attempt + ",trace=" + Trace.events);
        }
    }
}
