import fixture.*;
import java.util.*;
import java.lang.reflect.*;
import java.lang.invoke.*;

public class ErasedTerminalConsumersHarness {
    static int values, identities, states, failures;
    static void check(boolean yes) { values++; if (!yes) throw new AssertionError("value " + values); }
    static void same(Object a, Object b) { identities++; if (a != b) throw new AssertionError("identity " + identities); }
    static void state(boolean yes) { states++; if (!yes) throw new AssertionError("state " + states); }
    @SuppressWarnings("unchecked") static Object invoke(int route, String name, Class<?> result, Class<?>[] params, Core core, Object... input) throws Throwable {
        if (route <= 2) {
            Api<Object> api = (Api<Object>) (Api) core;
            if (name.equals("add")) return route == 0 ? core.add(input[0]) : api.add(input[0]);
            if (name.equals("bulk")) return route == 0 ? core.bulk((Collection<?>) input[0]) : api.bulk((Collection<?>) input[0]);
            if (name.equals("echo")) return route == 0 ? core.echo(input[0], input[1]) : api.echo(input[0], input[1]);
            if (name.equals("array")) return route == 0 ? core.array((Object[]) input[0]) : api.array((Object[]) input[0]);
            if (name.equals("containsAll")) return route == 0 ? core.containsAll((Collection<?>) input[0]) : api.containsAll((Collection<?>) input[0]);
            return route == 0 ? core.values() : ((Parent<Object>) api).values();
        }
        Class<?> owner = route == 3 ? Core.class : (name.equals("values") ? Parent.class : Api.class);
        try {
            if (route != 5) return owner.getMethod(name, params).invoke(core, input);
            MethodHandle handle = MethodHandles.publicLookup().findVirtual(owner, name, MethodType.methodType(result, params));
            Object[] arguments = new Object[input.length + 1]; arguments[0] = core; System.arraycopy(input, 0, arguments, 1, input.length);
            return handle.invokeWithArguments(arguments);
        } catch (InvocationTargetException error) { throw error.getCause(); }
    }
    public static void main(String[] args) throws Throwable {
        boolean rebuilt = args.length == 1 && args[0].equals("rebuilt");
        for (int route = 0; route < 6; route++) {
            Core core = new Core(); int[] alias = {19}; core.marker = alias;
            RuntimeException dormant = new IllegalArgumentException("owned dormant failure"); core.failure = dormant;
            UnsupportedOperationException prior = null;
            for (String name : new String[] {"add", "bulk"}) for (Object input : (name.equals("add") ? new Object[] {new Box(7), core, "wrong", null} : new Object[] {Arrays.asList(new Box(9)), Arrays.asList("wrong", core), Collections.emptyList(), null})) {
                try { invoke(route, name, boolean.class, new Class<?>[] {name.equals("add") ? Object.class : Collection.class}, core, input); throw new AssertionError("missing immutable failure"); }
                catch (UnsupportedOperationException error) { failures++; check(error.getClass() == UnsupportedOperationException.class); check(error.getMessage().equals("owned immutable operation")); check(error != prior); prior = error; }
                state(core.calls == 0); state(alias[0] == 19); same(core.marker, alias); same(core.failure, dormant);
            }
            try { core.addScalar(Integer.MIN_VALUE); throw new AssertionError("missing scalar failure"); }
            catch (UnsupportedOperationException error) { failures++; check(error.getMessage().equals("owned immutable operation")); check(error != prior); }
            state(core.calls == 0); state(alias[0] == 19);
            core.failure = null;
            for (Object input : new Object[] {new Box(-7), core, alias, "owned", null}) {
                int before = alias[0]; same(invoke(route, "echo", Object.class, new Class<?>[] {Object.class, Object.class}, core, input, alias), input);
                state(core.observed == before); state(alias[0] == before + 3); same(core.marker, alias);
            }
            for (Object[] input : new Object[][] {new Box[] {new Box(1), null}, new String[] {"owned", null}, new Object[] {core, alias}, new Object[0], null}) {
                int before = alias[0]; same(invoke(route, "array", Object[].class, new Class<?>[] {Object[].class}, core, (Object) input), input);
                state(core.observed == before); state(alias[0] == before + 3); same(core.marker, alias);
            }
            for (Collection<?> input : new Collection<?>[] {Collections.emptyList(), Arrays.asList(core, "owned"), null}) {
                int before = alias[0]; check(((Boolean) invoke(route, "containsAll", boolean.class, new Class<?>[] {Collection.class}, core, input)).booleanValue() == (input == null));
                state(core.observed == before); state(alias[0] == before + 3);
            }
            for (Iterator<?> input : new Iterator<?>[] {Arrays.asList(new Box(3), new Box(5)).iterator(), Arrays.asList("owned").iterator(), null}) {
                core.cursor = (Iterator<Object>) input; int before = alias[0]; same(invoke(route, "values", Iterator.class, new Class<?>[0], core), input);
                state(core.observed == before); state(alias[0] == before + 3);
            }
            state(core.calls == 16);
            for (String name : new String[] {"echo", "array", "containsAll", "values"}) {
                int before = alias[0]; int beforeCalls = core.calls; RuntimeException sentinel = new IllegalStateException("owned mutable failure"); core.failure = sentinel;
                Class<?>[] params = name.equals("echo") ? new Class<?>[] {Object.class, Object.class} : name.equals("array") ? new Class<?>[] {Object[].class} : name.equals("containsAll") ? new Class<?>[] {Collection.class} : new Class<?>[0];
                Class<?> result = name.equals("array") ? Object[].class : name.equals("containsAll") ? boolean.class : name.equals("values") ? Iterator.class : Object.class;
                Object[] inputs = name.equals("echo") ? new Object[] {null, alias} : params.length == 0 ? new Object[0] : new Object[] {null};
                try { invoke(route, name, result, params, core, inputs); throw new AssertionError("missing effectful failure"); }
                catch (RuntimeException error) { failures++; same(error, sentinel); }
                state(core.calls == beforeCalls + 1); state(core.observed == before); state(alias[0] == before + 3); same(core.marker, alias);
                try { invoke(route, name, result, params, null, inputs); throw new AssertionError("missing null receiver"); }
                catch (NullPointerException expected) { failures++; }
                state(core.calls == beforeCalls + 1); state(alias[0] == before + 3);
                core.failure = null;
            }
        }
        int measuredValues = values, measuredIdentities = identities, measuredStates = states, measuredFailures = failures;
        Method bridge = Core.class.getDeclaredMethod("add", Object.class); check(bridge.isBridge() == !rebuilt); check(bridge.isSynthetic() == !rebuilt); same(bridge.getGenericParameterTypes()[0], Object.class);
        Type edge = Core.class.getGenericInterfaces()[0]; if (rebuilt) same(edge, Api.class); else { same(((ParameterizedType) edge).getRawType(), Api.class); same(((ParameterizedType) edge).getActualTypeArguments()[0], Box.class); }
        for (String name : new String[] {"bulk", "containsAll"}) {
            Type parameter = Core.class.getDeclaredMethod(name, Collection.class).getGenericParameterTypes()[0];
            if (rebuilt) same(parameter, Collection.class); else { ParameterizedType type = (ParameterizedType) parameter; same(type.getRawType(), Collection.class); WildcardType wildcard = (WildcardType) type.getActualTypeArguments()[0]; same(wildcard.getUpperBounds()[0], name.equals("bulk") ? Box.class : Object.class); }
        }
        for (Class<?> declaration : new Class<?>[] {Api.class, Core.class}) {
            Method echo = declaration.getDeclaredMethod("echo", Object.class, Object.class); Method array = declaration.getDeclaredMethod("array", Object[].class);
            if (rebuilt && declaration == Core.class) { check(echo.getTypeParameters().length == 0); check(array.getTypeParameters().length == 0); same(echo.getGenericReturnType(), Object.class); same(array.getGenericReturnType(), Object[].class); }
            else { check(echo.getTypeParameters().length == 2); check(array.getTypeParameters().length == 1); check(echo.getGenericParameterTypes()[0].equals(echo.getTypeParameters()[0])); check(array.getGenericReturnType() instanceof GenericArrayType); }
        }
        ParameterizedType returned = (ParameterizedType) Core.class.getDeclaredMethod("values").getGenericReturnType(); same(returned.getRawType(), Iterator.class); same(returned.getActualTypeArguments()[0], Box.class);
        System.out.println("values=" + measuredValues + ",identities=" + measuredIdentities + ",states=" + measuredStates + ",failures=" + measuredFailures);
        System.out.println("parameter-independent-throw-and-original-runtime-slots=retained,full-other-contracts-effects-and-return-metadata=checked,raw-interface-and-member-metadata=reported");
    }
}
