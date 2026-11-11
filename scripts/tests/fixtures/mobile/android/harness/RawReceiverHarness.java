import fixture.*;
import java.lang.invoke.*;
import java.lang.reflect.*;
public final class RawReceiverHarness {
    public static final class Derived extends Provider<Object> {
        public <U> Box<? extends U> read(Box<U> input, Object tag, long bits, double value) {
            Trace.hit(4);
            Trace.receiver = this; Trace.input = input; Trace.tag = tag;
            Trace.bits = bits; Trace.value = value;
            Trace.hit(5);
            return input;
        }
    }
    static void require(boolean ok) { if (!ok) throw new AssertionError("raw receiver contract"); }
    static Box<?> call(boolean api, int route, Provider<Object> provider, Box<Object> input,
                       Object tag, long bits, double value) throws Throwable {
        if (route == 0) return api ? Caller.through(provider, input, tag, bits, value) : Caller.run(provider, input, tag, bits, value);
        if (route == 1) return api ? RawReceiverCaller.through(provider, input, tag, bits, value) : RawReceiverCaller.run(provider, input, tag, bits, value);
        String name = api ? "through" : "run";
        Class<?> type = api ? Api.class : Provider.class;
        if (route == 2) {
            try { return (Box<?>) Caller.class.getMethod(name, type, Box.class, Object.class, long.class, double.class)
                    .invoke(null, provider, input, tag, bits, value); }
            catch (InvocationTargetException failure) { throw failure.getCause(); }
        }
        MethodHandle target = MethodHandles.publicLookup().findStatic(Caller.class, name,
            MethodType.methodType(Box.class, type, Box.class, Object.class, long.class, double.class));
        return api ? (Box<?>) target.invokeExact((Api<Object>) provider, input, tag, bits, value)
                   : (Box<?>) target.invokeExact(provider, input, tag, bits, value);
    }
    static Class<?> token(int route, Class<?> candidate, Class<?> parent) throws Throwable {
        if (route == 0) return Caller.token(candidate, parent);
        if (route == 1) return RawReceiverCaller.token(candidate, parent);
        if (route == 2) {
            try { return (Class<?>) Caller.class.getMethod("token", Class.class, Class.class).invoke(null, candidate, parent); }
            catch (InvocationTargetException failure) { throw failure.getCause(); }
        }
        return (Class<?>) MethodHandles.publicLookup().findStatic(Caller.class, "token",
            MethodType.methodType(Class.class, Class.class, Class.class)).invokeExact(candidate, parent);
    }
    public static void main(String[] args) throws Throwable {
        int calls = 0, failures = 0, tokens = 0;
        for (int api = 0; api < 2; api++) for (int route = 0; route < 4; route++)
        for (int child = 0; child < 2; child++) for (int mode = 0; mode < 7; mode++) {
            Provider<Object> provider = mode == 5 ? null : child == 0 ? new Provider<>() : new Derived();
            Box<Object> input = mode == 6 ? null : new Box<>();
            int[] mutable = {19}; if (input != null) input.value = mutable;
            int before = child == 0 ? 2 : 4, after = child == 0 ? 3 : 5;
            Trace.failure = new IllegalArgumentException("owned provider failure");
            Trace.failAt = mode == 1 ? 1 : mode == 2 ? before : mode == 3 ? after : mode == 4 ? 6 : 0;
            for (int attempt = 0; attempt < 2; attempt++) {
                Trace.events = 0; Object sentinel = new Object();
                Trace.receiver = Trace.input = Trace.tag = sentinel;
                Trace.bits = 13; Trace.value = 17;
                Object tag = attempt == 0 ? mutable : null; mutable[0]++;
                long bits = attempt == 0 ? Long.MIN_VALUE + 71 : Long.MAX_VALUE - 37;
                long raw = attempt == 0 ? 0x7ff8123456789abcl : 0x8000000000000000l;
                Box<?> result = null; Throwable failure = null;
                try { result = call(api != 0, route, provider, input, tag, bits, Double.longBitsToDouble(raw)); }
                catch (Throwable caught) { failure = caught; }
                int expected = mode == 1 || mode == 5 ? 1 : mode == 2 ? 10 + before :
                    mode == 3 ? (10 + before) * 10 + after : ((10 + before) * 10 + after) * 10 + 6;
                require(Trace.events == expected);
                if (mode >= 1 && mode <= 4) require(failure == Trace.failure);
                else if (mode == 5) require(failure instanceof NullPointerException);
                else require(failure == null && result == input);
                if (failure != null) failures++;
                boolean reached = mode != 1 && mode != 2 && mode != 5;
                require(Trace.receiver == (reached ? provider : sentinel));
                require(Trace.input == (reached ? input : sentinel) && Trace.tag == (reached ? tag : sentinel));
                require(Trace.bits == (reached ? bits : 13));
                require(Double.doubleToRawLongBits(Trace.value) == (reached ? raw : Double.doubleToRawLongBits(17)));
                if (input != null) require(input.value == mutable && ((int[]) input.value)[0] == mutable[0]);
                calls++;
            }
        }
        Class<?>[] candidates = {Number.class, Integer.class, String.class, int[].class, Object.class, int.class, null, String.class};
        Class<?>[] parents = {Number.class, Number.class, CharSequence.class, Object.class, String.class, Object.class, Object.class, null};
        Trace.failAt = 0;
        for (int route = 0; route < 4; route++) for (int mode = 0; mode < 8; mode++)
        for (int attempt = 0; attempt < 2; attempt++) {
            Trace.events = 0; Throwable failure = null; Class<?> result = null;
            try { result = token(route, candidates[mode], parents[mode]); } catch (Throwable caught) { failure = caught; }
            if (mode < 4) require(failure == null && result == candidates[mode] && Trace.events == 67);
            else {
                require(Trace.events == 6);
                require(mode < 6 ? failure instanceof ClassCastException : failure instanceof NullPointerException);
                failures++;
            }
            tokens++;
        }
        require(calls == 224 && tokens == 64 && failures == 192);
        System.out.println("calls=" + calls + ",tokens=" + tokens + ",failures=" + failures);
        System.out.println("raw-receiver=exact,dispatch-and-identities=matched,wide-bits-and-ordered-failures=retained");
    }
}
