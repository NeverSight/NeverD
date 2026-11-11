package probe;
import fixture.*;
import java.lang.invoke.*;
import java.lang.reflect.*;
import java.util.Arrays;
public final class ClosedVisibilityResultBridgeHarness {
    static void require(boolean condition) {
        if (!condition) throw new AssertionError("owned closed visibility result bridge mismatch");
    }
    @SuppressWarnings("unchecked")
    static <E extends Throwable> void raise(Throwable failure) throws E { throw (E) failure; }
    static Value direct(Child child) {
        Trace.hit(1);
        Value result = child.first();
        Trace.hit(8);
        return result;
    }
    public static void main(String[] args) throws Throwable {
        final int route = Integer.parseInt(args[0]), mode = Integer.parseInt(args[1]);
        final boolean future = Boolean.parseBoolean(args[2]);
        Class<?> parent = Child.class.getSuperclass();
        require(parent.getModifiers() == Modifier.ABSTRACT && parent.getSuperclass() == Root.class);
        require(Child.class.getModifiers() == (Modifier.PUBLIC | Modifier.FINAL));
        require(Child.class.getTypeParameters().length == 0);
        require(Child.class.getGenericSuperclass() instanceof ParameterizedType);
        ParameterizedType edge = (ParameterizedType) Child.class.getGenericSuperclass();
        require(edge.getRawType() == parent && Arrays.equals(edge.getActualTypeArguments(), new Type[]{Value.class}));
        TypeVariable<?>[] variables = parent.getTypeParameters();
        require(variables.length == 1 && Arrays.equals(variables[0].getBounds(), new Type[]{Input.class}));
        Method original = parent.getDeclaredMethod("first");
        require(original.getGenericReturnType().equals(variables[0]));
        require(original.getReturnType() == Input.class && original.getModifiers() == Modifier.PUBLIC);
        require(original.getTypeParameters().length == 0 && original.getExceptionTypes().length == 0);
        Field field = parent.getDeclaredField("value");
        field.setAccessible(true);
        require(field.getType() == Input.class && field.getGenericType().equals(variables[0]));
        Method bridge = Child.class.getDeclaredMethod("first");
        require(bridge.isBridge() && bridge.isSynthetic());
        require(bridge.getModifiers() == (Modifier.PUBLIC | 0x1040));
        require(bridge.getReturnType() == Input.class && bridge.getGenericReturnType() == Input.class);
        require(bridge.getTypeParameters().length == 0 && bridge.getExceptionTypes().length == 0);
        int entries = 0;
        for (Method method : Child.class.getDeclaredMethods()) {
            if (method.getName().equals("__neverdThrow")) {
                require(method.getModifiers() == (Modifier.PRIVATE | Modifier.STATIC));
                require(Arrays.equals(method.getParameterTypes(), new Class<?>[]{Throwable.class}));
                require(method.getReturnType() == RuntimeException.class && method.getTypeParameters().length == 1);
                TypeVariable<?> variable = method.getTypeParameters()[0];
                require(Arrays.equals(variable.getBounds(), new Type[]{Throwable.class}));
                require(Arrays.equals(method.getGenericExceptionTypes(), new Type[]{variable}));
            } else {
                require(method.equals(bridge));
                ++entries;
            }
        }
        require(entries == 1);
        MethodHandle erased = MethodHandles.publicLookup().findVirtual(Child.class, "first", MethodType.methodType(Input.class));
        for (int attempt = 0; attempt < 2; ++attempt) {
            final boolean wrong = mode == 2 || mode == 9;
            final Input selectedValue = mode == 1 ? null : wrong ? new Input() { }
                    : future ? new ClosedVisibilityResultBridgeCaller.Future() : new Value();
            final Child child = mode == 3 ? null : new Child();
            final Value replacement = new Value();
            if (child != null) field.set(child, selectedValue);
            final Throwable selectedFailure = mode == 4 || mode == 9 || mode == 11 ? new java.io.IOException()
                    : mode == 6 ? new AssertionError() : new IllegalStateException();
            final int[] counts = {0, 0};
            Trace.events = 0;
            Trace.receiver = null;
            Trace.before = null;
            Trace.valueReceiver = null;
            Trace.answer = attempt == 0 ? Integer.MIN_VALUE : 0x1234abcd;
            Trace.action = mode == 7 ? null : new Runnable() { public void run() {
                int call = ++counts[0];
                require(Trace.receiver == child);
                require(Trace.before == (call == 2 ? replacement : selectedValue));
                require(call == 1 || mode == 10 && call == 2);
                if (mode >= 4 && mode <= 6 || mode == 9)
                    ClosedVisibilityResultBridgeHarness.<RuntimeException>raise(selectedFailure);
                if ((mode == 8 || mode == 10) && call == 1) {
                    try {
                        field.set(child, replacement);
                        if (mode == 10) require(child.first() == replacement);
                    } catch (Throwable failure) {
                        ClosedVisibilityResultBridgeHarness.<RuntimeException>raise(failure);
                    }
                }
            }};
            Trace.valueAction = new Runnable() { public void run() {
                require(++counts[1] == 1 && Trace.valueReceiver == selectedValue);
                if (mode == 11) ClosedVisibilityResultBridgeHarness.<RuntimeException>raise(selectedFailure);
            }};
            Input result = null;
            Throwable failure = null;
            int value = 0;
            try {
                if (route == 0) result = direct(child);
                else if (route == 1) result = ClosedVisibilityResultBridgeCaller.value(child);
                else if (route == 2) {
                    Trace.hit(1);
                    try { result = (Input) bridge.invoke(child); }
                    catch (InvocationTargetException caught) { throw caught.getCause(); }
                    Trace.hit(8);
                } else if (route == 3) {
                    Trace.hit(1);
                    result = (Input) erased.invokeExact(child);
                    Trace.hit(8);
                } else if (route == 4) result = ClosedVisibilityResultBridgeCaller.input(child);
                else if (route == 5) result = Core.input(child);
                else result = Core.value(child);
                require(result == selectedValue);
                if (result instanceof Value) value = ((Value) result).read();
                Trace.hit(2);
            } catch (Throwable caught) { failure = caught; }
            boolean entered = child != null;
            boolean bodyFailure = entered && (mode >= 4 && mode <= 7 || mode == 9);
            boolean returned = entered && !bodyFailure;
            boolean castFailure = returned && wrong && (route == 0 || route == 1 || route == 6);
            boolean read = returned && !castFailure && selectedValue instanceof Value;
            boolean readFailure = read && mode == 11;
            String sequence = "1";
            if (entered) sequence += "3";
            if (mode == 10) sequence += "34";
            if (returned) sequence += "4";
            if (returned && !castFailure) sequence += "8";
            if (read) sequence += (future ? "7" : "") + "5";
            if (read && !readFailure) sequence += "6" + (future ? "9" : "");
            if (returned && !castFailure && !readFailure) sequence += "2";
            require(Trace.events == Long.parseLong(sequence));
            require(counts[0] == (entered && mode != 7 ? mode == 10 ? 2 : 1 : 0));
            require(counts[1] == (read ? 1 : 0));
            require(Trace.receiver == (entered ? child : null));
            require(Trace.before == (entered ? mode == 10 ? replacement : selectedValue : null));
            require(Trace.valueReceiver == (read ? selectedValue : null));
            if (!entered || entered && mode == 7) require(failure instanceof NullPointerException);
            else if (bodyFailure || readFailure) require(failure == selectedFailure);
            else if (castFailure) require(failure instanceof ClassCastException);
            else require(failure == null && result == selectedValue);
            if (read && !readFailure) require(value == Trace.answer);
            if (child != null) require(field.get(child) == (mode == 8 || mode == 10 ? replacement : selectedValue));
        }
        System.out.println("attempts=2,closed-visibility-result-bridge=matched");
    }
}
