import fixture.*;
import java.lang.invoke.*;
import java.lang.reflect.*;
public final class GenericDefaultFoldHarness {
    static void check(boolean condition) {
        if (!condition) throw new AssertionError("owned generic default fold");
    }
    static final class Plain implements Api {}
    static final class Override implements Api {
        @SuppressWarnings("unchecked")
        public <R> R fold(R seed, Bi<? super R,? super Api,? extends R> operation) {
            Trace.events = 9;
            return (R)Trace.produced;
        }
    }
    static final class Callback implements Bi<Object,Api,Object> {
        final Object expectedSeed, result, late;
        final Api expectedSelf;
        int calls;
        Callback(Object seed, Api self, Object result, Object late) {
            expectedSeed = seed; expectedSelf = self; this.result = result; this.late = late;
        }
        public Object invoke(Object seed, Api self) {
            check(seed == expectedSeed && self == expectedSelf);
            calls++; Trace.step(2);
            Trace.produced = late;
            return result;
        }
    }
    static Object call(int route, Api receiver, Object seed, Bi<Object,Api,Object> operation) throws Throwable {
        if (route == 0) return receiver.fold(seed, operation);
        if (route == 1) return GenericDefaultFoldCaller.call(receiver, seed, operation);
        if (route == 2) {
            try { return Api.class.getDeclaredMethod("access$fold$jd", Api.class, Object.class, Bi.class).invoke(null, receiver, seed, operation); }
            catch (InvocationTargetException failure) { throw failure.getCause(); }
        }
        MethodHandle handle = MethodHandles.publicLookup().findStatic(Api.class, "access$fold$jd", MethodType.methodType(Object.class, Api.class, Object.class, Bi.class));
        return (Object)handle.invokeExact(receiver, seed, (Bi)operation);
    }
    static void metadata() throws Exception {
        Method method = Api.class.getMethod("fold", Object.class, Bi.class);
        check(method.isDefault() && method.getTypeParameters().length == 1);
        TypeVariable<?> formal = method.getTypeParameters()[0];
        check(formal.getGenericDeclaration().equals(method) && formal.getBounds().length == 1 && formal.getBounds()[0] == Object.class);
        check(method.getGenericReturnType().equals(formal) && method.getGenericParameterTypes()[0].equals(formal));
        ParameterizedType callback = (ParameterizedType)method.getGenericParameterTypes()[1];
        check(callback.getRawType() == Bi.class);
        Type[] slots = callback.getActualTypeArguments(); check(slots.length == 3);
        WildcardType input = (WildcardType)slots[0], self = (WildcardType)slots[1], result = (WildcardType)slots[2];
        check(input.getLowerBounds().length == 1 && input.getLowerBounds()[0].equals(formal));
        check(self.getLowerBounds().length == 1 && self.getLowerBounds()[0] == Api.class);
        check(result.getUpperBounds().length == 1 && result.getUpperBounds()[0].equals(formal));
    }
    public static void main(String[] args) throws Throwable {
        int route = Integer.parseInt(args[0]), variant = Integer.parseInt(args[1]), mode = Integer.parseInt(args[2]); metadata();
        for (int attempt = 0; attempt < 2; attempt++) {
            Api receiver = mode == 3 ? null : variant == 0 ? new Plain() : new Override();
            Object seed = mode == 1 ? null : new Object();
            Object chosen = mode == 8 ? null : mode == 9 ? seed : mode == 10 ? receiver : new Object(), late = new Object();
            Callback callback = new Callback(seed, receiver, chosen, late);
            Bi<Object,Api,Object> operation = mode == 2 ? null : callback;
            Trace.events = 0; Trace.beforeReceiver = Trace.originalSeed = Trace.seenResult = null;
            Trace.seed = seed; Trace.lateSeed = new Object(); Trace.produced = chosen;
            Trace.failAt = mode == 4 ? 1 : mode == 5 ? 2 : mode == 6 ? 3 : mode == 7 ? -2 : mode == 12 ? -3 : 0;
            Trace.failure = new IllegalArgumentException("owned failure"); Trace.error = new LinkageError("owned error");
            Object actual = null; Throwable caught = null;
            try { actual = call(route, receiver, seed, operation); } catch (Throwable failure) { caught = failure; }
            boolean virtual = receiver != null && route == 0 && variant == 1;
            boolean before = receiver != null && !virtual && operation != null;
            boolean invoked = before && mode != 4, returned = invoked && mode != 5 && mode != 7;
            int events = receiver == null ? 0 : virtual ? 9 : operation == null ? 0 : mode == 4 ? 1 : mode == 5 || mode == 7 ? 12 : 123;
            if (receiver == null) check(caught instanceof NullPointerException);
            else if (virtual) check(caught == null && actual == chosen);
            else if (operation == null) check(caught instanceof NullPointerException);
            else if (Trace.failAt < 0) check(caught == Trace.error);
            else if (Trace.failAt > 0) check(caught == Trace.failure);
            else check(caught == null && actual == chosen);
            check(Trace.events == events && callback.calls == (invoked ? 1 : 0));
            check(Trace.beforeReceiver == (before ? receiver : null) && Trace.originalSeed == (before ? seed : null));
            check(Trace.seed == (before ? Trace.lateSeed : seed));
            check(Trace.seenResult == (returned ? chosen : null));
            check(Trace.produced == (returned ? late : chosen));
            if (caught != null) check(actual == null);
            System.out.println("route=" + route + ",variant=" + variant + ",mode=" + mode + ",attempt=" + attempt + ",events=" + events);
        }
    }
}
