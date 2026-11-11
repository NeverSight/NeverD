import fixture.*;
import java.lang.reflect.*;
import java.util.*;

public final class ScopedWildcardDefaultsHarness {
    private static int values, identities, states, failures;
    private static final String KEY = new String("owned key");
    private static final Number NUMBER = Integer.valueOf(173);
    private interface Call { void run(Map entries) throws Throwable; }
    private static final class Leaf extends Core<String, Number> {
        public void putAll(Map<? extends String, ? extends Entry<String, Number>> entries) {
            Trace.calls += 2;
            super.putAll(entries);
        }
    }
    private static void invoke(Method method, Object receiver, Map entries) throws Throwable {
        try { method.invoke(receiver, entries); }
        catch (InvocationTargetException error) { throw error.getCause(); }
    }
    private static void same(Object actual, Object expected) {
        if (actual != expected) throw new AssertionError("original wildcard default identity changed");
        identities++;
    }
    private static void reset(RuntimeException failure) {
        Trace.calls = 0; Trace.visits = 0; Trace.source = null;
        Trace.key = null; Trace.value = null; Trace.failure = failure;
    }
    private static void state(int calls, int visits, Object source, Object key,
                              Object value, RuntimeException failure) {
        if (Trace.calls != calls || Trace.visits != visits)
            throw new AssertionError("wildcard default dispatch or ordered effects changed");
        same(Trace.source, source); same(Trace.key, key);
        same(Trace.value, value); same(Trace.failure, failure);
        states++;
    }
    @SuppressWarnings({"rawtypes", "unchecked"})
    public static void main(String[] args) throws Throwable {
        Method method = Provider.class.getDeclaredMethod("putAll", Map.class);
        if (!method.isDefault() || method.isBridge() || method.isSynthetic()
                || method.getTypeParameters().length != 0 || method.getReturnType() != void.class
                || method.getExceptionTypes().length != 0
                || !method.getGenericParameterTypes()[0].getTypeName()
                    .equals("java.util.Map<? extends K, ? extends fixture.Entry<K, V>>")
                || !Provider.class.getGenericInterfaces()[0].getTypeName()
                    .equals("fixture.Root<K, fixture.Entry<K, V>>")
                || !Core.class.getGenericInterfaces()[0].getTypeName().equals("fixture.Provider<L, R>"))
            throw new AssertionError("complete original wildcard default scope changed");
        for (Method declared : Core.class.getDeclaredMethods())
            if (declared.getName().equals("putAll"))
                throw new AssertionError("consumer acquired a default declaration");
        for (Core<String, Number> receiver : new Core[] {new Core<String, Number>(), new Leaf()}) {
            int delta = receiver instanceof Leaf ? 3 : 1;
            Root<String, Entry<String, Number>> view = receiver;
            Root raw = receiver;
            Call[] paths = {entries -> receiver.putAll(entries), entries -> view.putAll(entries),
                            raw::putAll, entries -> invoke(method, receiver, entries)};
            for (int route = 0; route < paths.length; route++) {
                Call path = paths[route];
                for (int mode = 0; mode < 3; mode++) {
                    Entry<String, Number> entry = new Entry<String, Number>(KEY, NUMBER);
                    Map entries = mode == 2 ? null : new LinkedHashMap();
                    if (mode == 0) entries.put(KEY, entry);
                    reset(null); path.run(entries);
                    state(delta, mode == 0 ? 1 : 0, entries, mode == 0 ? KEY : null,
                          mode == 0 ? entry : null, null);
                    if (mode == 0) {
                        same(entry.value, NUMBER); same(entries.get(KEY), entry);
                        entry.value = Long.valueOf(211);
                        same(((Entry) Trace.value).value, entry.value);
                    }
                    values++;
                }
                Map entries = new LinkedHashMap();
                Entry<String, Number> entry = new Entry<String, Number>(KEY, NUMBER);
                entries.put(KEY, entry);
                RuntimeException problem = new IllegalStateException("owned wildcard default failure");
                reset(problem);
                try { path.run(entries); throw new AssertionError("default failure missing"); }
                catch (Throwable error) { same(error, problem); failures++; }
                state(delta, 0, entries, null, null, problem);

                Map wrong = new LinkedHashMap(); wrong.put(KEY, new Object());
                reset(null);
                try { path.run(wrong); throw new AssertionError("late checked value failure missing"); }
                catch (Throwable error) { same(error.getClass(), ClassCastException.class); failures++; }
                state(delta, 0, wrong, KEY, null, null);
                if (route >= 2) {
                    Object wrongKey = new Object();
                    Map erased = new LinkedHashMap(); erased.put(wrongKey, entry);
                    reset(null); path.run(erased);
                    state(delta, 1, erased, wrongKey, entry, null);
                    values++;
                }
            }
            Call[] absent = {entries -> ((Provider) null).putAll(entries),
                             entries -> ((Root) null).putAll(entries),
                             entries -> ((Core) null).putAll(entries),
                             entries -> invoke(method, null, entries)};
            for (Call path : absent) {
                reset(null);
                try { path.run(null); throw new AssertionError("null receiver failure missing"); }
                catch (Throwable error) { same(error.getClass(), NullPointerException.class); failures++; }
                state(0, 0, null, null, null, null);
            }
        }
        System.out.println("values=" + values + ",identities=" + identities + ",states=" + states + ",failures=" + failures);
        System.out.println("nested-wildcard-arguments=retained,original-default-owner=retained,consumer-putAll-declarations=absent");
    }
}
