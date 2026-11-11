import fixture.*;
import java.lang.invoke.*;
import java.lang.reflect.*;
public final class SyntheticMethodHarness {
    static void require(boolean ok) { if (!ok) throw new AssertionError("synthetic method contract"); }
    public static void main(String[] args) throws Throwable {
        int operation = Integer.parseInt(args[0]), route = Integer.parseInt(args[1]);
        int mode = Integer.parseInt(args[2]);
        String original = args[3];
        Trace.failAt = mode; Trace.failure = new IllegalArgumentException("owned method failure");
        int[] sequence = operation == 0 ? new int[] {1,2} : operation == 1 ? new int[] {1,3,2}
                         : operation == 2 ? new int[] {1,6,2,5} : new int[] {1,4};
        long expected = 0;
        Box previous = null;
        for (int attempt = 1; attempt <= 2; attempt++) {
            Object value = attempt == 1 ? new int[] {7} : null;
            long bits = attempt == 1 ? Long.MIN_VALUE : Long.MAX_VALUE;
            Throwable caught = null;
            try {
                Object result = null; long scalar = 0;
                if (route == 0) {
                    if (operation == 0) result = Provider.relay(value);
                    else if (operation == 1) result = Provider.effect(value);
                    else if (operation == 2) result = Provider.allocate(value);
                    else scalar = Provider.wideRelay(bits);
                } else if (route == 1) {
                    if (operation == 0) result = SyntheticMethodCaller.relay(value);
                    else if (operation == 1) result = SyntheticMethodCaller.effect(value);
                    else if (operation == 2) result = SyntheticMethodCaller.allocate(value);
                    else scalar = SyntheticMethodCaller.wide(bits);
                } else {
                    // The immutable caller receives the disclosed name because
                    // Java cannot preserve the original illegal runtime name.
                    String name = operation == 0 || operation == 3 ? original
                                  : operation == 1 ? "effect" : "allocate";
                    Class<?> parameter = operation == 3 ? long.class : Object.class;
                    Class<?> returns = operation == 3 ? long.class : operation == 2 ? Box.class : Object.class;
                    if (route == 2) {
                        Method method = Provider.class.getDeclaredMethod(name, parameter);
                        try {
                            Object answer = method.invoke(null, operation == 3 ? Long.valueOf(bits) : value);
                            if (operation == 3) scalar = ((Long) answer).longValue(); else result = answer;
                        } catch (InvocationTargetException failure) { throw failure.getCause(); }
                    } else {
                        MethodHandle method = MethodHandles.publicLookup().findStatic(Provider.class, name,
                            MethodType.methodType(returns, parameter));
                        if (operation == 3) scalar = (long) method.invokeExact(bits);
                        else if (operation == 2) result = (Box) method.invokeExact(value);
                        else result = (Object) method.invokeExact(value);
                    }
                }
                require(mode == 0);
                if (operation == 3) require(scalar == bits);
                else if (operation == 2) {
                    require(result instanceof Box && ((Box) result).value == value && result != previous);
                    previous = (Box) result;
                } else require(result == value);
            } catch (Throwable failure) { caught = failure; }
            if (mode == 0) require(caught == null);
            else if (mode == 1 || mode == 6) {
                if (attempt == 1) require(caught instanceof ExceptionInInitializerError && caught.getCause() == Trace.failure);
                else require(caught instanceof NoClassDefFoundError);
            } else require(caught == Trace.failure);
            if (attempt == 1 || (mode != 1 && mode != 6))
                for (int event : sequence) {
                    if (attempt == 2 && (event == 1 || event == 6)) continue;
                    expected = expected * 10 + event;
                    if (event == mode) break;
                }
            require(Trace.events == expected);
        }
        System.out.println("operation=" + operation + ",route=" + route + ",mode=" + mode + ",trace=" + Trace.events);
    }
}
