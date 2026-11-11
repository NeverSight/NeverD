import fixture.*;
import java.lang.reflect.*;
import java.util.*;
public final class ClosedReifiableConstructorsHarness {
    static long trace;
    static int mode;
    static RuntimeException getterError, sizeError, providerError;
    static void check(boolean value) { if (!value) throw new AssertionError("owned closed reifiable constructors"); }
    static final class FutureMap extends AbstractMap<Source.Key<?>, Object> {
        int calls, reads, count;
        Source.Key<?> key;
        Object value;
        public Set<Entry<Source.Key<?>, Object>> entrySet() { throw new AssertionError("opaque map traversal"); }
        public int size() {
            calls++; trace = trace * 10 + 2;
            Core core = Core.escaping;
            check(core != null && core.map == this && core.original == this && core.size == 0 && !core.done);
            if (mode == 4) throw sizeError;
            return count;
        }
        public Object get(Object key) {
            reads++; check(key == this.key && Core.escaping.done && Core.escaping.size == count); return value;
        }
    }
    static final class FutureSource extends Source {
        int calls;
        Map<Key<?>, Object> reply;
        public Map<Key<?>, Object> data() {
            calls++; trace = trace * 10 + 1; check(Core.escaping == null);
            if (mode == 3) throw getterError;
            if (mode == 12) Core.failure = providerError;
            return reply;
        }
    }
    public static void main(String[] args) throws Exception {
        int route = Integer.parseInt(args[0]); mode = Integer.parseInt(args[1]);
        Constructor<Core> selected = Core.class.getConstructor(Map.class);
        ParameterizedType parameter = (ParameterizedType) selected.getGenericParameterTypes()[0];
        ParameterizedType keyType = (ParameterizedType) parameter.getActualTypeArguments()[0];
        check(parameter.getRawType() == Map.class && keyType.getRawType() == Source.Key.class
                && ((WildcardType) keyType.getActualTypeArguments()[0]).getUpperBounds()[0] == Object.class
                && ((WildcardType) parameter.getActualTypeArguments()[1]).getUpperBounds()[0] == Object.class);
        boolean sourceRoute = route < 2;
        for (int attempt = 0; attempt < 2; attempt++) {
            trace = 0; Core.escaping = null;
            getterError = new RuntimeException("owned getter failure"); sizeError = new RuntimeException("owned size failure"); providerError = new RuntimeException("owned provider failure");
            Core.failure = mode == 5 ? providerError : null;
            FutureMap map = new FutureMap(), alternate = new FutureMap();
            map.count = mode == 8 ? 0 : mode == 9 ? Integer.MIN_VALUE : mode == 10 ? Integer.MAX_VALUE : 31 + attempt;
            alternate.count = map.count + 1;
            map.key = mode == 11 ? null : new Source.Key<Object>(); alternate.key = map.key;
            map.value = mode == 6 ? null : mode == 7 ? new Object() : mode == 14 ? new Long(Long.MIN_VALUE + attempt)
                    : mode == 15 ? new Long(Long.MAX_VALUE - attempt) : attempt == 0 ? new String("owned") : new Object();
            alternate.value = map.value;
            Map<Source.Key<?>, Object> supplied = mode == 2 ? null : map;
            FutureSource source = new FutureSource(); source.reply = mode == 13 ? alternate : supplied;
            Source suppliedSource = mode == 1 ? null : source;
            Map<Source.Key<?>, Object> actual = sourceRoute ? source.reply : supplied;
            boolean entered = !sourceRoute || mode != 1 && mode != 3;
            boolean success = entered && actual != null && mode != 4 && mode != 5 && !(sourceRoute && mode == 12);
            Core result = null; Throwable caught = null;
            try {
                if (route == 0) result = Core.class.getConstructor(Source.class).newInstance(suppliedSource);
                else if (route == 1) result = new Core(suppliedSource);
                else if (route == 2) result = selected.newInstance(supplied);
                else result = new Core(supplied);
            } catch (InvocationTargetException error) { caught = error.getCause(); }
            catch (Throwable error) { caught = error; }
            if (success) check(caught == null && result == Core.escaping);
            else if (sourceRoute && mode == 1 || entered && actual == null) check(caught instanceof NullPointerException);
            else check(caught == (sourceRoute && mode == 3 ? getterError : mode == 4 ? sizeError : providerError));
            int getters = sourceRoute && mode != 1 ? 1 : 0, sizes = entered && actual != null ? 1 : 0;
            long expectedTrace = getters == 0 ? sizes == 0 ? 0 : 2 : sizes == 0 ? 1 : 12;
            check(trace == expectedTrace && source.calls == getters);
            check(map.calls + alternate.calls == sizes);
            check(map.calls == (sizes != 0 && actual == map ? 1 : 0) && alternate.calls == (sizes != 0 && actual == alternate ? 1 : 0));
            Core escaped = Core.escaping;
            if (!entered) check(escaped == null);
            else {
                check(escaped != null && escaped.map == actual && escaped.original == actual && escaped.done == success);
                int expectedSize = actual != null && mode != 4 ? ((FutureMap) actual).count : 0;
                check(escaped.size == expectedSize);
            }
            if (success) {
                FutureMap used = (FutureMap) actual;
                try {
                    String checked = (String) result.map.get(used.key);
                    check(mode == 6 ? checked == null : mode != 7 && mode != 14 && mode != 15 && attempt == 0 && checked == used.value);
                } catch (ClassCastException error) { check(mode == 7 || mode == 14 || mode == 15 || attempt == 1 && mode != 6); }
                Object future = new Object(); used.value = future;
                check(result.map.get(used.key) == future && used.reads == 2);
            }
            System.out.println("route=" + route + ",mode=" + mode + ",attempt=" + attempt + ",trace=" + expectedTrace);
        }
    }
}
