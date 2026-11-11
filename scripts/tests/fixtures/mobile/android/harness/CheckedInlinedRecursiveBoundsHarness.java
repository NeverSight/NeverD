import fixture.Api;
import fixture.Core;
import fixture.Other;
import fixture.Holder;
import fixture.Use;
import java.lang.reflect.TypeVariable;
import java.lang.reflect.WildcardType;
import java.lang.invoke.MethodHandle;
import java.lang.invoke.MethodHandles;
import java.lang.invoke.MethodType;
import java.lang.reflect.InvocationTargetException;
import java.lang.reflect.Method;
import java.lang.reflect.ParameterizedType;

public final class CheckedInlinedRecursiveBoundsHarness {
    private static int values;
    private static int identities;
    private static int states;
    private static int failures;
    private static final Method ORIGINAL;
    private static final Method INTERFACE;
    private static final MethodHandle HANDLE;
    static {
        try {
            ORIGINAL = Core.class.getMethod("read", Object.class);
            INTERFACE = Api.class.getMethod("read", Object.class);
            HANDLE = MethodHandles.lookup().findVirtual(Api.class, "read",
                MethodType.methodType(int.class, Object.class));
        } catch (Exception error) { throw new AssertionError(error); }
    }
    private static void state(int expected, int actual) {
        if (expected != actual) throw new AssertionError("state mismatch " + expected + ":" + actual);
        states++;
    }
    private static void same(Object expected, Object actual) {
        if (expected != actual) throw new AssertionError("identity changed");
        identities++;
    }
    private static int call(int route, Core self, Object input) throws Throwable {
        try {
            if (route >= 6) {
                Holder<Core> holder = new Holder<Core>(self);
                Use use = new Use(holder);
                same(holder, use.selected());
                same(self, holder.value());
                same(self, use.value());
                switch (route) {
                    case 6: return holder.read((Core) input);
                    case 7: return ((Holder) holder).read((Api) input);
                    case 8: return use.read((Core) input);
                    case 9: return ((Api) use.value()).read(input);
                    case 10: return ((Api<Core>) (Api) use.value()).read((Core) input);
                    default: throw new AssertionError();
                }
            }
            switch (route) {
                case 0: return self.read(input);
                case 1: return ((Api) self).read(input);
                case 2: return (Integer) ORIGINAL.invoke(self, input);
                case 3: return (Integer) INTERFACE.invoke(self, input);
                case 4: return (int) HANDLE.invokeExact((Api) self, input);
                case 5: return ((Api<Core>) (Api) self).read((Core) input);
                default: throw new AssertionError();
            }
        } catch (InvocationTargetException error) { throw error.getCause(); }
    }
    private static void metadata(boolean original) throws Exception {
        if (Core.class.getMethod("unbox").getReturnType() != long.class
                || ORIGINAL.getReturnType() != int.class
                || ORIGINAL.getParameterTypes()[0] != Object.class
                || ORIGINAL.getGenericParameterTypes()[0] != Object.class
                || ORIGINAL.isBridge() != original || ORIGINAL.isSynthetic() != original
                || !Api.class.getMethod("read", Object.class).getGenericParameterTypes()[0]
                       .equals(Api.class.getTypeParameters()[0]))
            throw new AssertionError("original runtime entry or interface scope changed");
        ParameterizedType api = (ParameterizedType) Core.class.getGenericInterfaces()[0];
        if (api.getRawType() != Api.class
                || api.getActualTypeArguments()[0] != (original ? Core.class : Object.class))
            throw new AssertionError("reported complete interface parameter projection lost");
        TypeVariable<?> formal = Holder.class.getTypeParameters()[0];
        ParameterizedType bound = (ParameterizedType) formal.getBounds()[0];
        WildcardType parameter = (WildcardType) bound.getActualTypeArguments()[0];
        ParameterizedType selected = (ParameterizedType) Use.class.getField("selected").getGenericType();
        if (bound.getRawType() != Api.class || parameter.getLowerBounds().length != 1
                || !parameter.getLowerBounds()[0].equals(formal)
                || selected.getRawType() != Holder.class || selected.getActualTypeArguments()[0] != Core.class
                || !Holder.class.getMethod("value").getGenericReturnType().equals(formal)
                || !Holder.class.getMethod("read", Api.class).getGenericParameterTypes()[0].equals(formal))
            throw new AssertionError("original complete recursive bound or scoped member changed");
    }
    private static void success(int route, long own, long other, boolean alias) throws Throwable {
        Core self = new Core(own);
        Core input = alias ? self : new Core(other);
        int[] marker = {7};
        self.marker = marker;
        input.marker = marker;
        int expected = alias ? 0 : own == other ? 0 : own < other ? -1 : 1;
        if (call(route, self, input) != expected) throw new AssertionError("comparison changed");
        values++;
        state(alias ? 2 : 1, self.calls);
        state(alias ? 2 : 1, input.calls);
        state(13, marker[0]);
        same(marker, self.marker);
        same(marker, input.marker);
        same(marker, self.observed);
        same(marker, input.observed);
    }
    private static void failed(int route, int kind) throws Throwable {
        Core self = new Core(Long.MIN_VALUE);
        Core input = kind == 4 ? self : new Core(Long.MAX_VALUE);
        int[] marker = {11};
        self.marker = marker;
        input.marker = marker;
        RuntimeException own = new RuntimeException("owned-receiver");
        RuntimeException other = new RuntimeException("owned-argument");
        Object argument = input;
        Throwable expected;
        int selfCalls = 0, inputCalls = 0, increment = 0;
        if (kind == 0) {
            argument = new Other();
            self.failure = own;
            expected = null; // entry cast must preempt the receiver failure
        } else if (kind == 1) {
            argument = null;
            self.failure = own;
            expected = null; // null argument must fail before receiver unboxing
        } else if (kind == 2) {
            input.failure = other;
            self.failure = own;
            expected = other;
            inputCalls = 1;
            increment = 3;
        } else if (kind == 3) {
            self.failure = own;
            expected = own;
            selfCalls = inputCalls = 1;
            increment = 6;
        } else {
            self.failure = own;
            expected = own;
            selfCalls = inputCalls = 1;
            increment = 3;
        }
        try {
            call(route, self, argument);
            throw new AssertionError("expected failure");
        } catch (Throwable error) {
            if (error instanceof AssertionError) throw error;
            if (expected != null) same(expected, error);
            else if (kind == 0 ? !(error instanceof ClassCastException)
                               : !(error instanceof NullPointerException))
                throw new AssertionError("ordered entry failure changed", error);
            failures++;
        }
        state(selfCalls, self.calls);
        state(inputCalls, input.calls);
        state(11 + increment, marker[0]);
        same(marker, self.marker);
        same(marker, input.marker);
        same(selfCalls == 0 ? null : marker, self.observed);
        same(inputCalls == 0 ? null : marker, input.observed);
    }
    public static void main(String[] args) throws Throwable {
        metadata(args[0].equals("original"));
        long[] edges = {Long.MIN_VALUE, -1, 0, 1, Long.MAX_VALUE};
        for (int route = 0; route < 11; route++) {
            for (long own : edges)
                for (long other : edges) success(route, own, other, false);
            success(route, Long.MIN_VALUE, Long.MIN_VALUE, true);
            for (int kind = 0; kind < 5; kind++) failed(route, kind);
            Core untouched = new Core(4);
            int[] marker = {19};
            untouched.marker = marker;
            try {
                call(route, null, untouched);
                throw new AssertionError("null receiver accepted");
            } catch (NullPointerException expected) { failures++; }
            state(0, untouched.calls);
            state(19, marker[0]);
            same(null, untouched.observed);
            same(marker, untouched.marker);
        }
        System.out.println("values=" + values + ",identities=" + identities
            + ",states=" + states + ",failures=" + failures);
        System.out.println("original-inlined-bridge-body=retained,cast-before-argument-before-receiver-effects=retained,object-interface-argument-and-bridge-flags=reported,complete-recursive-lower-bounds=retained");
    }
}
