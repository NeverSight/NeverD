import fixture.*;
import java.lang.invoke.MethodHandles;
import java.lang.invoke.MethodType;
import java.lang.reflect.InvocationTargetException;
public class UnseededScalarCarrierHarness {
    static void require(boolean value) {
        if (!value) throw new AssertionError("owned unseeded scalar carrier contract");
    }
    static boolean call(int entry, int route, Carrier<?> values, Object input) throws Throwable {
        if (route == 0) {
            if (entry == 0) return Core.call(values, input);
            if (entry == 1) return Core.guarded(values, input);
            return Core.locked(values, input);
        }
        if (route == 1) return UnseededScalarCarrierCaller.call(entry, values, input);
        String name = entry == 0 ? "call" : entry == 1 ? "guarded" : "locked";
        if (route == 2) {
            try { return ((Boolean) Core.class.getMethod(name, Carrier.class, Object.class).invoke(null, values, input)).booleanValue(); }
            catch (InvocationTargetException failure) { throw failure.getCause(); }
        }
        return (boolean) MethodHandles.publicLookup().findStatic(Core.class, name,
                MethodType.methodType(boolean.class, Carrier.class, Object.class)).invoke(values, input);
    }
    public static void main(String[] args) throws Throwable {
        int entry = Integer.parseInt(args[0]), route = Integer.parseInt(args[1]), mode = Integer.parseInt(args[2]);
        require(Finder.class.getTypeParameters().length == 1);
        Trace.lock = new Object(); Trace.expectLock = entry == 2;
        Trace.fail = mode == 1 ? 1 : mode == 2 ? 2 : mode == 3 ? 3 : mode == 4 ? 4 : mode == 5 ? 9 : 0;
        Trace.failure = new IllegalArgumentException("owned scalar failure");
        Item item = new Item(); int[] data = {17}; Trace.token = data;
        Carrier<?> values = mode == 6 ? null : item;
        Object input = mode == 7 ? null : mode == 8 ? new Object() : data;
        for (int attempt = 0; attempt < 2; attempt++) {
            Trace.events = 0; Trace.lastFailure = null;
            int oldCalls = item.calls, oldData = data[0]; Object oldLast = item.last;
            boolean result = false; Throwable thrown = null;
            try { result = call(entry, route, values, input); } catch (Throwable failure) { thrown = failure; }
            long expected = attempt == 0 ? 9 : 0;
            if (mode != 5) expected = expected * 11 + 1;
            if (mode != 1 && mode != 5 && mode != 6) expected = expected * 11 + 2;
            boolean callback = mode != 1 && mode != 2 && mode != 5 && mode != 6;
            if (callback && mode != 8) expected = expected * 11 + 3;
            if (callback && mode != 3 && mode != 8) expected = expected * 11 + 4;
            boolean succeeded = mode == 0 || mode == 7;
            Throwable observed = entry == 1 ? Trace.lastFailure : thrown;
            if (succeeded) require(observed == null && result == (input == Trace.token));
            else {
                require(!result);
                if (mode == 5) {
                    if (attempt == 0) require(observed instanceof ExceptionInInitializerError && observed.getCause() == Trace.failure);
                    else require(observed instanceof NoClassDefFoundError);
                } else if (mode == 6) require(observed instanceof NullPointerException);
                else if (mode == 8) require(observed instanceof ClassCastException);
                else require(observed == Trace.failure);
            }
            require(entry != 1 || thrown == null);
            require(!Thread.holdsLock(Trace.lock) && Trace.events == expected);
            require(item.calls == oldCalls + (callback ? 1 : 0));
            require(item.last == (callback ? input : oldLast));
            require(data[0] == oldData + (callback && mode != 7 && mode != 8 ? 1 : 0));
            System.out.println("entry=" + entry + ",route=" + route + ",mode=" + mode
                    + ",attempt=" + attempt + ",trace=" + Trace.events);
        }
    }
}
