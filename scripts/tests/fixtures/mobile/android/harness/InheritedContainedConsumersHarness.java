import fixture.*;
import java.lang.invoke.MethodHandle;
import java.lang.invoke.MethodHandles;
import java.lang.invoke.MethodType;
import java.lang.reflect.InvocationTargetException;
import java.lang.reflect.Method;
import java.lang.reflect.ParameterizedType;
import java.lang.reflect.WildcardType;

public final class InheritedContainedConsumersHarness {
    private static int values, identities, states, failures, effects;
    private static final Value VALUE = new Value(11);
    private static Object returned;
    private static RuntimeException failure;
    private static Method coreMethod, rootMethod;
    private static MethodHandle coreHandle, rootHandle;
    private static final class Provider implements Middle<Integer> {
        public Object request(Consumer<? super Integer> input, Consumer<? super Value> output) {
            effects = effects * 10 + 1;
            if (input != null) input.accept(7);
            if (output != null) output.accept(VALUE);
            effects = effects * 10 + 4;
            if (failure != null) throw failure;
            return returned;
        }
    }
    private static final class Override extends Core<Integer> {
        Override() { super(); }
        public Object request(Consumer<? super Integer> input, Consumer<? super Value> output) {
            effects = effects * 10 + 5;
            return super.request(input, output);
        }
    }
    private static void same(Object expected, Object actual) {
        if (expected != actual) throw new AssertionError("identity changed");
        identities++;
    }
    private static void state(int expected, int actual) {
        if (expected != actual) throw new AssertionError("state changed: " + expected + ":" + actual);
        states++;
    }
    private static Object call(int route, Core<Integer> core, Consumer input, Consumer output) throws Throwable {
        try {
            switch (route) {
                case 0: return core.request(input, output);
                case 1: return ((Root<Integer>) core).request(input, output);
                case 2: return ((Middle<Integer>) core).request(input, output);
                case 3: return ((Leaf<Integer>) core).request(input, output);
                case 4: return coreMethod.invoke(core, input, output);
                case 5: return rootMethod.invoke(core, input, output);
                case 6: return (Object) coreHandle.invokeExact((Core) core, input, output);
                case 7: return (Object) rootHandle.invokeExact((Root) core, input, output);
                default: throw new AssertionError();
            }
        } catch (InvocationTargetException error) { throw error.getCause(); }
    }
    private static void metadata(boolean original) throws Exception {
        coreMethod = Core.class.getDeclaredMethod("request", Consumer.class, Consumer.class);
        rootMethod = Root.class.getMethod("request", Consumer.class, Consumer.class);
        MethodType type = MethodType.methodType(Object.class, Consumer.class, Consumer.class);
        coreHandle = MethodHandles.lookup().findVirtual(Core.class, "request", type);
        rootHandle = MethodHandles.lookup().findVirtual(Root.class, "request", type);
        for (Class<?> owner : new Class<?>[]{Core.class, Middle.class}) {
            Method method = owner.getDeclaredMethod("request", Consumer.class, Consumer.class);
            WildcardType input = (WildcardType) ((ParameterizedType) method.getGenericParameterTypes()[0]).getActualTypeArguments()[0];
            WildcardType output = (WildcardType) ((ParameterizedType) method.getGenericParameterTypes()[1]).getActualTypeArguments()[0];
            if (!input.getLowerBounds()[0].equals(owner.getTypeParameters()[0])
                    || output.getUpperBounds()[0] != Object.class
                    || output.getLowerBounds().length != (original ? 0 : 1)
                    || (!original && output.getLowerBounds()[0] != Value.class)
                    || method.isBridge() || method.isSynthetic())
                throw new AssertionError("original scoped parameter or reported wildcard projection changed");
        }
        if (FinalCore.class.getMethod("request", Consumer.class, Consumer.class).getDeclaringClass() != Core.class)
            throw new AssertionError("inherited body was copied");
    }
    public static void main(String[] args) throws Throwable {
        metadata(args[0].equals("original"));
        Provider provider = new Provider();
        Core<Integer>[] receivers = new Core[]{new Core<Integer>(), new FinalCore<Integer>(), new Override()};
        for (Core<Integer> core : receivers) core.delegate = provider;
        Consumer<Integer> input = new Consumer<Integer>() {
            public void accept(Integer value) { if (value != 7) throw new AssertionError(); effects = effects * 10 + 2; }
        };
        Consumer<Value> output = new Consumer<Value>() {
            public void accept(Value value) { same(VALUE, value); value.value++; effects = effects * 10 + 3; }
        };
        Consumer<String> wrong = new Consumer<String>() { public void accept(String value) { throw new AssertionError("wrong callback entered"); } };
        Object[] results = {null, VALUE, new int[]{3}};
        for (Core<Integer> core : receivers) for (int route = 0; route < 8; route++) {
            int prefix = core instanceof Override ? 5 : 0;
            for (Object result : results) {
                returned = result; effects = 0; int before = VALUE.value;
                same(result, call(route, core, input, output));
                state(prefix * 10000 + 1234, effects); state(before + 1, VALUE.value); state(7, core.seed);
                if (result instanceof int[]) ((int[]) result)[0]++;
                effects = 0;
                same(result, call(route, core, null, null));
                state(prefix * 100 + 14, effects);
                values += 2;
            }
            failure = new RuntimeException("owned-provider-failure"); effects = 0; int before = VALUE.value;
            try { call(route, core, input, output); throw new AssertionError("failure missing"); }
            catch (RuntimeException error) { same(failure, error); failures++; }
            state(prefix * 10000 + 1234, effects); state(before + 1, VALUE.value); failure = null;
            effects = 0; before = VALUE.value;
            try { call(route, core, input, wrong); throw new AssertionError("wrong callback accepted"); }
            catch (ClassCastException error) { failures++; }
            state(prefix * 100 + 12, effects); state(before, VALUE.value);
            effects = 0;
            try { call(route, null, input, output); throw new AssertionError("null receiver accepted"); }
            catch (NullPointerException error) { failures++; }
            state(0, effects);
        }
        for (int route = 0; route < 8; route++) {
            effects = 0;
            try { call(route, new Core<Integer>(), input, output); throw new AssertionError("null delegate accepted"); }
            catch (NullPointerException error) { failures++; }
            state(0, effects);
        }
        System.out.println("values=" + values + ",identities=" + identities + ",states=" + states + ",failures=" + failures);
        System.out.println("complete-scoped-ancestry-and-original-dispatch=retained,consumer-projections=reported");
    }
}
