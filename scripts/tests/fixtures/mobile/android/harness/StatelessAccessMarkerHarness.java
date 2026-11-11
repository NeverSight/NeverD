import fixture.*;
import java.util.*;
import java.lang.reflect.*;
public final class StatelessAccessMarkerHarness {
    static final class FutureContext extends Context { }
    static void require(boolean value) {
        if (!value) throw new AssertionError("owned closed access marker invariant");
    }
    @SuppressWarnings({"rawtypes", "unchecked"})
    static Set<Integer> poison(Set values) { return values; }
    @SuppressWarnings("unchecked")
    private static <E extends Throwable> void fail(Throwable failure) throws E { throw (E) failure; }
    private static Object invoke(java.lang.reflect.Method method, Object target, Object value) throws Throwable {
        try { return method.invoke(target, value); }
        catch (InvocationTargetException failure) { throw failure.getCause(); }
    }
    private static void checkAnonymous(Object receiver, Method apply, final int mode, final Object value) throws Throwable {
        final Object initial = new Object(), selected = mode % 5 == 1 ? null : new Object();
        final Throwable failure = mode % 5 == 2 ? new java.io.IOException() :
            mode % 5 == 3 ? new IllegalStateException() : mode % 5 == 4 ? new AssertionError() : null;
        final int[] callbacks = {0};
        Core.observed = initial; Core.result = initial; Core.calls = 0;
        Core.action = new Runnable() { public void run() {
            require(++callbacks[0] == 1 && Core.observed == value && Core.calls == 1);
            Core.result = selected;
            if (failure != null) StatelessAccessMarkerHarness.<RuntimeException>fail(failure);
        }};
        Object returned = initial; Throwable caught = null;
        try { returned = invoke(apply, receiver, value); } catch (Throwable thrown) { caught = thrown; }
        require(Core.observed == value && Core.calls == 1 && callbacks[0] == 1 && Core.result == selected);
        require(caught == failure && returned == (failure == null ? selected : initial));
    }
    public static void main(String[] args) throws Throwable {
        final int route = Integer.parseInt(args[0]), mode = Integer.parseInt(args[1]);
        final boolean rebuilt = Boolean.parseBoolean(args[3]);
        Class<?> markerType = Class.forName(args[2]);
        Class<?> handlerType = Class.forName("fixture.Core$Handler");
        require(handlerType.isInterface() && Modifier.isPrivate(handlerType.getModifiers()) && Modifier.isStatic(handlerType.getModifiers()));
        require(Arrays.equals(markerType.getInterfaces(), new Class<?>[]{handlerType}));
        Constructor<Core> access = Core.class.getDeclaredConstructor(Set.class, handlerType, Context.class, Probe.class, markerType);
        require(access.isSynthetic() != rebuilt && !Modifier.isPublic(access.getModifiers()) && !Modifier.isPrivate(access.getModifiers()));
        require(markerType.getEnclosingClass() == Core.class && markerType.getEnclosingMethod() == null && markerType.getDeclaredFields().length == 0);
        require(markerType.isAnonymousClass() != rebuilt && markerType.isMemberClass() == rebuilt);
        require(markerType.getDeclaredConstructors().length == 1);
        Constructor<?> markerConstructor = markerType.getDeclaredConstructor();
        require(!Modifier.isPublic(markerConstructor.getModifiers()) && !Modifier.isPrivate(markerConstructor.getModifiers()) && !markerConstructor.isSynthetic());
        markerConstructor.setAccessible(true);
        Object unused = markerConstructor.newInstance();
        Object original = Core.implementation;
        require(original != unused && original.getClass() == markerType);
        Method apply = markerType.getDeclaredMethod("apply", Object.class); apply.setAccessible(true);
        Field handlerField = Core.class.getDeclaredField("handler"); handlerField.setAccessible(true);
        require(Modifier.isPrivate(handlerField.getModifiers()) && Modifier.isFinal(handlerField.getModifiers()));
        access.setAccessible(true);
        java.lang.invoke.MethodHandle constructor = java.lang.invoke.MethodHandles.lookup().unreflectConstructor(access);
        java.lang.invoke.MethodHandle builder = java.lang.invoke.MethodHandles.lookup().findStatic(Core.Builder.class, "create",
            java.lang.invoke.MethodType.methodType(Core.class, Set.class, Context.class, Probe.class));
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
            final Object handler = mode == 10 ? null : original;
            Core.implementation = handler;
            checkAnonymous(unused, apply, mode, values);
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
                if (route == 0) result = Core.Builder.create(values, context, probe);
                else if (route == 1) {
                    try { result = access.newInstance(values, handler, context, probe, null); }
                    catch (InvocationTargetException caught) { throw caught.getCause(); }
                } else if (route == 2) result = (Core) builder.invokeExact((Set) values, context, probe);
                else result = (Core) constructor.invokeWithArguments(values, handler, context, probe, unused);
            } catch (Throwable caught) { failure = caught; }
            boolean callbackFailed = mode == 3 || mode == 4 || mode == 5 || mode == 14;
            boolean targetFailed = mode == 6 || mode == 7;
            boolean success = !callbackFailed && !targetFailed;
            int stage = callbackFailed ? 1 : targetFailed ? 2 : 3;
            Core partial = Core.latest;
            require(partial != null && partial.values == values && partial.context == context && partial.probe == probe);
            require(handlerField.get(partial) == handler && Core.calls == 1);
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
            checkAnonymous(original, apply, mode, token);
            System.out.println("route=" + route + ",mode=" + mode + ",attempt=" + attempt + ",stage=" + stage);
        }
    }
}
