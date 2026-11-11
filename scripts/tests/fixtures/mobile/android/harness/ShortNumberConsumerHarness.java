import fixture.*;
import java.lang.invoke.*;
import java.lang.reflect.*;
@SuppressWarnings({"rawtypes", "unchecked"})
public final class ShortNumberConsumerHarness {
    static int values, identities, states, failures;
    static void value(boolean ok) { if (!ok) throw new AssertionError("value"); values++; }
    static void same(Object a, Object b) { if (a != b) throw new AssertionError("identity"); identities++; }
    static void state(boolean ok) { if (!ok) throw new AssertionError("state"); states++; }
    public static final class ObservedNumber extends Number {
        public int calls; public RuntimeException failure; public short result;
        public int intValue() { throw new AssertionError("wrong conversion"); }
        public long longValue() { throw new AssertionError("wrong conversion"); }
        public float floatValue() { throw new AssertionError("wrong conversion"); }
        public double doubleValue() { throw new AssertionError("wrong conversion"); }
        public short shortValue() { calls++; if (failure != null) throw failure; return result; }
    }
    public static final class DefaultNumber extends Number {
        public int calls, result; public RuntimeException failure;
        public int intValue() { calls++; if (failure != null) throw failure; return result; }
        public long longValue() { throw new AssertionError("wrong conversion"); }
        public float floatValue() { throw new AssertionError("wrong conversion"); }
        public double doubleValue() { throw new AssertionError("wrong conversion"); }
    }
    static void write(int route, Core self, Token token, Object item) throws Throwable {
        try {
            switch (route) {
            case 0: self.write(token, item); break;
            case 1: ((Consumer)self).write(token, item); break;
            case 2: ((Api)self).write(token, item); break;
            case 3: Core.class.getMethod("write", Token.class, Object.class).invoke(self, token, item); break;
            case 4: MethodHandles.publicLookup().findVirtual(Core.class, "write", MethodType.methodType(void.class, Token.class, Object.class)).invokeExact(self, token, item); break;
            default: ShortNumberConsumerCaller.write(self, token, item);
            }
        } catch (InvocationTargetException error) { throw error.getCause(); }
    }
    interface Action { void run() throws Throwable; }
    static void failure(Action action, Class<?> kind, Throwable original) throws Throwable {
        try { action.run(); throw new AssertionError("missing failure"); }
        catch (Throwable problem) {
            if (problem.getClass() != kind) throw new AssertionError("wrong failure", problem);
            if (original != null) same(problem, original);
            failures++;
        }
    }
    public static void main(String[] args) throws Throwable {
        boolean rebuilt = args.length != 0;
        Method entry = Core.class.getMethod("write", Token.class, Object.class);
        value(entry.isBridge() == !rebuilt); value(entry.isSynthetic() == !rebuilt);
        Type first = Core.class.getGenericInterfaces()[0];
        value(rebuilt ? first == Api.class : first instanceof ParameterizedType
            && ((ParameterizedType)first).getActualTypeArguments()[0] == Short.class);
        value(Core.class.getGenericInterfaces()[1].getTypeName().equals("fixture.Marker<java.lang.Short>"));
        value(Core.class.getMethod("read", Token.class).getReturnType() == Short.class);
        for (int route = 0; route < 6; route++) {
            final int selected = route;
            for (Number item : new Number[]{Short.MIN_VALUE, Short.MAX_VALUE, Long.MIN_VALUE, Long.MAX_VALUE,
                    -1L, 0L, 1L, Integer.MIN_VALUE, Integer.MAX_VALUE, (short)-7, (byte)3, 1.75f, Double.NaN,
                    Double.NEGATIVE_INFINITY, Double.POSITIVE_INFINITY, 128, -129, 32768, -32769, 65535}) {
                Core self = new Core(); Token token = new Token();
                write(route, self, token, item);
                value(self.value == item.shortValue() && token.accepted == item.shortValue());
                state(self.calls == 1 && token.writes == 1); same(token.receiver, self);
            }
            Core self = new Core(); Token token = new Token(); self.token = token;
            Short saved = Short.valueOf((short)101); token.saved = saved;
            same(self.read(token), saved); same(((Producer)self).read(token), saved);
            same(ShortNumberConsumerCaller.read(self, token), saved);
            same(ShortNumberConsumerCaller.meta(self), token); same(((Consumer)self).meta(), token);
            MethodHandle reader = MethodHandles.publicLookup().findVirtual(Core.class, "read", MethodType.methodType(Object.class, Token.class));
            same((Object)reader.invokeExact(self, token), saved);
            token.saved = null; same(ShortNumberConsumerCaller.read(self, token), null);
            RuntimeException marker = new IllegalStateException("owned ordered failure");
            ObservedNumber observed = new ObservedNumber(); observed.result = Short.MIN_VALUE;
            write(route, self, token, observed);
            state(observed.calls == 1 && self.calls == 1 && token.writes == 1); value(self.value == Short.MIN_VALUE);
            observed.failure = marker;
            failure(() -> write(selected, self, null, observed), marker.getClass(), marker);
            state(observed.calls == 2 && self.calls == 1 && token.writes == 1);
            observed.failure = null;
            failure(() -> write(selected, self, null, observed), NullPointerException.class, null);
            state(observed.calls == 3 && self.calls == 2 && token.writes == 1);
            failure(() -> write(selected, self, null, new Object()), ClassCastException.class, null);
            failure(() -> write(selected, self, null, null), NullPointerException.class, null);
            state(self.calls == 2 && token.writes == 1);
            self.failure = marker; token.accepted = 123;
            failure(() -> write(selected, self, token, observed), marker.getClass(), marker);
            state(observed.calls == 4 && self.calls == 3 && token.writes == 2); value(token.accepted == 123);
            same(token.receiver, self);
            failure(() -> write(selected, null, token, observed), NullPointerException.class, null);
            state(observed.calls == 4 && token.writes == 2);
            for (int raw : new int[]{Integer.MIN_VALUE, Integer.MAX_VALUE, 65535, -65537}) {
                Core converted = new Core(); Token sink = new Token(); DefaultNumber fallback = new DefaultNumber();
                fallback.result = raw;
                write(route, converted, sink, fallback);
                state(fallback.calls == 1 && converted.calls == 1 && sink.writes == 1);
                value(converted.value == (short)raw && sink.accepted == (short)raw); same(sink.receiver, converted);
                fallback.failure = marker;
                failure(() -> write(selected, converted, null, fallback), marker.getClass(), marker);
                state(fallback.calls == 2 && converted.calls == 1 && sink.writes == 1);
            }
        }
        Core self = new Core(); Token token = new Token();
        for (short item : new short[]{Short.MIN_VALUE, Short.MAX_VALUE, -1, 0, 1}) {
            self.write(token, item); value(self.value == item && token.accepted == item);
        }
        state(self.calls == 5 && token.writes == 5);
        System.out.println("values="+values+",identities="+identities+",states="+states+",failures="+failures);
        System.out.println("inherited-consumer-producer-and-metadata-contracts=retained,narrow-overrides-default-conversion-and-truncation=retained");
    }
}
