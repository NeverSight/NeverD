package fixture;
import java.lang.invoke.*;
import java.lang.reflect.*;
public final class ClosedVisibilityParameterBridgeHarness {
    static final class Value implements Child.Left { }
    static final class Other implements First<Child> { }
    static void require(boolean condition) {
        if (!condition) throw new AssertionError("owned closed visibility parameter bridge mismatch");
    }
    @SuppressWarnings("unchecked")
    static <E extends Throwable> void raise(Throwable failure) throws E { throw (E) failure; }
    public static void main(String[] args) throws Throwable {
        final int route = Integer.parseInt(args[0]), mode = Integer.parseInt(args[2]);
        final boolean future = Boolean.parseBoolean(args[1]);
        Method bridge = Child.class.getDeclaredMethod("apply", First.class);
        int originalEntries = 0;
        for (Method declared : Child.class.getDeclaredMethods()) {
            if (declared.getName().equals("__neverdThrow")) {
                require(declared.getModifiers() == (Modifier.PRIVATE | Modifier.STATIC));
                require(java.util.Arrays.equals(declared.getParameterTypes(), new Class<?>[]{Throwable.class}));
                require(declared.getReturnType() == RuntimeException.class && declared.getTypeParameters().length == 1);
                TypeVariable<?> variable = declared.getTypeParameters()[0];
                require(java.util.Arrays.equals(variable.getBounds(), new Type[]{Throwable.class}));
                require(java.util.Arrays.equals(declared.getGenericExceptionTypes(), new Type[]{variable}));
            } else {
                require(declared.equals(bridge));
                ++originalEntries;
            }
        }
        require(originalEntries == 1 && bridge.isBridge() && bridge.isSynthetic());
        require(bridge.getDeclaringClass() == Child.class && bridge.getModifiers() == (Modifier.PUBLIC | 0x1040));
        require(bridge.getGenericParameterTypes()[0] == First.class && bridge.getExceptionTypes().length == 0);
        require(Base.class.getModifiers() == Modifier.ABSTRACT && Child.class.getTypeParameters().length == 0);
        Type[] closed = ((ParameterizedType) Child.class.getGenericSuperclass()).getActualTypeArguments();
        require(closed.length == 3 && closed[0] == Child.class && closed[1] == Child.Left.class && closed[2] == Child.Right.class);
        TypeVariable<?>[] formals = Base.class.getTypeParameters();
        require(formals.length == 3 && Base.class.getSuperclass() == Root.class);
        ParameterizedType self = (ParameterizedType) formals[0].getBounds()[0];
        require(self.getRawType() == Base.class && java.util.Arrays.equals(self.getActualTypeArguments(), formals));
        for (int index = 1; index < 3; ++index) {
            ParameterizedType bound = (ParameterizedType) formals[index].getBounds()[0];
            require(bound.getRawType() == (index == 1 ? First.class : Second.class));
            require(bound.getActualTypeArguments()[0].equals(formals[0]));
        }
        Method original = Base.class.getDeclaredMethod("apply", First.class);
        require(original.getTypeParameters().length == 0 && original.getGenericParameterTypes()[0].equals(formals[1]));
        require(original.getModifiers() == Modifier.PUBLIC && !original.isBridge());
        Method core = Core.class.getMethod("send", Child.class, Child.Left.class);
        MethodHandle direct = MethodHandles.lookup().findStatic(Core.class, "send",
                MethodType.methodType(void.class, Child.class, Child.Left.class));
        MethodHandle raw = MethodHandles.lookup().findVirtual(Child.class, "apply",
                MethodType.methodType(void.class, First.class));
        for (int attempt = 0; attempt < 2; ++attempt) {
            final boolean wrong = mode == 2 && route >= 3;
            final First<Child> value = mode == 1 ? null : wrong ? new Other() : new Value();
            final Child child = mode == 3 ? null : future ? new ClosedVisibilityParameterBridgeFuture() : new Child();
            final Throwable selected = mode == 5 || mode == 8 ? new java.io.IOException()
                    : mode == 6 || mode == 9 ? new AssertionError() : new IllegalStateException();
            final int[] counts = {0, 0};
            Trace.events = 0; Trace.receiver = null; Trace.argument = null;
            Trace.futureAction = mode == 11 ? null : new Runnable() { public void run() {
                require(++counts[0] == 1 && counts[1] == 0 && Trace.events == 19);
                require(Trace.receiver == null && Trace.argument == null);
                if (mode >= 8 && mode <= 10) ClosedVisibilityParameterBridgeHarness.<RuntimeException>raise(selected);
            }};
            Trace.action = mode == 7 ? null : new Runnable() { public void run() {
                require(++counts[1] == 1 && counts[0] == (future ? 1 : 0));
                require(Trace.receiver == child && Trace.argument == value);
                if (mode >= 4 && mode <= 6) ClosedVisibilityParameterBridgeHarness.<RuntimeException>raise(selected);
            }};
            Throwable failure = null;
            try {
                if (route == 0) Core.send(child, (Child.Left) value);
                else if (route == 1) {
                    try { core.invoke(null, child, value); }
                    catch (InvocationTargetException caught) { throw caught.getCause(); }
                } else if (route == 2) direct.invokeExact(child, (Child.Left) value);
                else if (route == 3) {
                    Trace.hit(1);
                    try { bridge.invoke(child, value); }
                    catch (InvocationTargetException caught) { throw caught.getCause(); }
                    Trace.hit(8);
                } else if (route == 4) {
                    Trace.hit(1);
                    raw.invokeExact(child, (First) value);
                    Trace.hit(8);
                } else ClosedVisibilityParameterBridgeCaller.send(child, value);
            } catch (Throwable caught) { failure = caught; }
            boolean castFailure = future && wrong && child != null;
            boolean futureEntered = future && child != null && !castFailure;
            boolean futureFailure = futureEntered && mode >= 8;
            boolean entered = child != null && !castFailure && !futureFailure;
            boolean completed = entered && !(mode >= 4 && mode <= 7);
            String sequence = "1";
            if (futureEntered) sequence += "9";
            if (entered) sequence += "3";
            if (completed) sequence += "48";
            require(Trace.events == Long.parseLong(sequence));
            require(counts[0] == (futureEntered && mode != 11 ? 1 : 0));
            require(counts[1] == (entered && mode != 7 ? 1 : 0));
            require(Trace.receiver == (entered ? child : null) && Trace.argument == (entered ? value : null));
            if (child == null || futureFailure && mode == 11 || entered && mode == 7)
                require(failure instanceof NullPointerException);
            else if (castFailure) require(failure instanceof ClassCastException);
            else if (futureFailure || entered && mode >= 4 && mode <= 6) require(failure == selected);
            else require(failure == null);
        }
        System.out.println("attempts=2,closed-visibility-parameter-bridge=matched");
    }
}
