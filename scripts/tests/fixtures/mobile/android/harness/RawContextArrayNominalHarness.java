import fixture.*;
import java.lang.invoke.*;
import java.lang.reflect.*;
public final class RawContextArrayNominalHarness {
    static int shape, mode, gets, evaluations, writes;
    static Object from, to, next;
    static Property property;
    static Evaluator evaluator;
    static Object[] values;
    static Result wanted;
    static String events;
    static Throwable failure;
    static void check(boolean value) { if (!value) throw new AssertionError("owned raw context array nominal"); }
    static <E extends Throwable> void raise(Throwable value) throws E { throw (E)value; }
    static final class Checked extends Exception {}
    static final class PropertyProbe implements Property {
        public Object read(Object actual) {
            gets++; events += "G,"; check(actual == null);
            if (mode == 6) RawContextArrayNominalHarness.<RuntimeException>raise(failure);
            if (mode == 11) { to = new String("changed"); values[0] = to; }
            return from;
        }
        public void write(Object actual, Object element) {
            writes++; events += "W,"; check(actual == null && element == next && values[0] == next);
            if (mode == 8) RawContextArrayNominalHarness.<RuntimeException>raise(failure);
        }
    }
    static final class EvaluatorProbe implements Evaluator<Object> {
        public Object apply(Object actualFrom, Object actualTo) {
            evaluations++; events += "E,"; check(actualFrom == from && actualTo == to);
            if (mode == 7) RawContextArrayNominalHarness.<RuntimeException>raise(failure);
            if (mode == 12) Trace.result = wanted = Trace.alternative;
            return next;
        }
    }
    static final class Observer extends Trace {
        public void observe(int phase, Property<?, ?> action, Evaluator<?> evaluation, Object[] array) {
            events += phase + ","; check(action == property && evaluation == evaluator && array == values);
            if (phase == 1) {
                if (mode == 5) RawContextArrayNominalHarness.<RuntimeException>raise(failure);
            } else {
                check(gets == 1 && evaluations == 1 && writes == 1 && array[0] == next);
                if (mode == 13) Trace.result = Trace.alternative;
                if (mode == 9) RawContextArrayNominalHarness.<RuntimeException>raise(failure);
            }
        }
    }
    static Object call(int route) throws Throwable {
        if (route == 0) return shape == 0 ? Core.call(property, evaluator, values) : discard();
        String name = shape == 0 ? "call" : "discard";
        if (route == 1) {
            try { return Core.class.getMethod(name, Property.class, Evaluator.class, Object[].class)
                .invoke(null, property, evaluator, values); }
            catch (InvocationTargetException problem) { throw problem.getCause(); }
        }
        if (route == 2) return MethodHandles.publicLookup().findStatic(Core.class, name,
            MethodType.methodType(shape == 0 ? Result.class : void.class, Property.class, Evaluator.class, Object[].class))
            .invokeWithArguments(property, evaluator, values);
        return RawContextArrayNominalCaller.call(property, evaluator, values, shape);
    }
    static Object discard() { Core.discard(property, evaluator, values); return null; }
    static void metadata() throws Exception {
        Method method = Provider.class.getMethod("make", Property.class, Evaluator.class, Object[].class);
        TypeVariable<?>[] formals = method.getTypeParameters(); Type[] inputs = method.getGenericParameterTypes();
        check(method.isVarArgs() && Modifier.isStatic(method.getModifiers()) && formals.length == 1);
        check(formals[0].getBounds()[0] == Object.class && inputs[0] == Property.class);
        check(((ParameterizedType)inputs[1]).getActualTypeArguments()[0].equals(formals[0]));
        check(((GenericArrayType)inputs[2]).getGenericComponentType().equals(formals[0]) && method.getGenericReturnType() == Result.class);
    }
    public static void main(String[] args) throws Throwable {
        int route = Integer.parseInt(args[0]); shape = Integer.parseInt(args[1]); mode = Integer.parseInt(args[2]); metadata();
        for (int attempt = 0; attempt < 2; attempt++) {
            from = new String("from"); to = new String("to"); next = mode == 10 ? new Object() : mode == 15 ? null : new String("next");
            values = mode == 3 ? null : mode == 4 ? new Object[0] : mode == 10 ? new String[]{(String)to} : new Object[]{to};
            Object original = values != null && values.length != 0 ? values[0] : null;
            property = mode == 1 ? null : new PropertyProbe(); evaluator = mode == 2 ? null : new EvaluatorProbe();
            wanted = mode == 14 ? null : new Result(values); Trace.result = wanted; Trace.alternative = new Result(values); Trace.hook = new Observer();
            events = ""; gets = evaluations = writes = 0;
            failure = mode == 5 || mode == 7 || mode == 9 ? new Checked() : mode == 8 ? new LinkageError("owned error") : new RuntimeException("owned failure");
            Object result = null; Throwable caught = null;
            try { result = call(route); } catch (Throwable problem) { caught = problem; }
            boolean got = mode != 1 && mode != 5;
            boolean evaluated = got && mode != 2 && mode != 3 && mode != 4 && mode != 6;
            boolean stored = evaluated && mode != 7 && mode != 10;
            boolean suffix = stored && mode != 8;
            String expected = "1," + (got ? "G," : "") + (evaluated ? "E," : "") + (stored ? "W," : "") + (suffix ? "2," : "");
            check(events.equals(expected) && gets == (got ? 1 : 0) && evaluations == (evaluated ? 1 : 0) && writes == (stored ? 1 : 0));
            if (values != null && values.length != 0) check(values[0] == (stored ? next : original));
            if (mode == 5 || mode == 6 || mode == 7 || mode == 8 || mode == 9) check(caught == failure);
            else if (mode == 1 || mode == 2 || mode == 3) check(caught instanceof NullPointerException);
            else if (mode == 4) check(caught instanceof ArrayIndexOutOfBoundsException);
            else if (mode == 10) check(caught instanceof ArrayStoreException);
            else check(caught == null && result == (shape == 0 ? wanted : null));
        }
        System.out.println("attempts=2,raw-context-array-nominal=matched");
    }
}
