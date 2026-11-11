import fixture.*;
import java.lang.invoke.MethodHandles;
import java.lang.invoke.MethodType;
import java.lang.reflect.InvocationTargetException;
import java.util.Arrays;
public class GenericArrayTransfersHarness {
    static void require(boolean value) {
        if (!value) throw new AssertionError("owned generic array transfer contract");
    }
    static Object[] call(int route, Object[] source, Object[] destination, int offset, int begin, int end) throws Throwable {
        if (route == 0) return Core.move(source, destination, offset, begin, end);
        if (route == 1) return GenericArrayTransfersCaller.move(source, destination, offset, begin, end);
        if (route == 2) {
            try { return (Object[]) Core.class.getMethod("move", Object[].class, Object[].class, int.class, int.class, int.class)
                    .invoke(null, source, destination, offset, begin, end); }
            catch (InvocationTargetException failure) { throw failure.getCause(); }
        }
        return (Object[]) MethodHandles.publicLookup().findStatic(Core.class, "move", MethodType.methodType(Object[].class,
                Object[].class, Object[].class, int.class, int.class, int.class)).invoke(source, destination, offset, begin, end);
    }
    public static void main(String[] args) throws Throwable {
        int route = Integer.parseInt(args[0]), mode = Integer.parseInt(args[1]);
        Trace.fail = mode >= 1 && mode <= 4 ? mode : 0;
        Trace.failure = new IllegalArgumentException("owned array transfer failure");
        Object a = new String("a"), b = new String("b"), c = new String("c"), marker = new Object();
        Object[] source = mode == 5 ? null : mode == 11 ? new Object[]{a, marker, c} : new Object[]{a, b, c};
        Object[] destination = mode == 6 ? null : mode == 10 ? source : mode == 11 ? new String[]{null, null, null, null}
                : new Object[]{marker, marker, marker, marker};
        int begin = mode == 7 ? -1 : 0, end = mode == 8 ? 4 : mode == 12 ? 0 : 3, offset = mode == 9 ? 2 : 1;
        for (int attempt = 0; attempt < 2; attempt++) {
            Trace.events = 0; Trace.source = marker; Trace.destination = marker; Trace.result = marker;
            Object[] expected = destination == null ? null : destination.clone();
            Throwable expectedFailure = null;
            if (mode == 0 || mode >= 5) {
                try { System.arraycopy(source, begin, expected, offset, end - begin); }
                catch (Throwable failure) { expectedFailure = failure; }
            } else if (mode == 3 || mode == 4) System.arraycopy(source, begin, expected, offset, end - begin);
            Object[] result = null; Throwable failure = null;
            try { result = call(route, source, destination, offset, begin, end); } catch (Throwable error) { failure = error; }
            long events = attempt == 0 ? 1 : 0;
            boolean body = mode != 1;
            if (body) events = events * 11 + 2;
            boolean copied = body && mode != 2 && expectedFailure == null;
            if (copied) events = events * 11 + 3;
            boolean returned = copied && mode != 3;
            if (returned) events = events * 11 + 4;
            require(Trace.events == events && Trace.source == (body ? source : marker)
                    && Trace.destination == (body ? destination : marker));
            if (destination != null) require(Arrays.equals(destination, expected));
            require(Trace.result == (returned ? destination : marker));
            if (mode == 1) {
                if (attempt == 0) require(failure instanceof ExceptionInInitializerError && failure.getCause() == Trace.failure);
                else require(failure instanceof NoClassDefFoundError);
            } else if (mode >= 2 && mode <= 4) require(failure == Trace.failure);
            else if (expectedFailure != null) require(failure != null && failure.getClass() == expectedFailure.getClass());
            else require(failure == null && result == destination);
            System.out.println("route=" + route + ",mode=" + mode + ",attempt=" + attempt + ",trace=" + Trace.events);
        }
    }
}
