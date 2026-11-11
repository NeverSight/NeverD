import fixture.*;
import java.lang.invoke.*;
import java.lang.reflect.*;
import kotlin.jvm.internal.DefaultConstructorMarker;
public class ScopedMaskConstructorsHarness {
    static void require(boolean value) { if (!value) throw new AssertionError("scoped mask constructor contract"); }
    static Core<?> make(int route, int count, int capacity, Sink<?> callback, int mask) throws Throwable {
        if (route == 1) return ScopedMaskConstructorsCaller.make(count, capacity, callback, mask);
        if (route == 0 || route == 2) {
            try { return (Core<?>) Core.class.getDeclaredConstructor(int.class, int.class, Sink.class, int.class,
                    DefaultConstructorMarker.class).newInstance(count, capacity, callback, mask, null); }
            catch (InvocationTargetException failure) { throw failure.getCause(); }
        }
        return (Core<?>) MethodHandles.publicLookup().findConstructor(Core.class,
                MethodType.methodType(void.class, int.class, int.class, Sink.class, int.class, DefaultConstructorMarker.class))
                .invoke(11, 19, callback, mask, (DefaultConstructorMarker)null);
    }
    public static void main(String[] args) throws Throwable {
        int route = Integer.parseInt(args[0]), mode = Integer.parseInt(args[1]);
        boolean synthetic = Boolean.parseBoolean(args[2]);
        Constructor<?> ctor = Core.class.getDeclaredConstructor(int.class, int.class, Sink.class, int.class, DefaultConstructorMarker.class);
        require(ctor.isSynthetic() == synthetic && ctor.getGenericParameterTypes()[2] == Sink.class);
        Sink<Object> incoming = new Consumer();
        Object token = new Object();
        int mask = mode % 8;
        Trace.fail = mode >= 8 && mode < 16 ? 2 : mode >= 16 && mode < 24 ? 3 : mode >= 24 && mode < 32 ? 4 : 0;
        Trace.failure = new IllegalStateException("owned scoped mask failure");
        boolean defaultCallback = (mask & 4) != 0;
        for (int attempt = 0; attempt < 2; attempt++) {
            Trace.events = 0; Trace.partial = null; Trace.seen = null;
            Core<?> result = null; Throwable failure = null;
            try { result = make(route, 11, 19, incoming, mask); }
            catch (Throwable error) { failure = error; }
            boolean initFailure = defaultCallback && (Trace.fail == 2 || Trace.fail == 3);
            long events = 0;
            if (defaultCallback && attempt == 0) events = Trace.fail == 2 ? 2 : 23;
            if (!initFailure) events = events * 10 + 4;
            require(Trace.events == events && (Trace.partial != null) == !initFailure);
            if (!initFailure) {
                Core<?> partial = (Core<?>)Trace.partial;
                require(partial.count == 11 && partial.capacity == ((mask & 2) != 0 ? 7 : 19));
                require(defaultCallback ? partial.callback == Defaults.CB : partial.callback == incoming);
            }
            if (initFailure) {
                if (attempt == 0) require(failure instanceof ExceptionInInitializerError && failure.getCause() == Trace.failure);
                else require(failure instanceof NoClassDefFoundError);
            } else if (Trace.fail == 4) require(failure == Trace.failure);
            else {
                require(failure == null && result == Trace.partial);
                ((Sink<Object>)result.callback).accept(token);
                events = events * 10 + 5;
                require(Trace.seen == token && Trace.events == events);
            }
            System.out.println("route=" + route + ",mode=" + mode + ",attempt=" + attempt + ",trace=" + Trace.events);
        }
    }
}
