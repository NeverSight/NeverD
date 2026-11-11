import fixture.*;
import java.lang.invoke.*;
import java.lang.reflect.*;
public final class TypedTokenConstructorTransferHarness {
    static int trace, failAt, mode;
    static RuntimeException failure;
    static Input suppliedInput;
    static Callback suppliedCallback;
    static Output base, alternate, expected;
    static Context context;
    static Object mark;
    static void check(boolean value) { if (!value) throw new AssertionError("owned typed token constructor transfer"); }
    static void hit(int n) { trace = trace * 10 + n; if (failAt == n) throw failure; }
    static final class FutureInput extends Input {
        int calls;
        public Object inspect() { calls++; hit(4); return mark; }
    }
    static final class FutureContext extends Context {
        public void check(Object value, String label) {
            int phase = label.equals("input") ? 1 : label.equals("callback") ? 2 : label.equals("map") ? 3 : 5;
            if (phase == 1) TypedTokenConstructorTransferHarness.check(value == suppliedInput && Mapper.escaping == null);
            else if (phase == 2) { TypedTokenConstructorTransferHarness.check(value == suppliedCallback && Mapper.escaping == null); if (mode == 12) { Mapper.replacement = alternate; expected = alternate; } }
            else if (phase == 3) TypedTokenConstructorTransferHarness.check(value == suppliedInput && Mapper.escaping == null);
            else { Core core = (Core) value; TypedTokenConstructorTransferHarness.check(core == Mapper.escaping && core.output == expected && core.callback == suppliedCallback && core.payload == expected.value && !core.done); }
            hit(phase);
        }
    }
    static final class FutureCallback implements Callback {
        int calls; Object reply;
        public Object apply(Object value) { calls++; check(value == context && Mapper.escaping.done); return reply; }
    }
    static Core make(Input input, Callback callback) { return new Core(input, callback); }
    static Core call(int route, Input input, Callback callback) throws Throwable {
        if (route == 0) return new Core<Bound>(input, callback);
        if (route == 1) { try { return Core.class.getConstructor(Input.class, Callback.class).newInstance(input, callback); } catch (InvocationTargetException error) { throw error.getCause(); } }
        if (route == 2) return (Core) MethodHandles.publicLookup().findConstructor(Core.class, MethodType.methodType(void.class, Input.class, Callback.class)).invokeExact(input, callback);
        return make(input, callback);
    }
    public static void main(String[] args) throws Throwable {
        int route = Integer.parseInt(args[0]); mode = Integer.parseInt(args[1]);
        TypeVariable<?> owner = Core.class.getTypeParameters()[0];
        check(owner.getBounds()[0] == Bound.class);
        for (Class<?> token : new Class<?>[] {Input.class, Output.class}) {
            ParameterizedType p = (ParameterizedType) Core.class.getConstructor(token, Callback.class).getGenericParameterTypes()[0];
            check(p.getRawType() == token && p.getActualTypeArguments()[0].equals(owner));
        }
        for (int attempt = 0; attempt < 2; attempt++) {
            trace = 0; failAt = mode == 3 ? 1 : mode == 4 ? 2 : mode == 5 ? 3 : mode == 6 ? 4 : 0;
            failure = new RuntimeException("owned prefix failure"); RuntimeException mappingFailure = new RuntimeException("owned mapping failure"), constructorFailure = new RuntimeException("owned target failure");
            FutureInput input = new FutureInput(); FutureCallback callback = new FutureCallback(); context = new FutureContext();
            suppliedInput = mode == 1 ? null : input; suppliedCallback = mode == 2 ? null : callback;
            base = new Output(); alternate = new Output();
            base.value = mode == 11 ? null : mode == 14 ? new Long(Long.MIN_VALUE + attempt) : mode == 15 ? new Long(Long.MAX_VALUE - attempt) : attempt == 0 ? new String("owned") : new Object();
            alternate.value = base.value;
            expected = mode == 8 ? null : base;
            Mapper.observer = context; Mapper.replacement = expected; Mapper.escaping = null;
            Mapper.checked = Mapper.marker = null; Mapper.label = null; Mapper.saved = null; mark = new Object();
            Mapper.failure = mode == 7 ? mappingFailure : null; Mapper.constructorFailure = mode == 9 ? constructorFailure : null;
            Bound bound = new Bound(); callback.reply = mode == 10 ? new Object() : bound;
            Core result = null; Throwable caught = null;
            try { result = call(route, suppliedInput, suppliedCallback); } catch (Throwable error) { caught = error; }
            boolean success = mode == 0 || mode >= 10;
            if (success) check(caught == null && result == Mapper.escaping);
            else if (mode == 1 || mode == 2 || mode == 8) check(caught instanceof NullPointerException);
            else check(caught == (mode == 7 ? mappingFailure : mode == 9 ? constructorFailure : failure));
            int stage = mode == 1 || mode == 3 ? 1 : mode == 2 || mode == 4 ? 2 : mode == 5 ? 3 : mode == 6 || mode == 7 || mode == 8 ? 4 : 5;
            int expectedTrace = stage == 1 ? 1 : stage == 2 ? 12 : stage == 3 ? 123 : stage == 4 ? 1234 : 12345;
            check(trace == expectedTrace && input.calls == (stage >= 4 ? 1 : 0));
            check(Mapper.saved == (stage >= 4 ? suppliedInput : null) && Mapper.marker == (stage >= 4 && mode != 6 ? mark : null));
            check(Mapper.checked == (stage == 1 ? suppliedInput : suppliedCallback) && Mapper.label.equals(stage == 1 ? "input" : "callback"));
            if (stage < 5 && mode != 8) check(Mapper.escaping == null);
            else { Core escaped = Mapper.escaping; check(escaped != null && escaped.output == expected && escaped.callback == suppliedCallback && escaped.payload == (mode == 8 ? null : expected.value) && escaped.done == success); }
            if (success) {
                try { Bound value = (Bound) result.callback.apply(context); check(mode != 10 && value == bound); }
                catch (ClassCastException error) { check(mode == 10); }
                check(callback.calls == 1);
                Object future = new Object(); callback.reply = future;
                check(result.callback.apply(context) == future && callback.calls == 2);
                try { String value = (String) result.payload; check(mode == 11 ? value == null : mode != 14 && mode != 15 && attempt == 0 && value == expected.value); }
                catch (ClassCastException error) { check(mode == 14 || mode == 15 || attempt == 1 && mode != 11); }
            }
            System.out.println("route=" + route + ",mode=" + mode + ",attempt=" + attempt + ",trace=" + expectedTrace);
        }
    }
}
