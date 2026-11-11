import fixture.*;
import java.util.*;
import java.lang.reflect.*;
public final class ClosedAccessMarkerHarness {
    static final class FutureContext extends Context { }
    static void require(boolean value) {
        if (!value) throw new AssertionError("owned closed access marker invariant");
    }
    @SuppressWarnings({"rawtypes", "unchecked"})
    static Set<Integer> poison(Set values) { return values; }
    public static void main(String[] args) throws Throwable {
        final int route = Integer.parseInt(args[0]), mode = Integer.parseInt(args[1]);
        final boolean rebuilt = Boolean.parseBoolean(args[3]);
        Class<?> markerType = Class.forName(args[2]);
        Constructor<Core> access = Core.class.getDeclaredConstructor(Set.class, Context.class, Probe.class, markerType);
        require(access.isSynthetic() != rebuilt && !Modifier.isPublic(access.getModifiers()) && !Modifier.isPrivate(access.getModifiers()));
        require(markerType.getEnclosingClass() == Core.class && markerType.getEnclosingMethod() == null && markerType.getDeclaredFields().length == 0);
        require(markerType.isAnonymousClass() != rebuilt && markerType.isMemberClass() == rebuilt);
        require(markerType.getDeclaredConstructors().length == (rebuilt ? 1 : 0));
        if (rebuilt) require(Modifier.isPrivate(markerType.getDeclaredConstructors()[0].getModifiers()));
        access.setAccessible(true);
        for (int attempt = 0; attempt < 2; ++attempt) {
            final LinkedHashSet<Object> raw = new LinkedHashSet<Object>();
            if (mode != 11) raw.add(mode == 12 ? "poison" : Integer.valueOf(42));
            final Set<Integer> values = mode == 1 ? null : poison(raw);
            final Context context = mode == 2 ? null : mode == 13 ? new FutureContext() : new Context();
            final Object token = new Object(), changedToken = new Object();
            if (context != null) context.token = token;
            final Object marker = mode == 10 ? null : new Object();
            final RuntimeException callbackFailure = new RuntimeException(), targetFailure = new RuntimeException();
            final Error callbackError = new AssertionError("callback sentinel");
            final int[] calls = {0};
            if (context != null) context.failure = mode == 6 || mode == 8 || mode == 14 ? targetFailure : null;
            Core.latest = null;
            final Probe probe = mode == 3 ? null : new Probe() {
                public Object observe(Core self, Object actualValues, Context actualContext) {
                    require(++calls[0] == 1 && self == Core.latest && actualValues == values && actualContext == context);
                    require(self.values == values && self.context == context && self.probe == this && self.stage == 1 && self.marker == null);
                    if (mode == 4 || mode == 14) throw callbackFailure;
                    if (mode == 5) throw callbackError;
                    if (mode == 7) context.failure = targetFailure;
                    if (mode == 8) context.failure = null;
                    if (mode == 9) raw.add(Integer.valueOf(99));
                    if (mode == 15) context.token = changedToken;
                    return marker;
                }
            };
            Core result = null; Throwable failure = null;
            try {
                if (route % 2 == 0) result = Core.Builder.create(values, context, probe);
                else {
                    try { result = access.newInstance(values, context, probe, null); }
                    catch (InvocationTargetException caught) { throw caught.getCause(); }
                }
            } catch (Throwable caught) { failure = caught; }
            boolean callbackFailed = mode == 3 || mode == 4 || mode == 5 || mode == 14;
            boolean targetFailed = mode == 6 || mode == 7;
            boolean success = !callbackFailed && !targetFailed;
            int stage = callbackFailed ? 1 : targetFailed ? 2 : 3;
            Core partial = Core.latest;
            require(partial != null && partial.values == values && partial.context == context && partial.probe == probe);
            require(partial.stage == stage && partial.marker == (callbackFailed ? null : marker));
            require(calls[0] == (mode == 3 ? 0 : 1) && result == (success ? partial : null));
            if (mode == 3) require(failure instanceof NullPointerException);
            else if (mode == 4 || mode == 14) require(failure == callbackFailure);
            else if (mode == 5) require(failure == callbackError);
            else if (targetFailed) require(failure == targetFailure);
            else require(failure == null);
            if (success && values != null && !values.isEmpty()) {
                if (mode == 12) {
                    boolean bad = false;
                    try { Integer ignored = result.values.iterator().next(); }
                    catch (ClassCastException caught) { bad = true; }
                    require(bad && partial.stage == 3);
                } else require(result.values.iterator().next().intValue() == 42);
            }
            if (context != null) require(context.token == (mode == 15 ? changedToken : token));
            if (mode == 9) require(raw.size() == 2);
            System.out.println("route=" + route + ",mode=" + mode + ",attempt=" + attempt + ",stage=" + stage);
        }
    }
}
