package probe;
import fixture.*;
import java.lang.invoke.*;
import java.lang.reflect.*;
public final class IntersectionSuperForwardingHarness {
    static void require(boolean ok) {
        if (!ok) throw new AssertionError("owned intersection super forwarding mismatch");
    }
    @SuppressWarnings("unchecked")
    static <E extends Throwable> void raise(Throwable error) throws E { throw (E) error; }
    public static void main(String[] args) throws Throwable {
        int route = Integer.parseInt(args[0]), mode = Integer.parseInt(args[1]);
        boolean future = Boolean.parseBoolean(args[2]);
        Method entry = Owner.class.getMethod("accept", First.class);
        for (Class<?> owner : new Class<?>[]{Base.class, Owner.class}) {
            Method method = owner.getDeclaredMethod("accept", First.class);
            TypeVariable<?>[] variables = method.getTypeParameters();
            require(variables.length == 1 && variables[0].getGenericDeclaration().equals(method));
            require(java.util.Arrays.equals(variables[0].getBounds(), new Type[]{First.class, Second.class}));
            require(java.util.Arrays.equals(method.getGenericParameterTypes(), variables));
            require(method.getReturnType() == void.class && method.getExceptionTypes().length == 0);
            require(method.getModifiers() == Modifier.PUBLIC && !method.isBridge() && !method.isSynthetic());
        }
        int entries = 0;
        for (Method method : Owner.class.getDeclaredMethods())
            if (!method.getName().equals("__neverdThrow")) ++entries;
        require(entries == 1);
        MethodHandle handle = MethodHandles.lookup().findVirtual(Owner.class,"accept",MethodType.methodType(void.class,First.class));
        Owner receiver = future ? new IntersectionSuperForwardingCaller.Future() : new Owner();
        for (int attempt = 0; attempt < 2; ++attempt) {
            final Throwable failure = mode == 3 || mode == 6 ? new java.io.IOException()
                : mode == 4 || mode == 7 ? new AssertionError() : new IllegalStateException();
            final IntersectionSuperForwardingCaller.Value normal = mode == 1 ? null : new IntersectionSuperForwardingCaller.Value();
            final First value = mode == 8 ? new First() { } : normal;
            final int[] before = {0}, after = {0};
            Trace.events = 0; Trace.receiver = null; Trace.value = null;
            Trace.before = new Runnable() { public void run() {
                require(++before[0] == 1 && Trace.receiver == receiver && Trace.value == value);
                require(Trace.events == (future ? 12 : 2));
                if (mode >= 2 && mode <= 4) IntersectionSuperForwardingHarness.<RuntimeException>raise(failure);
                if (mode == 9) Trace.value = null;
            } };
            Trace.after = new Runnable() { public void run() {
                require(++after[0] == 1 && before[0] == 1 && Trace.receiver == receiver);
                require(Trace.value == (mode == 9 ? null : value));
                if (mode >= 5 && mode <= 7) IntersectionSuperForwardingHarness.<RuntimeException>raise(failure);
            } };
            Throwable caught = null;
            try {
                if (route == 0 && mode != 8) receiver.accept(normal);
                else if (route == 1 && mode != 8) IntersectionSuperForwardingCaller.call(receiver, normal);
                else if (route == 3) handle.invokeExact(receiver, value);
                else try { entry.invoke(receiver, value); }
                     catch (InvocationTargetException wrapped) { throw wrapped.getCause(); }
            } catch (Throwable error) { caught = error; }
            String body = mode >= 2 && mode <= 4 ? "2" : mode == 8 ? "23"
                : mode >= 5 && mode <= 7 ? "23456" : mode == 1 ? "23567" : "234567";
            boolean completed = mode < 2 || mode == 9;
            String expected = (future ? "1" : "") + body + (future && completed ? "8" : "");
            require(Trace.events == Long.parseLong(expected));
            require(before[0] == 1 && after[0] == (mode >= 2 && mode <= 4 || mode == 8 ? 0 : 1));
            require(Trace.receiver == receiver && Trace.value == (mode == 9 ? null : value));
            if (mode >= 2 && mode <= 7) require(caught == failure);
            else if (mode == 8) require(caught instanceof ClassCastException);
            else require(caught == null);
        }
        System.out.println("attempts=2,intersection-super-forwarding=matched");
    }
}
