import fixture.*;
import java.lang.reflect.*;
public final class PrivateContextualCarrierHarness {
    static int mode;
    static Carrier output;
    static RuntimeException failure;
    static void check(boolean v) { if (!v) throw new AssertionError("owned private contextual carrier"); }
    static final class FutureCallback implements Callback {
        int calls, selected; Context context; Carrier seed;
        public Carrier create(Context context, int index, Carrier seed) {
            calls++; this.context = context; selected = index; this.seed = seed;
            if (mode == 4) throw failure;
            if (mode == 12) seed.value = new String("mutated");
            return output;
        }
    }
    public static void main(String[] args) throws Exception {
        int route = Integer.parseInt(args[0]); mode = Integer.parseInt(args[1]);
        Method method = Core.class.getDeclaredMethod("produce", int.class, Context.class, Callback.class, Carrier.class);
        method.setAccessible(true); TypeVariable<?> t = method.getTypeParameters()[0];
        check(Modifier.isPrivate(method.getModifiers()) && t.getBounds()[0] == Object.class
            && ((ParameterizedType) method.getGenericReturnType()).getActualTypeArguments()[0].equals(t)
            && ((ParameterizedType) method.getGenericParameterTypes()[2]).getActualTypeArguments()[0].equals(t)
            && ((ParameterizedType) method.getGenericParameterTypes()[3]).getActualTypeArguments()[0].equals(t));
        for (int attempt = 0; attempt < 2; attempt++) {
            Core.events = 0; failure = new RuntimeException("owned callback");
            RuntimeException contextFailure = new RuntimeException("owned context"), providerFailure = new RuntimeException("owned provider");
            Core owner = new Core(); Context context = new Context(); FutureCallback callback = new FutureCallback();
            Core supplied = mode == 1 ? null : owner; Context suppliedContext = mode == 2 ? null : context;
            Callback suppliedCallback = mode == 7 ? null : callback;
            Object marker = mode == 15 ? null : new Object(); context.marker = marker;
            context.failure = mode == 3 ? contextFailure : null; owner.failure = mode == 5 ? providerFailure : null;
            int index = mode == 10 ? Integer.MIN_VALUE + attempt : mode == 11 ? Integer.MAX_VALUE - attempt : 17 + attempt;
            Object payload = mode == 8 ? new Object() : mode == 13 ? null : new String("payload" + attempt);
            Carrier seed = mode == 9 ? null : new Carrier(new String("seed" + attempt));
            Carrier cached = new Carrier(payload), sentinel = new Carrier(new Object());
            output = mode == 6 ? null : mode == 14 ? seed : cached; owner.savedResult = sentinel; owner.selected = 99;
            Object result = null; Throwable caught = null;
            try {
                if (route == 0) result = Core.call(supplied, index, suppliedContext, suppliedCallback, seed);
                else if (route == 1) Core.discard(supplied, index, suppliedContext, suppliedCallback, seed);
                else if (route == 2) result = Core.typed(supplied, index, suppliedContext, suppliedCallback, seed);
                else if (supplied == null) throw new NullPointerException();
                else result = method.invoke(supplied, index, suppliedContext, suppliedCallback, seed);
            } catch (Throwable error) { caught = error instanceof InvocationTargetException ? error.getCause() : error; }
            boolean success = mode != 1 && mode != 2 && mode != 3 && mode != 4 && mode != 5 && mode != 7;
            if (success) check(caught == null);
            else if (mode == 1 || mode == 2 || mode == 7) check(caught instanceof NullPointerException);
            else check(caught == (mode == 3 ? contextFailure : mode == 4 ? failure : providerFailure));
            int events = mode == 1 ? route == 3 ? 0 : 3 : route == 3 ? success ? 7 : 1 : success ? 414 : 16;
            int touched = mode == 1 || mode == 2 ? 0 : 1;
            boolean called = mode != 1 && mode != 2 && mode != 3 && mode != 7;
            check(Core.events == events && context.calls == touched && callback.calls == (called ? 1 : 0));
            check(owner.savedContext == (mode == 1 ? null : suppliedContext)
                && owner.savedCallback == (mode == 1 ? null : suppliedCallback)
                && owner.savedSeed == (mode == 1 ? null : seed) && owner.selected == (mode == 1 ? 99 : index));
            check(owner.marker == (mode == 1 || mode == 2 || mode == 3 ? null : marker));
            check(callback.context == (called ? suppliedContext : null) && callback.seed == (called ? seed : null)
                && callback.selected == (called ? index : 0));
            check(owner.savedResult == (success ? output : sentinel) && result == (success && route != 1 ? output : null));
            if (mode == 12) check(seed.value instanceof String && seed.value.equals("mutated"));
            if (success && output != null) {
                Carrier<String> later = owner.savedResult; String value = null; Throwable late = null;
                Object actual = output.value;
                try { value = later.get(); } catch (Throwable error) { late = error; }
                check(mode == 8 ? late instanceof ClassCastException : late == null && value == actual);
                output.value = new String("future"); String future = later.get();
                check(future == output.value && Core.events == (events * 5 + 4) * 5 + 4);
            }
            System.out.println("route=" + route + ",mode=" + mode + ",attempt=" + attempt + ",events=" + events);
        }
    }
}
