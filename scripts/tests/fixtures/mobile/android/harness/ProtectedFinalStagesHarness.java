import fixture.*;
import java.lang.invoke.MethodHandles;
import java.lang.invoke.MethodType;
import java.lang.reflect.InvocationTargetException;

public final class ProtectedFinalStagesHarness {
    static void require(boolean value) {
        if (!value) throw new AssertionError("protected final initialization stages");
    }
    static Core create(int route, Provider provider, int selector) throws Throwable {
        if (route == 0) return new Core(provider, selector);
        if (route == 1) return ProtectedFinalStagesCaller.create(provider, selector);
        if (route == 2) {
            try { return Core.class.getConstructor(Provider.class, int.class).newInstance(provider, selector); }
            catch (InvocationTargetException failure) { throw failure.getCause(); }
        }
        return (Core) MethodHandles.publicLookup().findConstructor(Core.class,
                MethodType.methodType(void.class, Provider.class, int.class)).invokeExact(provider, selector);
    }
    public static void main(String[] args) throws Throwable {
        int selector = Integer.parseInt(args[0]), route = Integer.parseInt(args[1]), mode = Integer.parseInt(args[2]);
        Provider provider = new Provider();
        Trace.failure = new IllegalArgumentException("owned stage failure");
        Trace.stepFailure = mode == 6 ? new IllegalArgumentException("owned earlier step failure") : null;
        Trace.failAt = mode;
        for (int attempt = 0; attempt < 2; attempt++) {
            Trace.seed = attempt == 0 ? 7 : 200;
            Trace.events = 0; Trace.caught = null; Trace.published = null;
            long expected = attempt == 0 ? 9 : 0;
            int seed = attempt == 0 ? 100 : 200;
            boolean completed = false, published = false;
            if (mode != 9) {
                expected = expected * 11 + 1;
                if (mode != 1) {
                    int mark = selector + 2;
                    expected = expected * 11 + mark;
                    if (mode != mark) {
                        expected = expected * 11 + 5;
                        if (mode == 5 || mode == 6) expected = expected * 11 + 6;
                        expected = expected * 11 + 7;
                        if (mode != 6 && mode != 7) {
                            expected = expected * 11 + 8;
                            if (mode != 8) { expected = expected * 11 + 10; published = true; completed = mode != 10; }
                        }
                    }
                }
            }
            Core actual = null; Throwable thrown = null;
            try { actual = create(route, provider, selector); } catch (Throwable failure) { thrown = failure; }
            require(Trace.events == expected);
            if (completed) require(thrown == null && actual == Trace.published);
            else if (mode == 9) {
                if (attempt == 0) require(thrown instanceof ExceptionInInitializerError && thrown.getCause() == Trace.failure);
                else require(thrown instanceof NoClassDefFoundError);
            } else require(thrown == Trace.failure);
            require(published ? Trace.published != null : Trace.published == null);
            if (published) require(Trace.published.first == seed + 1 && Trace.published.choice == seed + selector + 2 && Trace.published.last == seed + 8);
            Throwable caught = mode == 5 ? Trace.failure : mode == 6 ? Trace.stepFailure : null;
            require(Trace.caught == caught);
            System.out.println("selector=" + selector + ",route=" + route + ",mode=" + mode
                    + ",attempt=" + attempt + ",trace=" + Trace.events);
        }
    }
}
