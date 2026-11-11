import fixture.*;
import java.lang.reflect.*;

public final class FixedThrowsEmptyOverridesHarness {
    private static int values, identities, states, failures;

    private interface Call { Object run() throws Throwable; }

    private static Object reflect(Method method, Object target, Object receiver,
                                  Method invoked, Object[] arguments) throws Throwable {
        try { return method.invoke(target, receiver, invoked, arguments); }
        catch (InvocationTargetException error) { throw error.getCause(); }
    }

    private static Call[] paths(Host.Child child, Object receiver, Method method, Object[] arguments,
                                Method parentMethod, Method interfaceMethod) {
        Host.Parent<String> parent = child;
        Handler handler = child;
        return new Call[] {
            () -> child.invoke(receiver, method, arguments),
            () -> parent.invoke(receiver, method, arguments),
            () -> handler.invoke(receiver, method, arguments),
            () -> reflect(parentMethod, child, receiver, method, arguments),
            () -> reflect(interfaceMethod, child, receiver, method, arguments)
        };
    }

    private static void same(Object actual, Object expected) {
        if (actual != expected) throw new AssertionError("original handler identity changed");
        identities++;
    }

    private static void execute(Call call, Host.Child child, Object receiver, Method method,
                                Object[] arguments, String value, int parentCalls, int childCalls,
                                int events, Object expected, Class<? extends Throwable> errorKind,
                                RuntimeException sentinel) {
        int oldInvokes = child.invokes;
        int oldTests = child.tests;
        Object oldReceiver = child.seenReceiver;
        Method oldMethod = child.seenMethod;
        Object[] oldArguments = child.seenArguments;
        Object oldTarget = child.seenTarget;
        String oldValue = child.seenValue;
        child.events = 0;
        try {
            Object result = call.run();
            if (errorKind != null) throw new AssertionError("handler failure missing");
            same(result, expected);
            values++;
        } catch (Throwable error) {
            if (errorKind == null || error.getClass() != errorKind
                    || (sentinel != null && error != sentinel))
                throw new AssertionError("handler exception type or identity changed", error);
            if (sentinel != null) identities++;
            failures++;
        }
        if (child.invokes != oldInvokes + parentCalls || child.tests != oldTests + childCalls
                || child.events != events)
            throw new AssertionError("parent, bridge or target effect order changed");
        same(child.seenReceiver, parentCalls == 0 ? oldReceiver : receiver);
        same(child.seenMethod, parentCalls == 0 ? oldMethod : method);
        same(child.seenArguments, parentCalls == 0 ? oldArguments : arguments);
        same(child.seenTarget, childCalls == 0 ? oldTarget : receiver);
        same(child.seenValue, childCalls == 0 ? oldValue : value);
        states++;
    }

    private static void exceptions(Executable method, Class<?>... expected) {
        if (!java.util.Arrays.equals(method.getExceptionTypes(), expected)
                || !java.util.Arrays.equals(method.getGenericExceptionTypes(), expected))
            throw new AssertionError("original empty or fixed Exceptions metadata changed");
    }

    @SuppressWarnings({"rawtypes", "unchecked"})
    public static void main(String[] args) throws Exception {
        Class<?>[] parameters = {Object.class, Method.class, Object[].class};
        Method parentMethod = Host.Parent.class.getDeclaredMethod("invoke", parameters);
        Method interfaceMethod = Handler.class.getDeclaredMethod("invoke", parameters);
        exceptions(interfaceMethod, Throwable.class);
        exceptions(parentMethod);
        exceptions(Host.Parent.class.getDeclaredMethod("test", Object.class, Object.class));
        Method typed = Host.Child.class.getDeclaredMethod("test", Object.class, String.class);
        Method bridge = Host.Child.class.getDeclaredMethod("test", Object.class, Object.class);
        exceptions(typed);
        exceptions(bridge);
        for (Class<?> owner : new Class<?>[] {Host.class, Host.Parent.class, Host.Child.class}) {
            for (Constructor<?> constructor : owner.getDeclaredConstructors()) exceptions(constructor);
        }
        if (typed.isBridge() || typed.isSynthetic() || !bridge.isBridge() || !bridge.isSynthetic()
                || Host.Child.class.getMethod("invoke", parameters).getDeclaringClass() != Host.Parent.class
                || !Modifier.isStatic(Host.Parent.class.getModifiers())
                || !Modifier.isAbstract(Host.Parent.class.getModifiers())
                || !Modifier.isStatic(Host.Child.class.getModifiers()))
            throw new AssertionError("original parent ownership or compiler bridge changed");
        Type parent = Host.Child.class.getGenericSuperclass();
        if (!(parent instanceof ParameterizedType)
                || ((ParameterizedType) parent).getRawType() != Host.Parent.class
                || ((ParameterizedType) parent).getActualTypeArguments()[0] != String.class
                || Host.Parent.class.getGenericInterfaces()[0] != Handler.class)
            throw new AssertionError("complete original generic parent or plain interface changed");

        Host.Child child = new Host.Child();
        child.fallback = new Object();
        String a = new String("a");
        String b = new String("b");
        Object[] receivers = {a, b, null, a};
        String[] compared = {a, a, null, b};
        Method method = Object.class.getDeclaredMethod("toString");
        for (int round = 0; round < receivers.length; round++) {
            child.value = compared[round];
            Object[] arguments = {new Object()};
            Object result = receivers[round] == compared[round] ? arguments[0] : child.fallback;
            for (Call call : paths(child, receivers[round], method, arguments, parentMethod, interfaceMethod))
                execute(call, child, receivers[round], method, arguments, compared[round],
                        1, 1, 73, result, null, null);
        }
        RuntimeException problem = new IllegalStateException("owned handler failure");
        Object[] arguments = {new Object()};
        child.value = a;
        child.failure = problem;
        for (Call call : paths(child, a, method, arguments, parentMethod, interfaceMethod))
            execute(call, child, a, method, arguments, a, 1, 0, 7, null, IllegalStateException.class, problem);
        child.failure = null;
        child.testFailure = problem;
        for (Call call : paths(child, a, method, arguments, parentMethod, interfaceMethod))
            execute(call, child, a, method, arguments, a, 1, 1, 73, null, IllegalStateException.class, problem);
        child.testFailure = null;
        ((Host.Parent) child).value = new Object();
        for (Call call : paths(child, a, method, arguments, parentMethod, interfaceMethod))
            execute(call, child, a, method, arguments, null, 1, 0, 7, null, ClassCastException.class, null);
        child.value = a;
        Object[] empty = new Object[0];
        for (Call call : paths(child, a, method, empty, parentMethod, interfaceMethod))
            execute(call, child, a, method, empty, a, 1, 1, 73, null, ArrayIndexOutOfBoundsException.class, null);
        for (Call call : paths(child, a, method, null, parentMethod, interfaceMethod))
            execute(call, child, a, method, null, a, 1, 1, 73, null, NullPointerException.class, null);
        for (Call call : paths(null, a, method, arguments, parentMethod, interfaceMethod))
            execute(call, child, a, method, arguments, a, 0, 0, 0, null, NullPointerException.class, null);
        System.out.println("values=" + values + ",identities=" + identities
                + ",states=" + states + ",failures=" + failures);
        System.out.println("fixed-interface-throws=retained,empty-parent-throws=retained,bridge-casts-and-ordered-effects=matched");
    }
}
