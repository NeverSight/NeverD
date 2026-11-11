import fixture.*;
import java.util.*;
import java.lang.invoke.*;
import java.lang.reflect.*;
public final class ClosedAuxiliaryVoidHarness {
    static int mode, tokenCalls, callbackCalls;
    static Registry registry;
    static Token<?> token;
    static Callback<?, ?> callback;
    static Object owner, type, action, selected, returned, beforeStored;
    static Carrier<?> target, marker;
    static String events;
    static Throwable failure;
    static void check(boolean value) { if (!value) throw new AssertionError("owned closed auxiliary void"); }
    static <E extends Throwable> void raise(Throwable failure) throws E { throw (E)failure; }
    static final class Checked extends Exception {}
    static final class ValueCarrier implements Carrier<Object> { public Object read() { return new Object(); } }
    static final class PoisonList extends AbstractList<Object> {
        public int size() { throw new AssertionError("owned closed auxiliary void"); }
        public Object get(int index) { throw new AssertionError("owned closed auxiliary void"); }
        public Object[] toArray() { throw new AssertionError("owned closed auxiliary void"); }
    }
    static final class TokenProbe implements Token<Object> {
        public Object read() {
            events += "T,"; tokenCalls++;
            if (mode == 8) ClosedAuxiliaryVoidHarness.<RuntimeException>raise(failure);
            if (mode == 19) registry.payload = selected = new PoisonList();
            return new Object();
        }
    }
    static final class CallbackProbe implements Callback<Object, Object> {
        public Object apply(Object value) {
            events += "C,"; callbackCalls++;
            check(value == selected);
            if (mode == 10) ClosedAuxiliaryVoidHarness.<RuntimeException>raise(failure);
            if (mode == 20) { registry.payload = new Object(); registry.stored = (Carrier<?>) (beforeStored = new ValueCarrier()); }
            if (mode == 21) returned = target = new ValueCarrier();
            return returned;
        }
    }
    static final class FutureRegistry extends Registry {
        public <T> void consume(Token<T> type, Callback<? super List<? extends Carrier<?>>, ? extends Carrier<?>> action) {
            check(this == registry && type == token && action == callback);
            events += "O,";
            super.consume(type, action);
        }
    }
    static final class Observer extends Trace {
        public void observe(int phase, Object receiver, Object type, Object action, Object result) {
            events += phase + ",";
            check(receiver == registry && type == token && action == callback);
            check(result == (phase == 2 || phase == 3 ? returned : null));
            if (phase == 2) check(registry.stored == beforeStored);
            if (phase == 3 || phase == 4) check(registry.stored == returned);
            if (phase == (mode == 7 ? 1 : mode == 13 ? 2 : mode == 14 ? 3 : mode == 15 ? 4 : mode == 23 ? 0 : -1))
                ClosedAuxiliaryVoidHarness.<RuntimeException>raise(failure);
        }
    }
    static void call(int route) throws Throwable {
        if (route == 0) Core.run(owner, type, action);
        else if (route == 1) Core.typed(owner, type, action);
        else if (route == 2) {
            try { Core.class.getMethod("run", Object.class, Object.class, Object.class).invoke(null, owner, type, action); }
            catch (InvocationTargetException failure) { throw failure.getCause(); }
        } else MethodHandles.publicLookup().findStatic(Core.class, "run",
            MethodType.methodType(void.class, Object.class, Object.class, Object.class)).invokeExact(owner, type, action);
    }
    public static void main(String[] args) throws Throwable {
        int route = Integer.parseInt(args[0]); mode = Integer.parseInt(args[1]);
        Method method = Registry.class.getMethod("consume", Token.class, Callback.class);
        check(method.getTypeParameters().length == 1 && method.getTypeParameters()[0].getBounds()[0] == Object.class);
        check(((ParameterizedType)method.getGenericParameterTypes()[0]).getActualTypeArguments()[0].equals(method.getTypeParameters()[0]));
        Type[] slots = ((ParameterizedType)method.getGenericParameterTypes()[1]).getActualTypeArguments();
        Type list = ((WildcardType)slots[0]).getLowerBounds()[0];
        Type element = ((WildcardType)((ParameterizedType)list).getActualTypeArguments()[0]).getUpperBounds()[0];
        check(((WildcardType)((ParameterizedType)element).getActualTypeArguments()[0]).getUpperBounds()[0] == Object.class);
        check(((ParameterizedType)((WildcardType)slots[1]).getUpperBounds()[0]).getRawType() == Carrier.class);
        for (int attempt = 0; attempt < 2; attempt++) {
            events = ""; tokenCalls = 0; callbackCalls = 0; Trace.hook = new Observer();
            Registry original = mode == 22 ? new FutureRegistry() : new Registry(); registry = mode == 1 ? null : original;
            token = mode == 2 ? null : new TokenProbe(); callback = mode == 3 ? null : new CallbackProbe();
            owner = mode == 4 ? new Object() : registry; type = mode == 5 ? new Object() : token; action = mode == 6 ? new Object() : callback;
            selected = mode == 9 ? new Object() : mode == 16 ? null : mode == 17 ? new PoisonList() : mode == 18 ? Arrays.asList(new Object()) : Collections.singletonList(new ValueCarrier());
            original.payload = selected; target = new ValueCarrier(); returned = mode == 11 ? new Object() : mode == 12 ? null : target;
            marker = new ValueCarrier(); original.stored = marker; beforeStored = marker;
            failure = mode == 8 || mode == 13 || mode == 23 ? new Checked() : mode == 14 ? new LinkageError("owned error") : new RuntimeException("owned failure");
            Throwable caught = null;
            try { call(route); } catch (Throwable problem) { caught = problem; }
            String expected = mode == 4 || mode == 5 || mode == 6 ? "" : "0,";
            boolean entered = !expected.isEmpty() && mode != 1 && mode != 23;
            if (entered) { if (mode == 22) expected += "O,"; expected += "1,"; }
            boolean calledToken = entered && mode != 7 && mode != 2;
            if (calledToken) expected += "T,";
            boolean calledCallback = calledToken && mode != 8 && mode != 9 && mode != 3;
            if (calledCallback) expected += "C,";
            boolean stored = calledCallback && mode != 10 && mode != 11 && mode != 13;
            if (calledCallback && mode != 10 && mode != 11) expected += "2,";
            if (stored) expected += "3,";
            if (stored && mode != 14) expected += "4,";
            check(events.equals(expected) && tokenCalls == (calledToken ? 1 : 0) && callbackCalls == (calledCallback ? 1 : 0));
            check(original.stored == (stored ? returned : beforeStored));
            if (mode == 7 || mode == 8 || mode == 10 || mode == 13 || mode == 14 || mode == 15 || mode == 23) check(caught == failure);
            else if (mode == 1 || mode == 2 || mode == 3) check(caught instanceof NullPointerException);
            else if (mode == 4 || mode == 5 || mode == 6 || mode == 9 || mode == 11) check(caught instanceof ClassCastException);
            else check(caught == null);
        }
        System.out.println("attempts=2,closed-auxiliary-void=matched");
    }
}
