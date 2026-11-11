import fixture.*;
import java.lang.reflect.*;

public final class PredicateReferenceBridgeHarness {
    private static int identities, values, failures;
    private static void identity(Object actual, Object expected) {
        if (actual != expected) throw new AssertionError("identity changed");
        identities++;
    }
    private static void value(int actual, int expected) {
        if (actual != expected) throw new AssertionError("value changed");
        values++;
    }
    private static void result(boolean actual, boolean expected) {
        value(actual ? 1 : 0, expected ? 1 : 0);
    }
    public static void main(String[] args) throws Exception {
        Counter<String> counter = new Counter<>();
        CountView<String> view = counter;
        Element<String> element = new Element<>();
        counter.expected = element;
        result(counter.contains(element), true);
        identity(counter.last, element); value(counter.calls, 1); value(Probe.calls, 0);
        result(counter.contains((Element<String>) null), false);
        identity(counter.last, null); value(counter.calls, 2); value(Probe.calls, 0);
        Object wrong = new Object();
        int count = 0;
        for (Object input : new Object[] {wrong, "owned incompatible value", null}) {
            result(view.contains(input), false);
            identity(Probe.last, input); value(counter.calls, 2); value(Probe.calls, ++count);
        }
        result(view.contains(element), true);
        identity(counter.last, element); identity(Probe.last, element);
        value(Probe.calls, 4); value(counter.calls, 3);
        Probe.forceFalse = true;
        result(view.contains(element), false);
        value(Probe.calls, 5); value(counter.calls, 3); identity(Probe.last, element);
        Probe.forceFalse = false;
        RuntimeException probeFailure = new IllegalArgumentException("owned probe failure");
        RuntimeException targetFailure = new IllegalStateException("owned target failure");
        Probe.failure = probeFailure; counter.failure = targetFailure;
        try { view.contains(element); throw new AssertionError("probe failure missing"); }
        catch (IllegalArgumentException e) { identity(e, probeFailure); failures++; }
        value(Probe.calls, 6); value(counter.calls, 3); identity(Probe.last, element);
        Probe.failure = null;
        result(view.contains(wrong), false);
        value(Probe.calls, 7); value(counter.calls, 3); identity(Probe.last, wrong);
        try { view.contains(element); throw new AssertionError("target failure missing"); }
        catch (IllegalStateException e) { identity(e, targetFailure); failures++; }
        value(Probe.calls, 8); value(counter.calls, 4); identity(counter.last, element);
        Probe.forceTrue = true;
        try { view.contains(wrong); throw new AssertionError("checked cast failure missing"); }
        catch (ClassCastException e) { failures++; }
        value(Probe.calls, 9); value(counter.calls, 4); identity(Probe.last, wrong);
        counter.failure = null;
        result(view.contains(null), false);
        value(Probe.calls, 10); value(counter.calls, 5);
        identity(counter.last, null); identity(Probe.last, null);
        Counter<String> absent = null;
        try { absent.contains((Object) element); throw new AssertionError("receiver failure missing"); }
        catch (NullPointerException e) { failures++; }
        value(Probe.calls, 10); value(counter.calls, 5);
        try { absent.contains(element); throw new AssertionError("typed receiver failure missing"); }
        catch (NullPointerException e) { failures++; }
        value(Probe.calls, 10); value(counter.calls, 5);
        Method bridge = Counter.class.getMethod("contains", Object.class);
        Method target = Counter.class.getMethod("contains", Element.class);
        boolean recovered = args[0].equals("recovered");
        if (bridge.isBridge() == recovered || bridge.isSynthetic()
                || !Modifier.isFinal(bridge.getModifiers()))
            throw new AssertionError("bridge flag receipt changed");
        ParameterizedType parameter = (ParameterizedType) target.getGenericParameterTypes()[0];
        if (!parameter.getActualTypeArguments()[0].equals(Counter.class.getTypeParameters()[0]))
            throw new AssertionError("typed generic scope changed");
        System.out.println("values=" + values + ",identities=" + identities + ",failures=" + failures);
        System.out.println("predicate-guard=matched,call-effects-and-failures=retained,flags=reported");
    }
}
