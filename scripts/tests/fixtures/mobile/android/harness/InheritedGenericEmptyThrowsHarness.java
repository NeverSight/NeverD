import fixture.*;
import java.lang.reflect.*;
import java.util.Arrays;

public final class InheritedGenericEmptyThrowsHarness {
    private static int values, identities, states, failures;
    private static void require(boolean condition) {
        if (!condition) throw new AssertionError("owned inherited empty throws contract");
    }
    private static void same(Object actual, Object expected) {
        require(actual == expected);
        identities++;
    }
    private static Object invoke(Method method, Object receiver, Object input, Object fallback) throws Throwable {
        try { return method.invoke(receiver, input, fallback); }
        catch (InvocationTargetException error) { throw error.getCause(); }
    }
    @SuppressWarnings({"rawtypes", "unchecked"})
    private static Object call(int path, Child<Object> child, Object input, Object fallback) throws Throwable {
        switch (path) {
            case 0: return child.convert(input, fallback);
            case 1: return ((Parent<Object>) child).convert(input, fallback);
            case 2: return ((Api<Object>) child).convert(input, fallback);
            case 3: return ((Api) child).convert(input, fallback);
            case 4: return invoke(Parent.class.getDeclaredMethod("convert", Object.class, Object.class), child, input, fallback);
            case 5: return invoke(Api.class.getDeclaredMethod("convert", Object.class, Object.class), child, input, fallback);
            case 6: return invoke(Child.class.getMethod("convert", Object.class, Object.class), child, input, fallback);
            default: throw new AssertionError(path);
        }
    }
    private static void execute(int path, Child<Object> child, Object input, Object fallback,
                                Object expected, Throwable failure, boolean nullReceiver,
                                int selects, int events) throws Throwable {
        try {
            Object result = call(path, nullReceiver ? null : child, input, fallback);
            require(failure == null && !nullReceiver);
            same(result, expected);
            values++;
        } catch (Throwable actual) {
            if (nullReceiver) require(actual instanceof NullPointerException);
            else same(actual, failure);
            failures++;
        }
        require(child.calls == (nullReceiver ? 0 : 1) && child.selects == selects && child.events == events);
        if (!nullReceiver) {
            same(child.seenInput, input);
            same(child.seenFallback, selects == 0 ? null : fallback);
        } else require(child.seenInput == null && child.seenFallback == null);
        states++;
    }
    private static void edge(Class<?> owner, Class<?> parent, Type argument) {
        Type inherited = owner.getGenericSuperclass();
        require(inherited instanceof ParameterizedType);
        require(((ParameterizedType) inherited).getRawType() == parent);
        require(((ParameterizedType) inherited).getActualTypeArguments()[0] == argument);
    }
    public static void main(String[] args) throws Throwable {
        Method api = Api.class.getDeclaredMethod("convert", Object.class, Object.class);
        Method parent = Parent.class.getDeclaredMethod("convert", Object.class, Object.class);
        require(Arrays.equals(api.getExceptionTypes(), new Class<?>[] {java.io.IOException.class, ReflectiveOperationException.class}));
        require(Arrays.equals(api.getGenericExceptionTypes(), api.getExceptionTypes()));
        require(parent.getExceptionTypes().length == 0 && parent.getGenericExceptionTypes().length == 0);
        require(!parent.isBridge() && !parent.isSynthetic() && parent.getTypeParameters().length == 0);
        require(parent.getGenericParameterTypes()[0] == Object.class);
        require(parent.getGenericParameterTypes()[1] == Parent.class.getTypeParameters()[0]);
        require(parent.getGenericReturnType() == Parent.class.getTypeParameters()[0]);
        require(Modifier.isAbstract(Parent.class.getModifiers()));
        for (Class<?> owner : new Class<?>[] {Middle.class, Child.class, Fixed.class}) {
            require(owner.getMethod("convert", Object.class, Object.class).getDeclaringClass() == Parent.class);
            for (Method method : owner.getDeclaredMethods()) require(!method.getName().equals("convert"));
        }
        edge(Middle.class, Parent.class, Middle.class.getTypeParameters()[0]);
        edge(Child.class, Middle.class, Child.class.getTypeParameters()[0]);
        edge(Fixed.class, Middle.class, Object.class);
        Type apiEdge = Parent.class.getGenericInterfaces()[0];
        require(apiEdge instanceof ParameterizedType && ((ParameterizedType) apiEdge).getRawType() == Api.class);
        require(((ParameterizedType) apiEdge).getActualTypeArguments()[0] == Parent.class.getTypeParameters()[0]);
        Object first = new Object(), second = new Object(), input = new Object();
        for (int path = 0; path < 7; path++) {
            for (int round = 0; round < 4; round++) {
                Child<Object> child = new Child<Object>();
                Object fallback = round < 2 ? (round == 0 ? first : second) : null;
                child.result = round == 3 ? null : first;
                execute(path, child, round == 1 ? null : input, fallback,
                        fallback == null ? child.result : fallback, null, false, 1, 12);
            }
            Child<Object> before = new Child<Object>();
            before.failure = new IllegalStateException("owned parent failure");
            execute(path, before, input, first, null, before.failure, false, 0, 1);
            Child<Object> after = new Child<Object>();
            after.selectFailure = new AssertionError("owned callback failure");
            execute(path, after, input, second, null, after.selectFailure, false, 1, 12);
            execute(path, new Child<Object>(), input, first, null, null, true, 0, 0);
        }
        Fixed fixed = new Fixed();
        fixed.result = second;
        same(fixed.convert(input, first), first); values++;
        require(fixed.calls == 1 && fixed.events == 13); states++;
        fixed.events = 0;
        same(((Api<Object>) fixed).convert(null, null), second); values++;
        require(fixed.calls == 2 && fixed.events == 13 && fixed.seenInput == null); states++;
        require(values == 30 && identities == 128 && states == 51 && failures == 21);
        System.out.println("values=30,identities=128,states=51,failures=21");
        System.out.println("fixed-interface-exceptions=retained,empty-generic-provider-exceptions=retained,original-provider-owner=retained");
    }
}
