package probe;
import fixture.*;
import java.lang.invoke.*;
import java.lang.reflect.*;
import java.util.Arrays;
public final class UnusedPrivateIntersectionHarness {
    public static final class FutureInput { }
    public static final class FutureOwner extends Owner {
        public CharSequence render(Object input) { ++Trace.renderOverrides; return super.render(input); }
        public void apply(CharSequence text, boolean filter) { ++Trace.applyOverrides; super.apply(text, filter); }
    }
    static void require(boolean value) {
        if (!value) throw new AssertionError("owned unused private intersection mismatch");
    }
    @SuppressWarnings("unchecked")
    static <E extends Throwable> void raise(Throwable failure) throws E { throw (E) failure; }
    static void throwHelper(Method method) {
        require(method.getModifiers() == (Modifier.PRIVATE | Modifier.STATIC));
        require(method.getReturnType() == RuntimeException.class && Arrays.equals(method.getParameterTypes(), new Class<?>[]{Throwable.class}));
        require(method.getTypeParameters().length == 1 && Arrays.equals(method.getExceptionTypes(), new Class<?>[]{Throwable.class}));
        TypeVariable<?> variable = method.getTypeParameters()[0];
        require(Arrays.equals(variable.getBounds(), new Type[]{Throwable.class}));
        require(Arrays.equals(method.getGenericExceptionTypes(), new Type[]{variable}));
        require(method.getDeclaredAnnotations().length == 0);
    }
    static void identity() throws Exception {
        require(Owner.class.getModifiers() == Modifier.PUBLIC && Owner.class.getSuperclass() == Base.class);
        require(Owner.class.getTypeParameters().length == 0 && Owner.class.getDeclaredFields().length == 0);
        int originals = 0;
        for (Method method : Owner.class.getDeclaredMethods()) {
            if (method.getName().equals("__neverdThrow")) { throwHelper(method); continue; }
            require(method.getReturnType() == void.class && !method.isBridge() && method.getExceptionTypes().length == 0);
            require(method.getDeclaredAnnotations().length == 0);
            if (method.getName().equals("accept")) {
                require(method.getModifiers() == Modifier.PRIVATE && !method.isSynthetic());
                require(Arrays.equals(method.getParameterTypes(), new Class<?>[]{Object.class}));
                require(Arrays.equals(method.getGenericParameterTypes(), new Type[]{Object.class}));
                TypeVariable<?>[] variables = method.getTypeParameters();
                require(variables.length == 1 && variables[0].getGenericDeclaration().equals(method));
                require(Arrays.equals(variables[0].getBounds(), new Type[]{First.class, Second.class}));
            } else {
                require(method.getName().equals("access$000"));
                // Ordinary source accessors disclose only loss of this synthetic bit.
                require((method.getModifiers() & ~0x1000) == Modifier.STATIC);
                require(method.getTypeParameters().length == 0);
                require(Arrays.equals(method.getParameterTypes(), new Class<?>[]{Owner.class, Object.class}));
            }
            ++originals;
        }
        require(originals == 2 && Owner.class.getDeclaredConstructors().length == 1);
        require(Owner.Dispatch.class.getModifiers() == (Modifier.PUBLIC | Modifier.STATIC));
        require(Owner.Dispatch.class.getEnclosingClass() == Owner.class && Owner.Dispatch.class.isMemberClass());
        require(First.class.isInterface() && Second.class.isInterface());
        require(First.class.getTypeParameters().length == 0 && Second.class.getTypeParameters().length == 0);
    }
    public static void main(String[] args) throws Throwable {
        final int route = Integer.parseInt(args[0]), mode = Integer.parseInt(args[1]);
        final boolean future = Boolean.parseBoolean(args[2]);
        identity();
        final Method entry = Owner.Dispatch.class.getDeclaredMethod("call", Owner.class, Object.class);
        final Method helper = Owner.class.getDeclaredMethod("access$000", Owner.class, Object.class);
        helper.setAccessible(true);
        final MethodHandle handle = MethodHandles.lookup().findStatic(Owner.Dispatch.class, "call",
                MethodType.methodType(void.class, Owner.class, Object.class));
        final Owner first = future ? new FutureOwner() : new Owner(), other = new Owner();
        for (int attempt = 0; attempt < 2; ++attempt) {
            final Object input = mode == 1 ? null : new FutureInput(), replacement = new FutureInput(), marker = new FutureInput();
            final CharSequence textMarker = new StringBuilder("marker"), result = mode == 11 ? null : new StringBuilder("after" + attempt);
            final Throwable selected = mode == 4 || mode == 7 ? new IllegalStateException()
                    : mode == 5 || mode == 8 ? new AssertionError() : new java.io.IOException();
            final int[] calls = {0, 0, 0, 0};
            final boolean supplierFailure = mode == 12 || mode == 13 || mode == 17;
            final boolean body = !supplierFailure && mode != 2;
            final boolean applying = body && !(mode >= 3 && mode <= 5 || mode == 9);
            final boolean complete = applying && !(mode >= 6 && mode <= 8 || mode == 10);
            final String prefix = "78" + (route == 3 ? "" : "1");
            Trace.events = 0; Trace.renderOverrides = 0; Trace.applyOverrides = 0;
            Trace.current = mode == 2 || mode == 17 ? null : first; Trace.input = input;
            Trace.receiver = null; Trace.inputSeen = marker; Trace.output = new StringBuilder("before");
            Trace.textSeen = textMarker; Trace.flag = true;
            Trace.receiverAction = new Runnable() { public void run() {
                require(++calls[0] == 1 && Trace.events == 7);
                if (mode == 12) UnusedPrivateIntersectionHarness.<RuntimeException>raise(selected);
            }};
            Trace.argumentAction = new Runnable() { public void run() {
                require(++calls[1] == 1 && Trace.events == 78);
                if (mode == 13 || mode == 17) UnusedPrivateIntersectionHarness.<RuntimeException>raise(selected);
                if (mode == 14) { Trace.current = other; Trace.input = replacement; }
            }};
            Trace.before = mode == 9 ? null : new Runnable() { public void run() {
                require(++calls[2] == 1 && Trace.events == Long.parseLong(prefix + "23"));
                require(Trace.receiver == first && Trace.inputSeen == (mode == 14 ? replacement : input));
                require(Trace.textSeen == textMarker && Trace.flag);
                Trace.output = result;
                if (mode == 15 || mode == 16) { Trace.current = other; Trace.input = replacement; }
                if (mode >= 3 && mode <= 5) UnusedPrivateIntersectionHarness.<RuntimeException>raise(selected);
            }};
            Trace.after = mode == 10 ? null : new Runnable() { public void run() {
                require(++calls[3] == 1 && Trace.events == Long.parseLong(prefix + "234"));
                require(Trace.receiver == first && Trace.textSeen == result && !Trace.flag);
                if (mode == 16) Trace.output = new StringBuilder("later");
                if (mode >= 6 && mode <= 8) UnusedPrivateIntersectionHarness.<RuntimeException>raise(selected);
            }};
            Throwable failure = null;
            try { UnusedPrivateIntersectionCaller.call(route, entry, helper, handle); }
            catch (Throwable caught) { failure = caught; }
            String events = mode == 12 ? "7" : supplierFailure ? "78" : prefix;
            if (body) events += applying ? complete ? "23456" : "234" : "23";
            require(Trace.events == Long.parseLong(events));
            require(calls[0] == 1 && calls[1] == (mode == 12 ? 0 : 1));
            require(calls[2] == (body && mode != 9 ? 1 : 0) && calls[3] == (applying && mode != 10 ? 1 : 0));
            require(Trace.renderOverrides == (body && future ? 1 : 0) && Trace.applyOverrides == (applying && future ? 1 : 0));
            require(Trace.receiver == (body ? first : null));
            require(Trace.inputSeen == (body ? mode == 14 ? replacement : input : marker));
            require(Trace.textSeen == (applying ? result : textMarker) && Trace.flag == !applying);
            if (complete) require(failure == null);
            else if (supplierFailure || body && mode >= 3 && mode <= 8) require(failure == selected);
            else require(failure instanceof NullPointerException);
            identity();
        }
        System.out.println("attempts=2,unused-private-intersection=matched");
    }
}
