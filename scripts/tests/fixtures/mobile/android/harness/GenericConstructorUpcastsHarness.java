package fixture;
import java.lang.invoke.MethodHandles;
import java.lang.invoke.MethodType;
import java.lang.reflect.InvocationTargetException;
public class GenericConstructorUpcastsHarness {
    static void require(boolean value) {
        if (!value) throw new AssertionError("owned generic constructor upcast contract");
    }
    static Core<?,?> make(int route, Derived<Object,Object> input) throws Throwable {
        if (route == 0) return new Core<>(input);
        if (route == 1) return GenericConstructorUpcastsCaller.make(input);
        if (route == 2) {
            try { return Core.class.getDeclaredConstructor(Derived.class).newInstance(input); }
            catch (InvocationTargetException failure) { throw failure.getCause(); }
        }
        return (Core<?,?>) MethodHandles.lookup().findConstructor(Core.class,
                MethodType.methodType(void.class, Derived.class)).invokeExact((Derived)input);
    }
    public static void main(String[] args) throws Throwable {
        int route = Integer.parseInt(args[0]), mode = Integer.parseInt(args[1]);
        Object key = mode == 9 ? null : new Object(), value = mode == 10 ? null : mode == 8 ? key : new Object();
        Derived<Object,Object> input = mode == 6 ? null : new Derived<>(key, value);
        Trace.fail = mode >= 1 && mode <= 5 ? mode : mode == 7 ? 9 : 0;
        Trace.failure = new IllegalStateException("owned generic constructor failure");
        for (int attempt = 0; attempt < 2; attempt++) {
            Trace.events = 0; Trace.partial = null;
            Core<?,?> result = null; Throwable failure = null;
            try { result = make(route, input); } catch (Throwable error) { failure = error; }
            boolean initFailed = mode == 1 || mode == 2;
            long events = attempt == 0 ? mode == 1 ? 1 : mode == 2 ? 12 : 12 : 0;
            if (!initFailed) events = events * 10 + 3;
            boolean parent = !initFailed && mode != 3 && mode != 6;
            if (parent) events = events * 10 + 4;
            boolean captured = parent && mode != 4;
            if (captured) events = events * 10 + 5;
            require(Trace.events == events && (Trace.partial != null) == parent);
            if (parent) {
                Parent<?,?> partial = (Parent<?,?>)Trace.partial;
                require(partial.selected == (captured ? input : null));
                require(((Core<?,?>)partial).original == (captured ? input : null));
            }
            if (initFailed) {
                if (attempt == 0) require(failure instanceof ExceptionInInitializerError && failure.getCause() == Trace.failure);
                else require(failure instanceof NoClassDefFoundError);
            } else if ((mode >= 3 && mode <= 6)) require(failure == Trace.failure);
            else require(failure == null && result == Trace.partial && result.original == input
                    && result.selected == input && input.key == key && input.value == value);
            System.out.println("route=" + route + ",mode=" + mode + ",attempt=" + attempt + ",trace=" + Trace.events);
        }
    }
}
