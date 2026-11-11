import fixture.*;
import java.lang.reflect.*;
import java.util.*;

public final class ParameterizedFixedThrowsHarness {
    private static int values, identities, states, failures;
    private static void require(boolean condition) {
        if (!condition) throw new AssertionError("owned parameterized fixed throws contract");
    }
    private static void same(Object actual, Object expected) { require(actual == expected); identities++; }
    private static Object get(Object receiver, String name) throws Exception {
        return receiver.getClass().getField(name).get(receiver);
    }
    private static void set(Object receiver, String name, Object value) throws Exception {
        receiver.getClass().getField(name).set(receiver, value);
    }
    private static Object codec(int profile) {
        return profile == 0 ? new CollectionCodec() : new EmptyCollectionCodec();
    }
    private static Object invoke(Method method, Object receiver, Object input, Object fallback) throws Throwable {
        try { return method.invoke(receiver, input, fallback); }
        catch (InvocationTargetException error) { throw error.getCause(); }
    }
    @SuppressWarnings({"rawtypes", "unchecked"})
    private static Object call(int profile, int path, Object receiver, Object input, Object fallback) throws Throwable {
        switch (path) {
            case 0: return profile == 0 ? ((CollectionCodec) receiver).convert(input, (Collection<?>) fallback)
                                       : ((EmptyCollectionCodec) receiver).convert(input, (Collection<?>) fallback);
            case 1: return ((Api<Collection<?>>) receiver).convert(input, (Collection<?>) fallback);
            case 2: return ((Api) receiver).convert(input, fallback);
            case 3: return invoke((profile == 0 ? CollectionCodec.class : EmptyCollectionCodec.class)
                                 .getDeclaredMethod("convert", Object.class, Collection.class), receiver, input, fallback);
            case 4: return invoke((profile == 0 ? CollectionCodec.class : EmptyCollectionCodec.class)
                                 .getDeclaredMethod("convert", Object.class, Object.class), receiver, input, fallback);
            default: throw new AssertionError(path);
        }
    }
    private static void state(Object receiver, int calls, Object input, Object fallback) throws Exception {
        require(((Integer) get(receiver, "calls")) == calls && ((Integer) get(receiver, "events")) == (calls == 0 ? 0 : 6));
        same(get(receiver, "seenInput"), input);
        same(get(receiver, "seenFallback"), fallback);
        states++;
    }
    private static void failures(int profile, int path, Object receiver, Object input, Object fallback,
                                 Throwable expected, Class<?> kind) throws Throwable {
        try { call(profile, path, receiver, input, fallback); throw new AssertionError("missing original failure"); }
        catch (Throwable actual) {
            require(kind.isInstance(actual));
            if (expected != null) same(actual, expected);
            failures++;
        }
    }
    @SuppressWarnings({"rawtypes", "unchecked"})
    public static void main(String[] args) throws Throwable {
        Class<?>[] exceptions = {java.io.IOException.class, ReflectiveOperationException.class};
        require(Arrays.equals(Api.class.getDeclaredMethod("convert", Object.class, Object.class).getExceptionTypes(), exceptions));
        for (int profile = 0; profile < 2; profile++) {
            Class<?> owner = profile == 0 ? CollectionCodec.class : EmptyCollectionCodec.class;
            Method typed = owner.getDeclaredMethod("convert", Object.class, Collection.class);
            Method bridge = owner.getDeclaredMethod("convert", Object.class, Object.class);
            require(!typed.isBridge() && !typed.isSynthetic() && bridge.isBridge() && bridge.isSynthetic());
            require(Arrays.equals(typed.getExceptionTypes(), profile == 0 ? exceptions : new Class<?>[0]));
            require(Arrays.equals(typed.getGenericExceptionTypes(), typed.getExceptionTypes()));
            require(Arrays.equals(bridge.getExceptionTypes(), exceptions));
            Type parameter = typed.getGenericParameterTypes()[1], result = typed.getGenericReturnType();
            require(parameter.equals(result) && result instanceof ParameterizedType);
            require(((ParameterizedType) result).getRawType() == Collection.class);
            require(((ParameterizedType) result).getActualTypeArguments()[0] instanceof WildcardType);
            Type implemented = owner.getGenericInterfaces()[0];
            require(implemented instanceof ParameterizedType && ((ParameterizedType) implemented).getRawType() == Api.class);
            require(((ParameterizedType) implemented).getActualTypeArguments()[0].equals(result));
            for (int path = 0; path < 5; path++) {
                for (int round = 0; round < 3; round++) {
                    Object receiver = codec(profile), input = round == 2 ? null : new Object();
                    Collection<?> fallback = round == 0 ? new ArrayList<Object>() : null;
                    Collection<?> resultValue = round == 1 ? new ArrayList<Object>() : null;
                    set(receiver, "result", resultValue);
                    Object expected = fallback == null ? resultValue : fallback;
                    Object actual = call(profile, path, receiver, input, fallback);
                    same(actual, expected); values++;
                    state(receiver, 1, input, fallback);
                    if (actual != null) {
                        Object marker = new Object();
                        ((Collection) actual).add(marker);
                        Collection<?> alias = (Collection<?>) get(receiver, fallback == null ? "result" : "seenFallback");
                        same(alias.iterator().next(), marker);
                    }
                }
                Object input = new Object();
                Collection<?> fallback = new ArrayList<Object>();
                for (boolean fatal : new boolean[] {false, true}) {
                    Object receiver = codec(profile);
                    Throwable error = profile == 0
                        ? (fatal ? new ReflectiveOperationException("owned checked failure") : new java.io.IOException("owned checked failure"))
                        : (fatal ? new AssertionError("owned unchecked failure") : new IllegalStateException("owned unchecked failure"));
                    set(receiver, fatal ? "fatal" : "failure", error);
                    failures(profile, path, receiver, input, fallback, error, error.getClass());
                    state(receiver, 1, input, fallback);
                }
                Object untouched = codec(profile);
                failures(profile, path, null, input, fallback, null, NullPointerException.class);
                state(untouched, 0, null, null);
            }
            for (int path : new int[] {2, 4}) {
                Object receiver = codec(profile);
                failures(profile, path, receiver, new Object(), new Object(), null, ClassCastException.class);
                state(receiver, 0, null, null);
            }
        }
        require(values == 30 && identities == 198 && states == 64 && failures == 34);
        System.out.println("throws-values=30,throws-identities=198,throws-states=64,throws-failures=34");
        System.out.println("complete-parameterized-types=retained,fixed-and-empty-target-exceptions=retained,ordered-original-bridge-exceptions=retained");
    }
}
