import fixture.*;
import java.lang.reflect.*;
import java.util.*;
public final class AnchoredClosedHeadHarness {
    static long trace;
    static int failAt;
    static Object item;
    static RuntimeException failure;
    static void check(boolean value) { if (!value) throw new AssertionError("owned anchored closed head"); }
    static void hit(int n) { trace = trace * 10 + n; if (failAt == n) throw failure; }
    static final class FutureContext extends Context {
        final int n;
        FutureContext(int n) { this.n = n; }
        public void touch() { hit(n); }
    }
    static final class Read implements Producer {
        int calls;
        public Object next() { calls++; hit(3); return item; }
    }
    static final class FutureMap extends AbstractMap<Object, Object> {
        final Object key, value;
        int reads, writes;
        FutureMap(Object key, Object value) { this.key = key; this.value = value; }
        public Set<Entry<Object, Object>> entrySet() { throw new AssertionError("opaque map traversal"); }
        public Object get(Object key) { reads++; check(key == this.key); return value; }
        public Object put(Object key, Object value) { writes++; check(key == this.key && value == item); return null; }
    }
    public static void main(String[] args) throws Exception {
        int route = Integer.parseInt(args[0]), mode = Integer.parseInt(args[1]);
        Method method = Factory.class.getMethod("create", Context.class, Class.class, String.class, Context.class, Producer.class);
        TypeVariable<?> t = method.getTypeParameters()[0];
        ParameterizedType output = (ParameterizedType) method.getGenericReturnType();
        check(method.getTypeParameters().length == 1 && t.getBounds()[0] == Object.class
                && output.getRawType() == Map.class && output.getActualTypeArguments()[0] == Object.class
                && output.getActualTypeArguments()[1].equals(t));
        check(((ParameterizedType) method.getGenericParameterTypes()[1]).getActualTypeArguments()[0].equals(t));
        for (int attempt = 0; attempt < 2; attempt++) {
            trace = 0; failAt = mode == 8 ? 1 : mode == 9 ? 2 : mode == 5 || mode == 12 ? 3 : 0;
            failure = new RuntimeException("owned callback failure");
            RuntimeException providerError = new RuntimeException("owned provider failure");
            Context first = mode == 2 || mode == 10 || mode == 13 ? null : new FutureContext(1);
            Context last = mode == 3 || mode == 10 || mode == 11 || mode == 13 ? null : new FutureContext(2);
            Class<String> type = mode == 1 || mode == 10 || mode == 11 ? null : String.class;
            Read read = new Read(); Producer producer = mode == 4 || mode == 10 ? null : read;
            item = mode == 14 ? new Long(Long.MIN_VALUE + attempt) : mode == 15 ? new Long(Long.MAX_VALUE - attempt) : new Object();
            Object key = new Object(), value = attempt == 0 ? new String("owned") : new Object();
            FutureMap map = new FutureMap(key, value);
            Map cached = mode == 7 ? null : map;
            String label = new String("label"); Object sentinel = new Object();
            Factory.events = 0; Factory.savedContext = Factory.savedTail = null;
            Factory.savedType = null; Factory.name = Factory.savedLabel = null;
            Factory.pending = null; Factory.produced = null; Factory.saved = sentinel;
            Factory.replacement = cached;
            Factory.failure = mode == 6 || mode == 12 || mode == 13 ? providerError : null;
            int stage = first == null ? 0 : failAt == 1 || type == null || last == null ? 1
                    : failAt == 2 || producer == null ? 2 : 3;
            boolean success = first != null && type != null && last != null && producer != null && failAt == 0 && Factory.failure == null;
            Map result = null; Throwable caught = null;
            try {
                if (route == 0) result = Core.call(first, type, label, last, producer);
                else if (route == 1) Core.discard(first, type, label, last, producer);
                else if (route == 2) result = Core.<String>typed(first, type, label, last, producer);
                else result = Factory.create(first, type, label, last, producer);
            } catch (Throwable error) { caught = error; }
            if (success) check(caught == null);
            else if (first == null || type == null && failAt != 1 || last == null && failAt != 1 || producer == null && failAt != 1 && failAt != 2) check(caught instanceof NullPointerException);
            else check(caught == (failAt != 0 ? failure : providerError));
            long expectedTrace = stage == 0 ? 0 : stage == 1 ? 1 : stage == 2 ? 12 : 123;
            int events = route == 3 ? success ? 7 : 1 : success ? 414 : 16;
            check(trace == expectedTrace && Factory.events == events);
            check(Factory.savedContext == first && Factory.savedType == type);
            boolean named = first != null && failAt != 1 && type != null;
            check(named ? String.class.getName().equals(Factory.name) : Factory.name == null);
            check(Factory.savedTail == (named ? last : null));
            boolean pending = named && last != null && failAt != 2;
            check(Factory.pending == (pending ? producer : null) && Factory.savedLabel == (pending ? label : null));
            check(read.calls == (stage == 3 ? 1 : 0));
            check(Factory.produced == (stage == 3 && failAt != 3 ? item : null));
            check(Factory.saved == (success ? cached : sentinel) && result == (success && route != 1 ? cached : null));
            if (success) {
                Object next = new Object(); item = next;
                check(Factory.pending.next() == next && read.calls == 2);
                if (route != 1) {
                    try {
                        String checked = ((Map<Object, String>) result).get(key);
                        check(cached != null && attempt == 0 && checked == value);
                    } catch (NullPointerException error) { check(cached == null); }
                    catch (ClassCastException error) { check(cached != null && attempt == 1); }
                    if (cached != null) { result.put(key, item); check(map.reads == 1 && map.writes == 1); }
                }
            }
            check(Factory.events == events);
            System.out.println("route=" + route + ",mode=" + mode + ",attempt=" + attempt + ",events=" + events);
        }
    }
}
