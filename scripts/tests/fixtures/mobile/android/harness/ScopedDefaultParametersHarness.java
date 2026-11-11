import fixture.*;
import java.lang.reflect.*;

public final class ScopedDefaultParametersHarness {
    private static int values, identities, states, failures;
    private static final String KEY = new String("owned key");
    private static final Number NUMBER = Integer.valueOf(173);
    private interface Call { Object run(Object key, Object value) throws Throwable; }
    private static final class Leaf extends Core<String, Number> {
        public Entry<String, Number> put(String key, Entry<String, Number> value) {
            Trace.calls += 2;
            return super.put(key, value);
        }
    }
    private static Object invoke(Method method, Object receiver, Object key, Object value) throws Throwable {
        try { return method.invoke(receiver, key, value); }
        catch (InvocationTargetException error) { throw error.getCause(); }
    }
    private static void same(Object actual, Object expected) {
        if (actual != expected) throw new AssertionError("original scoped default identity changed");
        identities++;
    }
    private static void reset(RuntimeException failure) {
        Trace.calls = 0; Trace.key = null; Trace.value = null; Trace.failure = failure;
    }
    private static void state(int calls, Object key, Object value, RuntimeException failure) {
        if (Trace.calls != calls) throw new AssertionError("default dispatch or effects changed");
        same(Trace.key, key); same(Trace.value, value); same(Trace.failure, failure);
        states++;
    }
    @SuppressWarnings({"rawtypes", "unchecked"})
    public static void main(String[] args) throws Throwable {
        Method typed = Provider.class.getDeclaredMethod("put", Object.class, Entry.class);
        Method bridge = Provider.class.getDeclaredMethod("put", Object.class, Object.class);
        if (!typed.isDefault() || typed.isBridge() || !bridge.isDefault() || !bridge.isBridge()
                || !bridge.isSynthetic() || typed.getTypeParameters().length != 0
                || bridge.getTypeParameters().length != 0 || typed.getReturnType() != Entry.class
                || bridge.getReturnType() != Object.class || typed.getExceptionTypes().length != 0
                || bridge.getExceptionTypes().length != 0
                || !typed.getGenericParameterTypes()[0].getTypeName().equals("K")
                || !typed.getGenericParameterTypes()[1].getTypeName().equals("fixture.Entry<K, V>")
                || !typed.getGenericReturnType().getTypeName().equals("fixture.Entry<K, V>")
                || !Provider.class.getGenericInterfaces()[0].getTypeName()
                    .equals("fixture.Root<K, fixture.Entry<K, V>>")
                || !Core.class.getGenericInterfaces()[0].getTypeName().equals("fixture.Provider<L, R>")
)
            throw new AssertionError("original scoped provider metadata or inherited ownership changed");
        for (Method method : Core.class.getDeclaredMethods())
            if (method.getName().equals("put"))
                throw new AssertionError("consumer acquired a default or bridge declaration");
        for (Core<String, Number> receiver : new Core[] {new Core<String, Number>(), new Leaf()}) {
            int delta = receiver instanceof Leaf ? 3 : 1;
            Root<String, Entry<String, Number>> view = receiver;
            Root raw = receiver;
            Call[] paths = {(key, value) -> receiver.put((String) key, (Entry<String, Number>) value),
                            (key, value) -> view.put((String) key, (Entry<String, Number>) value),
                            raw::put, (key, value) -> invoke(typed, receiver, key, value),
                            (key, value) -> invoke(bridge, receiver, key, value)};
            for (int route = 0; route < paths.length; route++) {
                Call path = paths[route];
                for (String key : new String[] {KEY, null})
                    for (boolean present : new boolean[] {true, false}) {
                        Entry<String, Number> value = present ? new Entry<String, Number>(KEY, NUMBER) : null;
                        reset(null);
                        Object result = path.run(key, value);
                        same(result, value);
                        state(delta, key, value, null);
                        if (value != null) {
                            same(value.key, KEY); same(value.value, NUMBER);
                            value.value = Long.valueOf(211);
                            same(((Entry<?, ?>) result).value, value.value);
                        }
                        values++;
                    }
                Entry<String, Number> value = new Entry<String, Number>(KEY, NUMBER);
                RuntimeException problem = new IllegalStateException("owned default failure");
                reset(problem);
                try { path.run(KEY, value); throw new AssertionError("default failure missing"); }
                catch (Throwable error) { same(error, problem); failures++; }
                state(delta, KEY, value, problem);

                reset(null);
                try { path.run(KEY, new Object()); throw new AssertionError("checked value failure missing"); }
                catch (Throwable error) {
                    same(error.getClass(), route == 3 ? IllegalArgumentException.class : ClassCastException.class);
                    failures++;
                }
                state(0, null, null, null);
                if (route >= 2) {
                    Object wrongKey = new Object();
                    reset(null);
                    if (receiver instanceof Leaf) {
                        try { path.run(wrongKey, value); throw new AssertionError("independent key bridge failure missing"); }
                        catch (Throwable error) { same(error.getClass(), ClassCastException.class); failures++; }
                        state(0, null, null, null);
                    } else {
                        same(path.run(wrongKey, value), value);
                        state(delta, wrongKey, value, null);
                        values++;
                    }
                }
            }
            Call[] absent = {(key, value) -> ((Provider) null).put(key, (Entry) value),
                             (key, value) -> ((Root) null).put(key, value),
                             (key, value) -> ((Core) null).put(key, (Entry) value),
                             (key, value) -> invoke(typed, null, key, value),
                             (key, value) -> invoke(bridge, null, key, value)};
            for (Call path : absent) {
                reset(null);
                try { path.run(null, null); throw new AssertionError("null receiver failure missing"); }
                catch (Throwable error) { same(error.getClass(), NullPointerException.class); failures++; }
                state(0, null, null, null);
            }
        }
        System.out.println("values=" + values + ",identities=" + identities + ",states=" + states + ",failures=" + failures);
        System.out.println("scoped-parent-arguments=retained,original-default-owner-and-bridge=retained,consumer-put-declarations=absent");
    }
}
