import fixture.*;
import java.lang.invoke.*;
import java.lang.reflect.*;
@SuppressWarnings({"rawtypes","unchecked"})
public final class ComparableSelectorHarness {
    static Object suppliedKey, suppliedInput;
    static Box suppliedBox, lateBox;
    static final class Key implements Comparable<Key> {
        final int value;
        Key(int value) { this.value = value; }
        public int compareTo(Key other) {
            require(other == this);
            Trace.comparisons++; Trace.hit(8); return value;
        }
    }
    static final class Select implements Callback {
        public Object apply(Object value) {
            require(value == suppliedInput);
            Trace.callbacks++; Trace.hit(7); return suppliedKey;
        }
    }
    static void require(boolean ok) { if (!ok) throw new AssertionError("owned comparable selector carrier"); }
    static Box call(int route,Box seed,Callback selector) throws Throwable {
        if (route == 0) return Client.call(seed,selector);
        if (route == 1) return ComparableSelectorCaller.call(seed,selector);
        if (route == 2) try {
            return (Box)Client.class.getMethod("call",Box.class,Callback.class).invoke(null,seed,selector);
        } catch (InvocationTargetException failure) { throw failure.getCause(); }
        return (Box)MethodHandles.publicLookup().findStatic(Client.class,"call",
            MethodType.methodType(Box.class,Box.class,Callback.class)).invokeExact(seed,selector);
    }
    public static void main(String[] args) throws Throwable {
        int route = Integer.parseInt(args[0]), mode = Integer.parseInt(args[1]);
        Method method = Provider.class.getDeclaredMethod("choose",Box.class,Callback.class);
        TypeVariable<Method>[] vars = method.getTypeParameters();
        require(vars.length == 2 && vars[0].getName().equals("T") && vars[1].getName().equals("K") && vars[0].getBounds()[0] == Object.class);
        ParameterizedType bound = (ParameterizedType)vars[1].getBounds()[0];
        require(bound.getRawType() == Comparable.class && ((WildcardType)bound.getActualTypeArguments()[0]).getLowerBounds()[0].equals(vars[1]));
        ParameterizedType input = (ParameterizedType)method.getGenericParameterTypes()[0];
        ParameterizedType selector = (ParameterizedType)method.getGenericParameterTypes()[1];
        ParameterizedType output = (ParameterizedType)method.getGenericReturnType();
        require(input.getRawType() == Box.class && output.getRawType() == Box.class && input.getActualTypeArguments()[0].equals(vars[0]) && output.getActualTypeArguments()[0].equals(vars[0]));
        require(selector.getRawType() == Callback.class && ((WildcardType)selector.getActualTypeArguments()[0]).getLowerBounds()[0].equals(vars[0]) &&
            ((WildcardType)selector.getActualTypeArguments()[1]).getUpperBounds()[0].equals(vars[1]));
        boolean initialized = false, failed = false;
        int[] failureEvents = {0,0,0,0,0,0,0,0,0,1,2,7,3,8,4,5,6};
        for (int attempt = 0; attempt < 2; ++attempt) {
            suppliedInput = mode == 6 ? null : new Object();
            Box seed = mode == 1 ? null : new Box(suppliedInput);
            Callback callback = mode == 2 ? null : new Select();
            suppliedKey = mode == 3 ? null : mode == 4 ? new Object() : mode == 5 ? "owned key" : mode == 7 ? Integer.valueOf(131 + attempt) : new Key(-17 - attempt);
            suppliedBox = new Box(new Object()); lateBox = mode == 8 ? null : new Box(new Object());
            Object lateInput = new Object(), sentinel = new Object();
            Trace.events = 0; Trace.callbacks = Trace.comparisons = 0; Trace.comparison = 931;
            Trace.failAt = failureEvents[mode]; Trace.failure = new IllegalStateException("owned comparable selector failure");
            Trace.seenSeed = Trace.seenInput = Trace.seenKey = sentinel; Trace.result = suppliedBox;
            Trace.observer = event -> {
                if (event == 2 && seed != null) seed.value = lateInput;
                if (event == 4) Trace.result = lateBox;
                if (event == 5) Trace.result = suppliedBox;
            };
            long expected = 0; Throwable expectedFailure = null;
            Class<?> expectedType = null;
            boolean seeded = false, captured = false, selected = false, compared = false;
            int callbacks = 0, comparisons = 0;
            if (failed) expectedType = NoClassDefFoundError.class;
            else if (!initialized) {
                expected = 6; initialized = true;
                if (Trace.failAt == 6) { failed = true; expectedType = ExceptionInInitializerError.class; }
            }
            if (expectedType == null) {
                for (int event : new int[]{1,2,7,3,8,4,5}) {
                    if (event == 2 && seed == null || event == 7 && callback == null || event == 8 && suppliedKey == null) {
                        expectedType = NullPointerException.class; break;
                    }
                    if (event == 3 && mode == 4) { expectedType = ClassCastException.class; break; }
                    if (event == 8 && !(suppliedKey instanceof Key)) continue;
                    expected = expected * 10 + event;
                    if (event == 1) seeded = true;
                    if (event == 2) captured = true;
                    if (event == 7) callbacks++;
                    if (event == 3) selected = true;
                    if (event == 8) comparisons++;
                    if (event == 4) compared = true;
                    if (event == Trace.failAt) { expectedFailure = Trace.failure; break; }
                }
            }
            Box result = null; Throwable caught = null;
            try { result = call(route,seed,callback); } catch (Throwable failure) { caught = failure; }
            require(Trace.events == expected && Trace.callbacks == callbacks && Trace.comparisons == comparisons);
            require(Trace.seenSeed == (seeded ? seed : sentinel) && Trace.seenInput == (captured ? suppliedInput : sentinel) && Trace.seenKey == (selected ? suppliedKey : sentinel));
            require(Trace.comparison == (compared ? suppliedKey instanceof Key ? ((Key)suppliedKey).value : 0 : 931));
            if (expectedType != null) {
                require(caught != null && caught.getClass() == expectedType);
                if (expectedType == ExceptionInInitializerError.class) require(caught.getCause() == Trace.failure);
            } else if (expectedFailure != null) require(caught == expectedFailure);
            else require(caught == null && result == lateBox);
            if (captured) require(seed.value == lateInput);
            System.out.println("route=" + route + ",mode=" + mode + ",attempt=" + attempt + ",trace=" + Trace.events);
        }
    }
}
