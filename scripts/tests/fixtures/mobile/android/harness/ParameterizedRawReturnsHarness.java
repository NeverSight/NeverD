import fixture.*;
import java.lang.reflect.*;
import java.util.concurrent.atomic.AtomicReference;

public final class ParameterizedRawReturnsHarness {
    private static int values, identities, states, failures;
    private static final Method PARENT, TYPED, BRIDGE;
    static {
        try {
            PARENT = Parent.class.getDeclaredMethod("wrap", Object.class);
            TYPED = RawChild.class.getDeclaredMethod("wrap", Object.class);
            Method bridge = null;
            for (Method method : RawChild.class.getDeclaredMethods()) {
                if (method.getName().equals("wrap") && method.getReturnType() == Object.class) bridge = method;
            }
            if (bridge == null) throw new AssertionError("missing owned original bridge");
            BRIDGE = bridge;
            PARENT.setAccessible(true); TYPED.setAccessible(true); BRIDGE.setAccessible(true);
        } catch (ReflectiveOperationException error) { throw new AssertionError(error); }
    }
    private static void require(boolean condition) {
        if (!condition) throw new AssertionError("owned parameterized raw result contract");
    }
    private static void same(Object actual, Object expected) { require(actual == expected); identities++; }
    private static Object invoke(Method method, Object receiver, Object input) throws Throwable {
        try { return method.invoke(receiver, input); }
        catch (InvocationTargetException error) { throw error.getCause(); }
    }
    private static Object call(int path, RawChild receiver, Object input) throws Throwable {
        switch (path) {
            case 0: return receiver.produce(input);
            case 1: return invoke(PARENT, receiver, input);
            case 2: return invoke(TYPED, receiver, input);
            case 3: return invoke(BRIDGE, receiver, input);
            default: throw new AssertionError(path);
        }
    }
    @SuppressWarnings({"rawtypes", "unchecked"})
    public static void main(String[] args) throws Throwable {
        require(!TYPED.isBridge() && !TYPED.isSynthetic() && BRIDGE.isBridge() && BRIDGE.isSynthetic());
        require(Modifier.isProtected(TYPED.getModifiers()) && Modifier.isProtected(BRIDGE.getModifiers()));
        require(TYPED.getExceptionTypes().length == 0 && BRIDGE.getExceptionTypes().length == 0);
        require(PARENT.getGenericReturnType() == Parent.class.getTypeParameters()[0]);
        Type result = TYPED.getGenericReturnType();
        require(result instanceof ParameterizedType && ((ParameterizedType) result).getRawType() == AtomicReference.class);
        Type wildcard = ((ParameterizedType) result).getActualTypeArguments()[0];
        require(wildcard instanceof WildcardType && ((WildcardType) wildcard).getUpperBounds()[0] == Object.class);
        Type parent = RawChild.class.getGenericSuperclass();
        require(parent instanceof ParameterizedType && ((ParameterizedType) parent).getRawType() == Parent.class);
        require(((ParameterizedType) parent).getActualTypeArguments()[0] == AtomicReference.class);
        require(RawChild.class.getMethod("convert", Object.class, Object.class).getDeclaringClass() == Parent.class);
        Object input = new Object(), payload = new Object(), replacement = new Object();
        for (int path = 0; path < 4; path++) {
            for (boolean absent : new boolean[] {false, true}) {
                RawChild receiver = new RawChild();
                AtomicReference value = absent ? null : new AtomicReference(payload);
                receiver.result = value;
                Object argument = absent ? null : input;
                same(call(path, receiver, argument), value); values++;
                same(receiver.seenInput, argument);
                require(receiver.calls == 1 && receiver.events == 4); states++;
                if (value != null) {
                    same(value.get(), payload);
                    value.set(replacement);
                    same(receiver.result.get(), replacement);
                }
            }
            RawChild receiver = new RawChild();
            receiver.failure = new IllegalStateException("owned raw result failure");
            try { call(path, receiver, input); throw new AssertionError("missing original failure"); }
            catch (Throwable error) { same(error, receiver.failure); failures++; }
            same(receiver.seenInput, input);
            require(receiver.calls == 1 && receiver.events == 4); states++;
            try { call(path, null, input); throw new AssertionError("missing null receiver failure"); }
            catch (NullPointerException expected) { failures++; }
            require(receiver.calls == 1 && receiver.events == 4); states++;
        }
        require(values == 8 && identities == 32 && states == 16 && failures == 8);
        System.out.println("raw-values=8,raw-identities=32,raw-states=16,raw-failures=8");
        System.out.println("raw-parent-result=retained,parameterized-child-result=retained,protected-original-bridge=retained");
    }
}
