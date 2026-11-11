import fixture.*;
import java.lang.invoke.*;
import java.lang.reflect.*;
public final class StarredHeadNominalHarness {
    static int shape, mode, gets, conversions, writes;
    static boolean useConverter;
    static Object value, from, next;
    static Property property;
    static Converter converter;
    static Context context;
    static Token point;
    static Result wanted;
    static String events;
    static Throwable failure;
    static void check(boolean value) { if (!value) throw new AssertionError("owned starred head nominal"); }
    static <E extends Throwable> void raise(Throwable value) throws E { throw (E)value; }
    static final class Checked extends Exception {}
    static final class PropertyProbe implements Property {
        public Object read(Object actual) {
            gets++; events += "G,"; check(actual == value);
            if (mode == 6) StarredHeadNominalHarness.<RuntimeException>raise(failure);
            if (mode == 11) { context.point = point = new Token(); if (!useConverter || converter == null) next = point; }
            return from;
        }
        public void write(Object actual, Object element) {
            writes++; events += "W,"; check(actual == value && element == next && context.seen == next);
            if (mode == 8) StarredHeadNominalHarness.<RuntimeException>raise(failure);
        }
    }
    static final class ConverterProbe implements Converter<Token, Object> {
        public Object apply(Token actual) {
            conversions++; events += "C,"; check(actual == point);
            if (mode == 7) StarredHeadNominalHarness.<RuntimeException>raise(failure);
            if (mode == 12) Trace.result = wanted = Trace.alternative;
            return next;
        }
    }
    static final class Observer extends Trace {
        public void observe(int phase, Object actual, Property<?, ?> action, Converter<?, ?> conversion, Context target) {
            events += phase + ","; check(actual == value && action == property && conversion == (useConverter ? converter : null) && target == context);
            if (phase == 1) {
                if (mode == 5) StarredHeadNominalHarness.<RuntimeException>raise(failure);
            } else {
                check(gets == 1 && conversions == (useConverter && converter != null ? 1 : 0) && writes == 1 && target.seen == next);
                if (mode == 13) Trace.result = Trace.alternative;
                if (mode == 9) StarredHeadNominalHarness.<RuntimeException>raise(failure);
            }
        }
    }
    static Object call(int route) throws Throwable {
        if (route == 0) return shape == 0 ? Core.call(property, converter, context, useConverter) : discard();
        String name = shape == 0 ? "call" : "discard";
        if (route == 1) {
            try { return Core.class.getMethod(name, Property.class, Converter.class, Context.class, boolean.class)
                .invoke(null, property, converter, context, useConverter); }
            catch (InvocationTargetException problem) { throw problem.getCause(); }
        }
        if (route == 2) return MethodHandles.publicLookup().findStatic(Core.class, name,
            MethodType.methodType(shape == 0 ? Result.class : void.class, Property.class, Converter.class, Context.class, boolean.class))
            .invokeWithArguments(property, converter, context, useConverter);
        return StarredHeadNominalCaller.call(property, converter, context, useConverter, shape);
    }
    static Object discard() { Core.discard(property, converter, context, useConverter); return null; }
    static void metadata() throws Exception {
        Class<?> nested = Class.forName("fixture.Bridge$Factory");
        check(Modifier.isStatic(nested.getModifiers()));
        for (Class<?> owner : new Class<?>[]{nested, Provider.class}) {
            Method method = owner == nested ? owner.getDeclaredMethod("make", Property.class, Context.class)
                : owner.getMethod("make", Property.class, Converter.class, Context.class);
            TypeVariable<?>[] formals = method.getTypeParameters(); Type[] inputs = method.getGenericParameterTypes();
            check(!method.isVarArgs() && Modifier.isStatic(method.getModifiers()) && formals.length == 1 && formals[0].getBounds()[0] == Object.class);
            Type[] slots = ((ParameterizedType)inputs[0]).getActualTypeArguments();
            check(slots[0] instanceof WildcardType && slots[1].equals(formals[0]));
            WildcardType star = (WildcardType)slots[0]; check(star.getLowerBounds().length == 0 && star.getUpperBounds()[0] == Object.class);
            if (owner == Provider.class) {
                Type[] converted = ((ParameterizedType)inputs[1]).getActualTypeArguments();
                check(converted[0] == Token.class && converted[1].equals(formals[0]));
            }
            check(inputs[inputs.length - 1] == Context.class && method.getGenericReturnType() == Result.class);
        }
    }
    public static void main(String[] args) throws Throwable {
        int route = Integer.parseInt(args[0]); shape = Integer.parseInt(args[1]); mode = Integer.parseInt(args[2]); useConverter = Integer.parseInt(args[3]) != 0; metadata();
        for (int attempt = 0; attempt < 2; attempt++) {
            value = null;
            from = new Object(); next = mode == 10 ? new Object() : mode == 15 ? null : new String("next");
            point = mode == 4 || mode == 15 ? null : new Token(); Context original = new Context(point); Object initial = original.seen = new Object();
            context = mode == 3 ? null : original;
            property = mode == 1 ? null : new PropertyProbe(); converter = mode == 2 ? null : new ConverterProbe();
            if (!useConverter || converter == null) next = point;
            wanted = mode == 14 ? null : new Result(context); Trace.result = wanted; Trace.alternative = new Result(context); Trace.hook = new Observer();
            events = ""; gets = conversions = writes = 0;
            failure = mode == 5 || mode == 7 || mode == 9 ? new Checked() : mode == 8 ? new LinkageError("owned error") : new RuntimeException("owned failure");
            Object result = null; Throwable caught = null;
            try { result = call(route); } catch (Throwable problem) { caught = problem; }
            boolean got = mode != 1 && mode != 5;
            boolean prepared = got && mode != 3 && mode != 6;
            boolean converted = prepared && useConverter && converter != null;
            boolean stored = prepared && (!converted || mode != 7);
            boolean suffix = stored && mode != 8;
            String expected = "1," + (got ? "G," : "") + (converted ? "C," : "") + (stored ? "W," : "") + (suffix ? "2," : "");
            check(events.equals(expected) && gets == (got ? 1 : 0) && conversions == (converted ? 1 : 0) && writes == (stored ? 1 : 0));
            check(original.seen == (stored ? next : initial));
            if (mode == 5 || mode == 6 || (mode == 7 && converted) || mode == 8 || mode == 9) check(caught == failure);
            else if (mode == 1 || mode == 3) check(caught instanceof NullPointerException);
            else check(caught == null && result == (shape == 0 ? wanted : null));
        }
        System.out.println("attempts=2,starred-head-nominal=matched");
    }
}
