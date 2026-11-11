package probe;
import fixture.*;
import java.lang.invoke.*;
import java.lang.reflect.*;
import java.util.Arrays;
public final class FinalNestedVisibilityBridgeHarness {
    public static final class FuturePayload extends Payload { }
    static void require(boolean value) {
        if (!value) throw new AssertionError("owned final nested visibility bridge mismatch");
    }
    @SuppressWarnings("unchecked")
    static <E extends Throwable> void raise(Throwable failure) throws E { throw (E) failure; }
    static void identity() throws Exception {
        Class<?> type = Owner.Child.class;
        Class<?> hidden = Class.forName("fixture.Family$Hidden", false, Owner.class.getClassLoader());
        require(type.getModifiers() == (Modifier.PUBLIC | Modifier.FINAL | Modifier.STATIC));
        require(type.isMemberClass() && !type.isAnonymousClass() && !type.isLocalClass());
        require(type.getDeclaringClass() == Owner.class && type.getEnclosingClass() == Owner.class);
        require(type.getEnclosingMethod() == null && type.getEnclosingConstructor() == null);
        require(type.getSuperclass() == hidden && type.getGenericSuperclass() == hidden);
        require(type.getInterfaces().length == 0 && type.getTypeParameters().length == 0 && type.getDeclaredFields().length == 0);
        require(hidden.getModifiers() == (Modifier.PROTECTED | Modifier.STATIC));
        require(hidden.getEnclosingClass() == Family.class && hidden.getSuperclass() == Base.class);
        require(hidden.getTypeParameters().length == 0);
        Constructor<?>[] constructors = type.getDeclaredConstructors();
        require(constructors.length == 1 && constructors[0].getModifiers() == Modifier.PUBLIC && constructors[0].getParameterCount() == 0);
        int originals = 0;
        for (Method method : type.getDeclaredMethods()) {
            if (method.getName().equals("__neverdThrow")) {
                require(method.getModifiers() == (Modifier.PRIVATE | Modifier.STATIC));
                require(method.getReturnType() == RuntimeException.class && Arrays.equals(method.getParameterTypes(), new Class<?>[]{Throwable.class}));
                require(method.getTypeParameters().length == 1 && Arrays.equals(method.getExceptionTypes(), new Class<?>[]{Throwable.class}));
                TypeVariable<?> variable = method.getTypeParameters()[0];
                require(Arrays.equals(variable.getBounds(), new Type[]{Throwable.class}));
                require(Arrays.equals(method.getGenericExceptionTypes(), new Type[]{variable}));
                require(method.getDeclaredAnnotations().length == 0);
            } else {
                require(method.getModifiers() == (Modifier.PUBLIC | 0x40 | 0x1000) && method.isBridge() && method.isSynthetic());
                require(method.getTypeParameters().length == 0 && method.getExceptionTypes().length == 0 && method.getDeclaredAnnotations().length == 0);
                if (method.getName().equals("apply"))
                    require(method.getReturnType() == void.class && Arrays.equals(method.getParameterTypes(), new Class<?>[]{Payload.class}));
                else {
                    require(method.getName().equals("wide") && method.getReturnType() == long.class);
                    require(Arrays.equals(method.getParameterTypes(), new Class<?>[]{long.class}));
                }
                ++originals;
            }
        }
        require(originals == 2);
        Type parentType = Owner.class.getGenericSuperclass();
        require(parentType instanceof ParameterizedType);
        ParameterizedType parent = (ParameterizedType) parentType;
        require(parent.getRawType() == Family.class && Arrays.equals(parent.getActualTypeArguments(), new Type[]{Owner.class}));
        TypeVariable<?>[] formals = Family.class.getTypeParameters();
        require(formals.length == 1 && Family.class.getModifiers() == (Modifier.PUBLIC | Modifier.ABSTRACT));
        Type boundType = formals[0].getBounds()[0];
        require(boundType instanceof ParameterizedType);
        ParameterizedType bound = (ParameterizedType) boundType;
        require(bound.getRawType() == Family.class && Arrays.equals(bound.getActualTypeArguments(), new Type[]{formals[0]}));
    }
    public static void main(String[] args) throws Throwable {
        final int route = Integer.parseInt(args[0]), operation = Integer.parseInt(args[1]);
        final int kind = Integer.parseInt(args[2]), mode = Integer.parseInt(args[3]);
        identity();
        final Method method = operation == 0 ? Owner.Child.class.getDeclaredMethod("apply", Payload.class)
                : Owner.Child.class.getDeclaredMethod("wide", long.class);
        final MethodHandle handle = MethodHandles.lookup().findVirtual(Owner.Child.class,
                operation == 0 ? "apply" : "wide", operation == 0
                ? MethodType.methodType(void.class, Payload.class) : MethodType.methodType(long.class, long.class));
        final Owner.Child first = new Owner.Child(), second = new Owner.Child();
        for (int attempt = 0; attempt < 2; ++attempt) {
            final Payload initial = kind == 1 ? null : new FuturePayload(), replacement = new FuturePayload(), marker = new FuturePayload();
            final long number = 0x100000003L + kind + attempt, changedNumber = Long.MIN_VALUE + 17 + attempt;
            final long numberMarker = 0x22334455667788L, resultAfter = Long.MIN_VALUE + 0x1234567L + attempt;
            final Throwable selected = mode == 1 ? new IllegalStateException() : mode == 3 ? new AssertionError() : new java.io.IOException();
            final int[] calls = {0, 0, 0};
            Trace.events = 0; Trace.current = kind == 2 ? null : first; Trace.input = initial; Trace.number = number;
            Trace.receiver = null; Trace.argument = marker; Trace.numberArgument = numberMarker; Trace.result = 0x44332211L;
            Trace.receiverAction = new Runnable() { public void run() {
                require(++calls[0] == 1 && Trace.events == 1);
                if (mode == 5) FinalNestedVisibilityBridgeHarness.<RuntimeException>raise(selected);
            }};
            Trace.argumentAction = new Runnable() { public void run() {
                require(++calls[1] == 1 && calls[0] == 1 && Trace.events == 12);
                if (mode == 6) FinalNestedVisibilityBridgeHarness.<RuntimeException>raise(selected);
                if (mode == 7) { Trace.current = second; Trace.input = replacement; Trace.number = changedNumber; }
            }};
            Trace.action = mode == 4 ? null : new Runnable() { public void run() {
                require(++calls[2] == 1 && calls[1] == 1 && Trace.events == 123 && Trace.receiver == first);
                require(Trace.argument == (operation == 0 ? (mode == 7 ? replacement : initial) : marker));
                require(Trace.numberArgument == (operation == 1 ? (mode == 7 ? changedNumber : number) : numberMarker));
                Trace.result = resultAfter;
                if (mode >= 1 && mode <= 3) FinalNestedVisibilityBridgeHarness.<RuntimeException>raise(selected);
            }};
            Throwable failure = null; long result = 0;
            try {
                if (operation == 0) FinalNestedVisibilityBridgeCaller.apply(route, method, handle);
                else result = FinalNestedVisibilityBridgeCaller.wide(route, method, handle);
            } catch (Throwable caught) { failure = caught; }
            final boolean body = mode != 5 && mode != 6 && kind != 2;
            final boolean complete = body && (mode == 0 || mode == 7);
            require(Trace.events == (mode == 5 ? 1 : !body ? 12 : complete ? 1234 : 123));
            require(calls[0] == 1 && calls[1] == (mode == 5 ? 0 : 1) && calls[2] == (body && mode != 4 ? 1 : 0));
            require(Trace.receiver == (body ? first : null));
            require(Trace.argument == (body && operation == 0 ? (mode == 7 ? replacement : initial) : marker));
            require(Trace.numberArgument == (body && operation == 1 ? (mode == 7 ? changedNumber : number) : numberMarker));
            if (complete) require(failure == null && (operation == 0 || result == resultAfter));
            else if (mode == 5 || mode == 6 || body && mode >= 1 && mode <= 3) require(failure == selected);
            else require(failure instanceof NullPointerException);
            identity();
        }
        System.out.println("attempts=2,final-nested-visibility-bridge=matched");
    }
}
