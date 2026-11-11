import fixture.*;
import java.lang.invoke.MethodHandles;
import java.lang.invoke.MethodType;
import java.lang.reflect.InvocationTargetException;
public final class CheckedStarHarness {
    private static void require(boolean value) {
        if (!value) throw new AssertionError("owned checked star constructor contract");
    }
    private static Child create(int route, Object key, Object first, Object second) throws Throwable {
        if (route == 0) return new Child(key, first, second);
        if (route == 1) return CheckedStarCaller.create(key, first, second);
        if (route == 2) {
            try {
                return Child.class.getConstructor(Object.class, Object.class, Object.class)
                    .newInstance(key, first, second);
            } catch (InvocationTargetException error) { throw error.getCause(); }
        }
        return (Child) MethodHandles.lookup().findConstructor(Child.class,
            MethodType.methodType(void.class, Object.class, Object.class, Object.class))
            .invokeWithArguments(key, first, second);
    }
    public static void main(String[] args) throws Throwable {
        int route = Integer.parseInt(args[0]), variant = Integer.parseInt(args[1]);
        int fail = Integer.parseInt(args[2]), kind = Integer.parseInt(args[3]);
        Object key = variant == 1 ? null : variant == 2 ? new Object()
            : variant == 3 ? Integer.valueOf(37) : new KeyValue();
        Object first = variant == 4 ? null : variant == 7 ? key : new Object();
        Object second = variant == 5 ? null : variant == 6 ? first : new Object();
        Trace.failure = kind == 0 ? new IllegalArgumentException("owned checked star failure")
            : new LinkageError("owned checked star failure");
        Trace.failAt = fail;
        boolean invalid = variant == 2 || variant == 3;
        for (int attempt = 0; attempt < 2; attempt++) {
            Trace.events = Trace.mask = 0;
            Trace.seenThis = null;
            Trace.seenKey = Trace.seenLeft = Trace.seenRight = Trace.seenFinal = null;
            Trace.wrong = false;
            Child result = null;
            Throwable caught = null;
            try { result = create(route, key, first, second); }
            catch (Throwable error) { caught = error; }
            boolean initFailure = fail == 1 || fail == 2;
            int events = attempt == 0 ? fail == 1 ? 1 : 12 : 0;
            int mask = 0;
            if (!initFailure && !invalid) {
                events = events * 10 + 3;
                mask = 1;
                if (fail != 3) {
                    events = events * 10 + 4;
                    mask = 3;
                    if (fail != 4) { events = events * 10 + 5; mask = 7; }
                }
            }
            require(!Trace.wrong && Trace.events == events && Trace.mask == mask);
            if (initFailure) {
                if (attempt == 1) require(caught instanceof NoClassDefFoundError);
                else if (kind == 0) require(caught instanceof ExceptionInInitializerError
                    && ((ExceptionInInitializerError) caught).getException() == Trace.failure);
                else require(caught == Trace.failure);
            } else if (invalid) require(caught instanceof ClassCastException);
            else require(fail == 0 ? caught == null && result == Trace.seenThis : caught == Trace.failure);
            if (mask != 0) {
                require(Trace.seenThis != null && Trace.seenKey == key
                    && Trace.seenLeft == first && Trace.seenRight == second
                    && Trace.seenThis.left() == first && Trace.seenThis.right() == second);
                require(Trace.seenThis.key == (mask == 1 ? null : key));
                require(Trace.seenFinal == (mask == 7 ? key : null));
            } else require(Trace.seenThis == null && Trace.seenKey == null
                && Trace.seenLeft == null && Trace.seenRight == null && Trace.seenFinal == null);
            System.out.println("route=" + route + ",variant=" + variant + ",fail=" + fail
                + ",kind=" + kind + ",attempt=" + attempt + ",events=" + Trace.events + ",mask=" + Trace.mask);
        }
    }
}
