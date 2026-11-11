import fixture.*;
import java.lang.reflect.*;
import java.util.*;

public final class BoundedFormalObjectContractsHarness {
    private static int values, identities, states, failures;
    private interface Call { Object run() throws Throwable; }
    private static final class Sink implements Callback<String, byte[]> {
        public void changed(String key, byte[] value) {
            throw new AssertionError("identity-only callback was executed");
        }
    }
    private static Object reflect(Method method, Object receiver, Object... args) throws Throwable {
        try { return method.invoke(receiver, args); }
        catch (InvocationTargetException error) { throw error.getCause(); }
    }
    private static void same(Object actual, Object expected) {
        if (actual != expected) throw new AssertionError("bounded source or exception identity changed");
        identities++;
    }
    @SuppressWarnings({"rawtypes", "unchecked"})
    private static Call[] paths(Child child, Callback<String, byte[]> callback, boolean echo,
                                Method implementation, Method declaration) {
        Parent<Token> parent = child;
        Parent raw = child;
        Source view = child;
        if (echo) {
            return new Call[] {
                () -> child.echo(callback), () -> parent.echo(callback), () -> raw.echo(callback),
                () -> view.echo(callback), () -> reflect(implementation, child, callback),
                () -> reflect(declaration, child, callback)
            };
        }
        return new Call[] {
            () -> child.get(), () -> parent.get(), () -> raw.get(), () -> view.get(),
            () -> reflect(implementation, child), () -> reflect(declaration, child)
        };
    }
    private static void execute(Call call, Child child, Token source, Callback<String, byte[]> callback,
                                boolean echo, boolean absent, RuntimeException problem) {
        int gets = child.gets;
        int echoes = child.echoes;
        Callback<String, byte[]> previous = child.seen;
        child.events = 0;
        try {
            Object result = call.run();
            if (absent || problem != null) throw new AssertionError("bounded source failure missing");
            same(result, source);
            values++;
        } catch (Throwable error) {
            Class<?> expected = absent ? NullPointerException.class : IllegalStateException.class;
            if ((!absent && problem == null) || error.getClass() != expected)
                throw new AssertionError("bounded source exception type changed", error);
            if (problem != null) same(error, problem);
            failures++;
        }
        int delta = absent ? 0 : 1;
        if (child.gets != gets + (echo ? 0 : delta)
                || child.echoes != echoes + (echo ? delta : 0)
                || child.events != delta * (echo ? 2 : 1))
            throw new AssertionError("bounded source state or failure order changed");
        same(child.seen, echo && !absent ? callback : previous);
        same(child.source, source);
        states++;
    }
    private static Call[] objectPaths(Child child, Object argument, String name,
                                       Method objectMethod, Method interfaceMethod) {
        Parent<Token> parent = child;
        Source source = child;
        ObjectView view = child;
        if (name.equals("equals")) {
            return new Call[] {
                () -> child.equals(argument), () -> parent.equals(argument), () -> source.equals(argument),
                () -> view.equals(argument), () -> reflect(objectMethod, child, argument),
                () -> reflect(interfaceMethod, child, argument)
            };
        }
        if (name.equals("hashCode")) {
            return new Call[] {
                () -> child.hashCode(), () -> parent.hashCode(), () -> source.hashCode(),
                () -> view.hashCode(), () -> reflect(objectMethod, child),
                () -> reflect(interfaceMethod, child)
            };
        }
        return new Call[] {
            () -> child.toString(), () -> parent.toString(), () -> source.toString(),
            () -> view.toString(), () -> reflect(objectMethod, child),
            () -> reflect(interfaceMethod, child)
        };
    }
    private static void objectCall(Call call, Child child, Object expected, boolean absent) {
        int gets = child.gets, echoes = child.echoes, events = child.events;
        Token source = child.source;
        Callback<String, byte[]> seen = child.seen;
        try {
            Object result = call.run();
            if (absent) throw new AssertionError("Object null receiver failure missing");
            if (!expected.equals(result)) throw new AssertionError("inherited Object result changed");
            values++;
        } catch (Throwable error) {
            if (!absent || error.getClass() != NullPointerException.class)
                throw new AssertionError("inherited Object exception changed", error);
            failures++;
        }
        if (child.gets != gets || child.echoes != echoes || child.events != events)
            throw new AssertionError("Object method executed an unrelated bounded source body");
        same(child.source, source);
        same(child.seen, seen);
        states++;
    }
    @SuppressWarnings("rawtypes")
    private static Call[] typePaths(Child child, Method implementation, Method declaration) {
        Parent<Token> parent = child;
        Parent raw = child;
        Source source = child;
        TypeView view = child;
        return new Call[] {
            () -> child.type(), () -> parent.type(), () -> raw.type(), () -> source.type(), () -> view.type(),
            () -> reflect(implementation, child), () -> reflect(declaration, child)
        };
    }
    private static void typeCall(Call call, Child child, boolean absent) {
        int gets = child.gets, echoes = child.echoes, events = child.events;
        int defaultCalls = Trace.calls, defaultEvents = Trace.events;
        OrderNode receiver = Trace.receiver, argument = Trace.argument;
        Token source = child.source;
        Callback<String, byte[]> seen = child.seen;
        try {
            Object result = call.run();
            if (absent) throw new AssertionError("class-token null receiver failure missing");
            same(result, Token.class);
            values++;
        } catch (Throwable error) {
            if (!absent || error.getClass() != NullPointerException.class)
                throw new AssertionError("class-token result or exception changed", error);
            failures++;
        }
        if (child.gets != gets || child.echoes != echoes || child.events != events
                || Trace.calls != defaultCalls || Trace.events != defaultEvents
                || Trace.receiver != receiver || Trace.argument != argument)
            throw new AssertionError("class-token getter acquired source or default effects");
        same(child.source, source); same(child.seen, seen);
        states++;
    }
    @SuppressWarnings({"rawtypes", "unchecked"})
    private static Call[] comparePaths(Child receiver, OrderNode argument, Method typed, Method bridge, Method root) {
        OrderNode node = receiver;
        Ordering<OrderNode> ordering = receiver;
        Ordering raw = receiver;
        return new Call[] {
            () -> receiver.compare(argument), () -> node.compare(argument), () -> ordering.compare(argument),
            () -> raw.compare(argument), () -> reflect(typed, receiver, argument),
            () -> reflect(bridge, receiver, argument), () -> reflect(root, receiver, argument)
        };
    }
    @SuppressWarnings({"rawtypes", "unchecked"})
    private static Call[] erasedComparePaths(Child receiver, Object argument, Method bridge, Method root) {
        Ordering raw = receiver;
        return new Call[] {() -> raw.compare(argument), () -> reflect(bridge, receiver, argument),
                           () -> reflect(root, receiver, argument)};
    }
    private static void compareCall(Call call, Child left, Child right, OrderNode receiver, OrderNode argument,
                                    int expected, int events, int defaultCalls, int leftGets, int rightGets,
                                    Class<?> failure, RuntimeException problem) {
        Token leftSource = left.source, rightSource = right.source;
        Trace.calls = 0; Trace.events = 0; Trace.receiver = null; Trace.argument = null;
        left.gets = 0; left.echoes = 0; left.events = 0;
        right.gets = 0; right.echoes = 0; right.events = 0;
        try {
            Object result = call.run();
            if (failure != null) throw new AssertionError("default comparison failure missing");
            if (!(result instanceof Integer) || ((Integer) result).intValue() != expected)
                throw new AssertionError("bounded getter default result changed");
            values++;
        } catch (Throwable error) {
            if (failure == null || error.getClass() != failure)
                throw new AssertionError("bounded getter default exception changed", error);
            if (problem != null) same(error, problem);
            failures++;
        }
        if (Trace.calls != defaultCalls || Trace.events != events || left.gets != leftGets || right.gets != rightGets
                || left.events != leftGets || right.events != rightGets || left.echoes != 0 || right.echoes != 0)
            throw new AssertionError("default cast or bounded getter effect order changed");
        same(Trace.receiver, receiver); same(Trace.argument, argument);
        same(left.source, leftSource); same(right.source, rightSource);
        states++;
    }
    private static void callbackParameter(Method method) {
        Type parameter = method.getGenericParameterTypes()[0];
        if (!(parameter instanceof ParameterizedType)
                || ((ParameterizedType) parameter).getRawType() != Callback.class
                || !Arrays.equals(((ParameterizedType) parameter).getActualTypeArguments(),
                                  new Type[] {String.class, byte[].class}))
            throw new AssertionError("complete fixed callback parameter changed");
    }
    private static void typeResult(Method method) {
        Type result = method.getGenericReturnType();
        if (!(result instanceof ParameterizedType) || ((ParameterizedType) result).getRawType() != Class.class
                || ((ParameterizedType) result).getOwnerType() != null
                || ((ParameterizedType) result).getActualTypeArguments().length != 1
                || !(((ParameterizedType) result).getActualTypeArguments()[0] instanceof WildcardType)
                || method.getReturnType() != Class.class || method.getTypeParameters().length != 0
                || method.isBridge() || method.isSynthetic())
            throw new AssertionError("complete class-token result structure changed");
        WildcardType argument = (WildcardType) ((ParameterizedType) result).getActualTypeArguments()[0];
        if (!Arrays.equals(argument.getUpperBounds(), new Type[] {Bound.class})
                || argument.getLowerBounds().length != 0)
            throw new AssertionError("class-token wildcard bound or direction changed");
    }
    private static void metadata(Method getter, Method echo, Method sourceGetter, Method sourceEcho, Class<?> typeOwner)
            throws Exception {
        TypeVariable<?> formal = Parent.class.getTypeParameters()[0];
        if (!Arrays.equals(formal.getBounds(), new Type[] {Bound.class})
                || !getter.getGenericReturnType().equals(formal) || !echo.getGenericReturnType().equals(formal)
                || !Parent.class.getDeclaredField("source").getGenericType().equals(formal)
                || !Child.class.getGenericSuperclass().getTypeName().equals("fixture.Parent<fixture.Token>")
                || !Arrays.equals(Parent.class.getGenericInterfaces(), new Type[] {Source.class})
                || !Arrays.equals(Source.class.getGenericInterfaces(),
                                  new Type[] {ObjectView.class, OrderNode.class, TypeView.class})
                || !Bound.class.isInterface() || ObjectView.class.getTypeParameters().length != 0)
            throw new AssertionError("original bounded formal or complete source ancestor scope changed");
        for (Method method : new Method[] {getter, echo, sourceGetter, sourceEcho}) {
            if (method.getReturnType() != Bound.class || method.isBridge() || method.isSynthetic()
                    || method.getTypeParameters().length != 0 || method.getExceptionTypes().length != 0
                    || method.getGenericExceptionTypes().length != 0)
                throw new AssertionError("same-erasure bounded return acquired a bridge or different scope");
        }
        if (sourceGetter.getGenericReturnType() != Bound.class || sourceEcho.getGenericReturnType() != Bound.class
                || Parent.class.getDeclaredField("source").getType() != Bound.class)
            throw new AssertionError("bounded runtime result or field erasure changed");
        callbackParameter(echo); callbackParameter(sourceEcho);
        Method typeImplementation = typeOwner.getDeclaredMethod("type");
        Method typeDeclaration = TypeView.class.getDeclaredMethod("type");
        typeResult(typeImplementation); typeResult(typeDeclaration);
        if (typeImplementation.isDefault() != (typeOwner == Source.class)
                || typeImplementation.getModifiers() != Modifier.PUBLIC || typeDeclaration.isDefault())
            throw new AssertionError("class-token provider instance kind changed");
        if (Child.class.getMethod("get").getDeclaringClass() != Parent.class
                || Child.class.getMethod("echo", Callback.class).getDeclaringClass() != Parent.class
                || Parent.class.getMethod("type").getDeclaringClass() != typeOwner
                || Child.class.getMethod("type").getDeclaringClass() != typeOwner
                || Source.class.getMethod("type").getDeclaringClass() !=
                    (typeOwner == Source.class ? Source.class : TypeView.class))
            throw new AssertionError("closed child acquired an invented getter");
        for (String name : new String[] {"equals", "hashCode", "toString"}) {
            Class<?>[] parameters = name.equals("equals") ? new Class<?>[] {Object.class} : new Class<?>[0];
            if (Parent.class.getMethod(name, parameters).getDeclaringClass() != Object.class
                    || Child.class.getMethod(name, parameters).getDeclaringClass() != Object.class
                    || Source.class.getMethod(name, parameters).getDeclaringClass() != ObjectView.class)
                throw new AssertionError("Object contract declaring owner changed");
        }
        for (Class<?> owner : new Class<?>[] {Parent.class, Child.class}) {
            for (Method method : owner.getDeclaredMethods()) {
                if (Arrays.asList("equals", "hashCode", "toString", "compare").contains(method.getName())
                        || (owner == Child.class && Arrays.asList("get", "echo", "type").contains(method.getName()))
                        || (typeOwner == Source.class && method.getName().equals("type")))
                    throw new AssertionError("consumer acquired an invented contract implementation");
            }
        }
        Method typed = OrderNode.class.getDeclaredMethod("compare", OrderNode.class);
        Method bridge = OrderNode.class.getDeclaredMethod("compare", Object.class);
        Method ordering = Ordering.class.getDeclaredMethod("compare", Object.class);
        if (!OrderNode.class.getGenericInterfaces()[0].getTypeName().equals("fixture.Ordering<fixture.OrderNode>")
                || !typed.isDefault() || typed.isBridge() || typed.isSynthetic()
                || !bridge.isDefault() || !bridge.isBridge() || !bridge.isSynthetic()
                || bridge.getModifiers() != (Modifier.PUBLIC | 0x1040)
                || Parent.class.getMethod("compare", OrderNode.class).getDeclaringClass() != OrderNode.class
                || Child.class.getMethod("compare", Object.class).getDeclaringClass() != OrderNode.class
                || !ordering.getGenericParameterTypes()[0].equals(Ordering.class.getTypeParameters()[0]))
            throw new AssertionError("complete default root or original bridge owner changed");
        // The exact original inventory has sixteen methods and four constructors; helpers are checked by the runner.
        for (Method method : new Method[] {getter, echo, sourceGetter, sourceEcho,
                Bound.class.getDeclaredMethod("value"), Token.class.getDeclaredMethod("value"),
                Callback.class.getDeclaredMethod("changed", Object.class, Object.class),
                ObjectView.class.getDeclaredMethod("equals", Object.class),
                ObjectView.class.getDeclaredMethod("hashCode"), ObjectView.class.getDeclaredMethod("toString"),
                OrderNode.class.getDeclaredMethod("get"), typed, bridge, ordering,
                typeImplementation, typeDeclaration}) {
            if (method.getExceptionTypes().length != 0 || method.getGenericExceptionTypes().length != 0)
                throw new AssertionError("original method acquired declared Exceptions: " + method);
        }
        for (Constructor<?> constructor : new Constructor<?>[] {Parent.class.getDeclaredConstructor(),
                Child.class.getDeclaredConstructor(), Token.class.getDeclaredConstructor(int.class),
                Trace.class.getDeclaredConstructor()}) {
            if (constructor.getExceptionTypes().length != 0 || constructor.getGenericExceptionTypes().length != 0)
                throw new AssertionError("original constructor acquired declared Exceptions: " + constructor);
        }
    }
    public static void main(String[] args) throws Exception {
        if (args.length != 1 || (!args[0].equals("class") && !args[0].equals("default")))
            throw new AssertionError("class-token provider profile missing");
        Class<?> typeOwner = args[0].equals("default") ? Source.class : Parent.class;
        Method getter = Parent.class.getDeclaredMethod("get");
        Method echo = Parent.class.getDeclaredMethod("echo", Callback.class);
        Method sourceGetter = Source.class.getDeclaredMethod("get");
        Method sourceEcho = Source.class.getDeclaredMethod("echo", Callback.class);
        metadata(getter, echo, sourceGetter, sourceEcho, typeOwner);
        Child child = new Child();
        Token first = new Token(4), second = new Token(9);
        Callback<String, byte[]> sink = new Sink();
        for (Token token : new Token[] {first, second, null}) {
            child.source = token;
            Callback<String, byte[]> callback = token == second ? null : sink;
            for (Call call : paths(child, callback, false, getter, sourceGetter))
                execute(call, child, token, callback, false, false, null);
            if (token != null) token.value += 13;
            for (Call call : paths(child, callback, true, echo, sourceEcho))
                execute(call, child, token, callback, true, false, null);
        }
        child.source = second;
        RuntimeException problem = new IllegalStateException("owned bounded source failure");
        child.failure = problem;
        for (Call call : paths(child, sink, false, getter, sourceGetter))
            execute(call, child, second, sink, false, false, problem);
        for (Call call : paths(child, sink, true, echo, sourceEcho))
            execute(call, child, second, sink, true, false, problem);
        child.failure = null;
        for (Call call : paths(null, null, false, getter, sourceGetter))
            execute(call, child, second, null, false, true, null);
        for (Call call : paths(null, null, true, echo, sourceEcho))
            execute(call, child, second, null, true, true, null);
        child.failure = problem;
        for (String name : new String[] {"equals", "hashCode", "toString"}) {
            Class<?>[] parameters = name.equals("equals") ? new Class<?>[] {Object.class} : new Class<?>[0];
            Method objectMethod = Object.class.getDeclaredMethod(name, parameters);
            Method interfaceMethod = ObjectView.class.getDeclaredMethod(name, parameters);
            if (name.equals("equals")) {
                for (Object argument : new Object[] {child, new Child(), null, first}) {
                    for (Call call : objectPaths(child, argument, name, objectMethod, interfaceMethod))
                        objectCall(call, child, Boolean.valueOf(argument == child), false);
                }
            } else {
                Object expected = name.equals("hashCode") ? Integer.valueOf(System.identityHashCode(child))
                        : child.getClass().getName() + "@" + Integer.toHexString(System.identityHashCode(child));
                for (Call call : objectPaths(child, null, name, objectMethod, interfaceMethod))
                    objectCall(call, child, expected, false);
            }
            for (Call call : objectPaths(null, child, name, objectMethod, interfaceMethod))
                objectCall(call, child, null, true);
        }
        Method typeImplementation = typeOwner.getDeclaredMethod("type");
        Method typeDeclaration = TypeView.class.getDeclaredMethod("type");
        for (Call call : typePaths(child, typeImplementation, typeDeclaration))
            typeCall(call, child, false);
        for (Call call : typePaths(null, typeImplementation, typeDeclaration))
            typeCall(call, child, true);
        child.failure = null;
        Child other = new Child();
        child.tag = 2; other.tag = 3;
        other.source = new Token(13);
        Method typed = OrderNode.class.getDeclaredMethod("compare", OrderNode.class);
        Method bridge = OrderNode.class.getDeclaredMethod("compare", Object.class);
        Method ordering = Ordering.class.getDeclaredMethod("compare", Object.class);
        for (int pass = 0; pass < 3; pass++) {
            if (pass == 1) { child.source.value = -4; other.source.value = 21; }
            if (pass == 2) child.source = new Token(8);
            int expected = pass == 0 ? 9 : pass == 1 ? -25 : -13;
            for (Call call : comparePaths(child, other, typed, bridge, ordering))
                compareCall(call, child, other, child, other, expected, 1234, 1, 1, 1, null, null);
        }
        Trace.failure = problem;
        for (Call call : comparePaths(child, other, typed, bridge, ordering))
            compareCall(call, child, other, child, other, 0, 1, 1, 0, 0, IllegalStateException.class, problem);
        Trace.failure = null;
        child.failure = problem;
        for (Call call : comparePaths(child, other, typed, bridge, ordering))
            compareCall(call, child, other, child, other, 0, 12, 1, 1, 0, IllegalStateException.class, problem);
        child.failure = null;
        other.failure = problem;
        for (Call call : comparePaths(child, other, typed, bridge, ordering))
            compareCall(call, child, other, child, other, 0, 123, 1, 1, 1, IllegalStateException.class, problem);
        other.failure = null;
        for (Call call : comparePaths(child, null, typed, bridge, ordering))
            compareCall(call, child, other, child, null, 0, 12, 1, 1, 0, NullPointerException.class, null);
        child.source = null;
        for (Call call : comparePaths(child, other, typed, bridge, ordering))
            compareCall(call, child, other, child, other, 0, 12, 1, 1, 0, NullPointerException.class, null);
        child.source = first;
        for (Call call : comparePaths(null, other, typed, bridge, ordering))
            compareCall(call, child, other, null, null, 0, 0, 0, 0, 0, NullPointerException.class, null);
        Trace.failure = problem;
        for (Call call : erasedComparePaths(child, new Object(), bridge, ordering))
            compareCall(call, child, other, null, null, 0, 0, 0, 0, 0, ClassCastException.class, null);
        Trace.failure = null;
        System.out.println("values=" + values + ",identities=" + identities
                + ",states=" + states + ",failures=" + failures);
        System.out.println("bounded-return-formals=retained,object-declaring-owner=retained,default-bridge-owner=retained");
    }
}
