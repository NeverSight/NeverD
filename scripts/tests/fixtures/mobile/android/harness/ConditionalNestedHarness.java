import fixture.*;
import java.lang.invoke.*;
import java.lang.reflect.*;
public final class ConditionalNestedHarness {
    static void require(boolean ok) { if (!ok) throw new AssertionError("conditional nested allocation contract"); }
    static Outer call(int variant, int route, Input input, Object tag) throws Throwable {
        if (route == 0) return variant == 0 ? Creator.make(input, tag) : Creator.withTag(input, tag);
        if (route == 1) return variant == 0 ? ConditionalNestedCaller.make(input, tag) : ConditionalNestedCaller.withTag(input, tag);
        String name = variant == 0 ? "make" : "withTag";
        if (route == 2) {
            try { return (Outer) Creator.class.getMethod(name, Input.class, Object.class).invoke(null, input, tag); }
            catch (InvocationTargetException failure) { throw failure.getCause(); }
        }
        return (Outer) MethodHandles.publicLookup().findStatic(Creator.class, name,
            MethodType.methodType(Outer.class, Input.class, Object.class)).invokeExact(input, tag);
    }
    static IllegalStateException failure(int route, Object input) throws Throwable {
        if (route == 0) return Creator.failure(input);
        if (route == 1) return ConditionalNestedCaller.failure(input);
        if (route == 2) {
            try { return (IllegalStateException) Creator.class.getMethod("failure", Object.class).invoke(null, input); }
            catch (InvocationTargetException error) { throw error.getCause(); }
        }
        return (IllegalStateException) MethodHandles.publicLookup().findStatic(Creator.class, "failure",
            MethodType.methodType(IllegalStateException.class, Object.class)).invokeExact(input);
    }
    public static void main(String[] args) throws Throwable {
        int variant = Integer.parseInt(args[0]), route = Integer.parseInt(args[1]);
        int branch = Integer.parseInt(args[2]), mode = Integer.parseInt(args[3]);
        Input input = branch == 0 ? null : new Input();
        Trace.failure = new IllegalArgumentException("owned nested failure"); Trace.failAt = mode;
        int[] mutable = {7};
        for (int attempt = 0; attempt < 2; attempt++) {
            Trace.events = 0; Trace.inner = null; Trace.outer = null;
            Object tag = attempt == 0 ? mutable : null; mutable[0]++;
            if (input != null) input.value = attempt == 0 ? "value" : null;
            String text = "owned " + (input == null ? "missing" : input.value);
            Throwable thrown = null; Outer result = null;
            try { result = call(variant, route, input, tag); } catch (Throwable caught) { thrown = caught; }
            long expected = 0; boolean fails = false, sawInner = false, sawOuter = false;
            if (attempt == 0 || mode > 2 || mode == 0) {
                int[] sequence = attempt == 0 ? new int[]{1,2,3,4,5,6,7,8,9} : new int[]{3,4,5,6,7,8,9};
                for (int event : sequence) {
                    if (event == 5 && input == null) continue;
                    expected = expected * 10 + event;
                    if (event == 9) sawOuter = true;
                    if (event == mode) { fails = true; break; }
                    if (event == 3) sawInner = true;
                }
            } else fails = true;
            require(Trace.events == expected);
            if (fails && mode <= 2) {
                if (attempt == 0) require(thrown instanceof ExceptionInInitializerError && thrown.getCause() == Trace.failure);
                else require(thrown instanceof NoClassDefFoundError);
            } else if (fails) require(thrown == Trace.failure);
            else require(thrown == null && result != null && result == Trace.outer);
            require((Trace.inner != null) == sawInner && (Trace.outer != null) == sawOuter);
            if (sawInner) require(Trace.inner.tag == (variant == 0 ? null : tag));
            if (sawOuter) require(Trace.outer.message.equals(text) && Trace.outer.tag == tag);
            if (result != null) require(result.message.equals(text) && result.tag == tag);
            System.out.println("variant=" + variant + ",route=" + route + ",branch=" + branch + ",mode=" + mode + ",attempt=" + attempt + ",trace=" + Trace.events);
        }
        for (Object value : new Object[]{null, mutable, "text"}) {
            IllegalStateException result = failure(route, value);
            require(result.getMessage().equals("owned " + (value == null ? "missing" : value.getClass().getSimpleName())));
        }
    }
}
