import fixture.*;
import java.lang.invoke.*;
import java.lang.reflect.*;
@SuppressWarnings({"rawtypes", "unchecked"})
public final class LongScalarResultHarness {
    static int values, identities, states, failures;
    static void value(boolean ok) { if (!ok) throw new AssertionError("value"); values++; }
    static void same(Object a, Object b) { if (a != b) throw new AssertionError("identity"); identities++; }
    static void state(boolean ok) { if (!ok) throw new AssertionError("state"); states++; }
    static void write(int route, Core self, Token token, Object item) throws Throwable {
        try {
            switch (route) {
            case 0: self.write(token, item); break;
            case 1: ((Consumer)self).write(token, item); break;
            case 2: ((Api)self).write(token, item); break;
            case 3: Core.class.getMethod("write", Token.class, Object.class).invoke(self, token, item); break;
            case 4: MethodHandles.publicLookup().findVirtual(Core.class, "write", MethodType.methodType(void.class, Token.class, Object.class)).invokeExact(self, token, item); break;
            default: LongScalarResultCaller.write(self, token, item);
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
    static Object read(int route, Core self, Token token) throws Throwable {
        try {
            switch (route) {
            case 0: return self.read(token);
            case 1: return ((Producer)self).read(token);
            case 2: return ((Api)self).read(token);
            case 3: return Core.class.getMethod("read", Token.class).invoke(self, token);
            case 4: return (Object)MethodHandles.publicLookup().findVirtual(Core.class, "read", MethodType.methodType(Object.class, Token.class)).invokeExact(self, token);
            default: return LongScalarResultCaller.read(self, token);
            }
        } catch (InvocationTargetException error) { throw error.getCause(); }
    }
    static void checkResults() throws Throwable {
        for (int route = 0; route < 6; route++) {
            final int selected = route;
            Core self = new Core(); Token token = new Token();
            Carrier.boxes = 0; Carrier.boxFailure = null; Carrier.reuse = false; Carrier.returnNull = false;
            int expectedReads = 0;
            for (long bits : new long[]{Long.MIN_VALUE, Long.MAX_VALUE, -1, 0, 1}) {
                token.bits = bits;
                Carrier first = (Carrier)read(route, self, token);
                Carrier second = (Carrier)read(route, self, token);
                expectedReads += 2;
                value(first.value == bits && second.value == bits && Carrier.lastBits == bits);
                value(first != second); same(token.receiver, self);
                state(self.reads == expectedReads && token.reads == expectedReads && Carrier.boxes == expectedReads);
            }
            Carrier saved = new Carrier(41); Carrier.cached = saved; Carrier.reuse = true;
            same(read(route, self, token), saved); same(read(route, self, token), saved);
            Carrier.returnNull = true; same(read(route, self, token), null);
            state(self.reads == 13 && token.reads == 13 && Carrier.boxes == 13);
            RuntimeException marker = new IllegalStateException("owned result failure");
            self.readFailure = marker; Carrier.boxFailure = new IllegalArgumentException("owned later failure");
            failure(() -> read(selected, self, token), marker.getClass(), marker);
            state(self.reads == 14 && token.reads == 14 && Carrier.boxes == 13);
            self.readFailure = null; Carrier.boxFailure = marker; token.bits = Long.MIN_VALUE;
            failure(() -> read(selected, self, token), marker.getClass(), marker);
            state(self.reads == 15 && token.reads == 15 && Carrier.boxes == 14); value(Carrier.lastBits == Long.MIN_VALUE);
            failure(() -> read(selected, self, null), NullPointerException.class, null);
            state(self.reads == 16 && token.reads == 15 && Carrier.boxes == 14);
            failure(() -> read(selected, null, token), NullPointerException.class, null);
            state(self.reads == 16 && token.reads == 15 && Carrier.boxes == 14);
            value(self.readValue(token) == Long.MIN_VALUE);
            state(self.reads == 17 && token.reads == 16 && Carrier.boxes == 14);
            Carrier.boxFailure = null; Carrier.returnNull = false; Carrier.reuse = false;
        }
    }
    public static void main(String[] args) throws Throwable {
        boolean rebuilt = args.length != 0;
        Method entry = Core.class.getMethod("write", Token.class, Object.class);
        value(entry.isBridge() == !rebuilt); value(entry.isSynthetic() == !rebuilt);
        Type first = Core.class.getGenericInterfaces()[0];
        value(rebuilt ? first == Api.class : first instanceof ParameterizedType
            && ((ParameterizedType)first).getActualTypeArguments()[0] == Carrier.class);
        value(Core.class.getGenericInterfaces()[1].getTypeName().equals("fixture.Marker<fixture.Carrier>"));
        Method readerEntry = Core.class.getMethod("read", Token.class);
        value(readerEntry.getReturnType() == Object.class);
        value(readerEntry.isBridge() == !rebuilt); value(readerEntry.isSynthetic() == !rebuilt);
        for (int route = 0; route < 6; route++) {
            final int selected = route;
            for (Carrier item : new Carrier[]{new Carrier(Long.MIN_VALUE), new Carrier(Long.MAX_VALUE),
                    new Carrier(-1), new Carrier(0), new Carrier(1)}) {
                Core self = new Core(); Token token = new Token();
                write(route, self, token, item);
                value(self.value == item.value && token.accepted == item.value);
                state(item.calls == 1 && self.calls == 1 && token.writes == 1); same(token.receiver, self);
            }
            Core self = new Core(); Token token = new Token(); self.token = token;
            same(LongScalarResultCaller.meta(self), token); same(((Consumer)self).meta(), token);
            RuntimeException marker = new IllegalStateException("owned ordered failure");
            Carrier observed = new Carrier(Long.MIN_VALUE);
            write(route, self, token, observed);
            state(observed.calls == 1 && self.calls == 1 && token.writes == 1); value(self.value == Long.MIN_VALUE);
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
        }
        Core self = new Core(); Token token = new Token();
        for (long item : new long[]{Long.MIN_VALUE, Long.MAX_VALUE, -1, 0, 1}) {
            self.writeValue(token, item); value(self.value == item && token.accepted == item);
        }
        state(self.calls == 5 && token.writes == 5);
        checkResults();
        System.out.println("values="+values+",identities="+identities+",states="+states+",failures="+failures);
        System.out.println("inherited-consumer-producer-and-metadata-contracts=retained,original-scalar-unbox-box-and-named-delegate-effects=retained");
    }
}
