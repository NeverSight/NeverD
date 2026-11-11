import fixture.*;
import java.lang.invoke.*;
import java.lang.reflect.*;
public final class PairedSeededNominalHarness {
    static int shape, mode, calls, stores;
    static Object value;
    static Property property;
    static Context context;
    static Token token;
    static Result wanted;
    static String events;
    static Throwable failure;
    static void check(boolean value) { if (!value) throw new AssertionError("owned paired seeded nominal"); }
    static <E extends Throwable> void raise(Throwable value) throws E { throw (E)value; }
    static final class Checked extends Exception {}
    static final class Probe implements Property {
        public Object apply(Object actual, Context target) {
            calls++; events += "C,"; check(actual == value && target == context);
            if (mode == 5 || mode == 6) PairedSeededNominalHarness.<RuntimeException>raise(failure);
            if (mode == 11) Trace.result = wanted = Trace.alternative;
            return mode == 10 ? new Object() : token;
        }
    }
    static final class Observer extends Trace {
        public void observe(int phase, Object actual, Property<?, ?> action, Context target, Token actualToken) {
            events += phase + ","; check(actual == value && action == property && target == context);
            if (phase == 1) {
                check(actualToken == null && (target == null || target.state == 7));
                if (mode == 4) PairedSeededNominalHarness.<RuntimeException>raise(failure);
                if (mode == 13) Trace.result = wanted = Trace.alternative;
            } else {
                stores++; check(target.state == 8 && target.seen == value && actualToken == token);
                if (mode == 12) Trace.result = Trace.alternative;
                if (mode == 7 || mode == 15) PairedSeededNominalHarness.<RuntimeException>raise(failure);
            }
        }
    }
    static Object call(int route) throws Throwable {
        if (route == 0) return shape == 0 ? Core.call(value, property, context) : discard();
        String name = shape == 0 ? "call" : "discard";
        if (route == 1) {
            try { return Core.class.getMethod(name, Object.class, Property.class, Context.class).invoke(null, value, property, context); }
            catch (InvocationTargetException problem) { throw problem.getCause(); }
        }
        if (route == 2) return MethodHandles.publicLookup().findStatic(Core.class, name,
            MethodType.methodType(shape == 0 ? Result.class : void.class, Object.class, Property.class, Context.class))
            .invokeWithArguments(value, property, context);
        return PairedSeededNominalCaller.call(value, property, context, shape);
    }
    static Object discard() { Core.discard(value, property, context); return null; }
    static void metadata() throws Exception {
        Class<?> owner = Class.forName("fixture.Bridge$Factory");
        check((owner.getModifiers() & (Modifier.PUBLIC | Modifier.PROTECTED | Modifier.PRIVATE)) == 0);
        Method method = owner.getDeclaredMethod("make", Object.class, Property.class, Context.class);
        TypeVariable<?>[] formals = method.getTypeParameters();
        check(formals.length == 2 && formals[0].getBounds()[0] == Object.class && formals[1].getBounds()[0] == Object.class);
        check(Modifier.isStatic(method.getModifiers()) && (method.getModifiers() & (Modifier.PUBLIC | Modifier.PROTECTED | Modifier.PRIVATE)) == 0);
        Type[] inputs = method.getGenericParameterTypes();
        check(inputs[0].equals(formals[0]) && inputs[2] == Context.class && method.getGenericReturnType() == Result.class);
        Type[] slots = ((ParameterizedType)inputs[1]).getActualTypeArguments();
        check(slots.length == 2 && slots[0].equals(formals[0]) && slots[1].equals(formals[1]));
    }
    public static void main(String[] args) throws Throwable {
        int route = Integer.parseInt(args[0]); shape = Integer.parseInt(args[1]); mode = Integer.parseInt(args[2]); metadata();
        for (int attempt = 0; attempt < 2; attempt++) {
            value = mode == 2 ? null : mode == 3 ? new Object() : new String("seed " + attempt);
            Context original = new Context(); original.state = 7; original.seen = new Object(); Object initial = original.seen;
            context = mode == 8 ? null : original; property = mode == 1 ? null : new Probe(); token = mode == 14 ? null : new Token();
            wanted = mode == 9 ? null : new Result(value, context, token);
            Trace.result = wanted; Trace.alternative = new Result(value, context, token); Trace.hook = new Observer();
            events = ""; calls = stores = 0;
            failure = mode == 4 || mode == 7 ? new Checked() : mode == 6 || mode == 15 ? new LinkageError("owned error") : new RuntimeException("owned failure");
            Object result = null; Throwable caught = null;
            try { result = call(route); } catch (Throwable problem) { caught = problem; }
            boolean called = mode != 1 && mode != 4;
            boolean stored = called && mode != 5 && mode != 6 && mode != 8 && mode != 10;
            check(events.equals("1," + (called ? "C," : "") + (stored ? "2," : "")));
            check(calls == (called ? 1 : 0) && stores == (stored ? 1 : 0));
            check(original.state == (stored ? 8 : 7) && original.seen == (stored ? value : initial));
            if (mode == 4 || mode == 5 || mode == 6 || mode == 7 || mode == 15) check(caught == failure);
            else if (mode == 1 || mode == 8) check(caught instanceof NullPointerException);
            else if (mode == 10) check(caught instanceof ClassCastException);
            else check(caught == null && result == (shape == 0 ? wanted : null));
        }
        System.out.println("attempts=2,paired-seeded-nominal=matched");
    }
}
