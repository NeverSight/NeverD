import fixture.*;
import java.lang.invoke.*;
import java.lang.reflect.*;
public final class FinalThrowHarness {
    static void require(boolean value) { if (!value) throw new AssertionError("final abrupt-exit contract"); }
    static Core create(int route, Object value) throws Throwable {
        try {
            if (route == 0) return new Core(value);
            if (route == 1) return FinalThrowCaller.create(value);
            if (route == 2) return Core.class.getConstructor(Object.class).newInstance(value);
            return (Core) MethodHandles.publicLookup().findConstructor(Core.class,
                MethodType.methodType(void.class, Object.class)).invokeExact(value);
        } catch (InvocationTargetException failure) { throw failure.getCause(); }
    }
    static Object read(int route) throws Throwable {
        if (route == 0) return StaticCore.second;
        if (route == 1) return FinalThrowCaller.read();
        if (route == 2) return StaticCore.class.getField("second").get(null);
        return (Object) MethodHandles.publicLookup().findStaticGetter(StaticCore.class, "second", Object.class).invokeExact();
    }
    public static void main(String[] args) throws Throwable {
        boolean statics = Boolean.parseBoolean(args[0]);
        int route = Integer.parseInt(args[1]), mode = Integer.parseInt(args[2]);
        Trace.mode = mode; Trace.inspectStatic = statics;
        Trace.failure = mode == 2 ? null : new IllegalArgumentException("owned final-write failure");
        Object first = new int[] {3};
        Trace.value = first;
        Core previous = null;
        long expected = 0;
        for (int attempt = 1; attempt <= 2; attempt++) {
            Object value = attempt == 1 ? first : null;
            Throwable caught = null;
            try {
                if (statics) {
                    Object result = read(route);
                    require(mode == 0 && result == first && StaticCore.first == first);
                } else {
                    Core result = create(route, value);
                    require(mode == 0 && result.first == value && result.second == value && result != previous);
                    previous = result;
                }
            } catch (Throwable failure) { caught = failure; }
            if (mode == 0) require(caught == null);
            else if (statics) {
                if (attempt == 1) {
                    require(caught instanceof ExceptionInInitializerError);
                    if (mode == 1) require(caught.getCause() == Trace.failure);
                    else require(caught.getCause() instanceof NullPointerException);
                } else require(caught instanceof NoClassDefFoundError);
            } else if (mode == 1) require(caught == Trace.failure);
            else require(caught instanceof NullPointerException);
            if (statics) require(Trace.observedFirst == first && Trace.observedSecond == null);
            else require(Trace.escaped != null && Trace.escaped.first == value
                         && Trace.escaped.second == (mode == 0 ? value : null));
            if (!statics || attempt == 1) expected = expected * (mode == 0 ? 10000 : 1000) + (mode == 0 ? 1234 : 125);
            require(Trace.events == expected);
        }
        System.out.println("static=" + statics + ",route=" + route + ",mode=" + mode + ",trace=" + Trace.events);
    }
}
