import fixture.*;
import java.lang.invoke.*;
import java.lang.reflect.*;

public final class InheritedDescriptorDefaultHarness {
    static void require(boolean ok) {
        if (!ok) throw new AssertionError("owned inherited descriptor default");
    }
    static Object call(int route, Child receiver, Object value) throws Throwable {
        if (route == 0) return receiver.find(value);
        if (route == 1) return InheritedDescriptorDefaultCaller.call(receiver, value);
        if (route == 2) {
            Method method = Child.class.getDeclaredMethod("access$find$jd", Child.class, Object.class);
            try { return method.invoke(null, receiver, value); }
            catch (InvocationTargetException failure) { throw failure.getCause(); }
        }
        MethodHandle handle = MethodHandles.publicLookup().findStatic(
            Child.class, "access$find$jd", MethodType.methodType(Object.class, Child.class, Object.class));
        return (Object) handle.invokeExact(receiver, value);
    }
    public static void main(String[] args) throws Throwable {
        int route = Integer.parseInt(args[0]), variant = Integer.parseInt(args[1]), mode = Integer.parseInt(args[2]);
        require(Parent.class.getTypeParameters().length == 0 && Child.class.getTypeParameters().length == 0);
        require(Child.class.getInterfaces().length == 1 && Child.class.getInterfaces()[0] == Parent.class);
        Method original = Parent.class.getMethod("find", Object.class);
        require(original.isDefault() && original.getReturnType() == Object.class);
        for (Method method : Parent.class.getDeclaredMethods()) {
            if (Modifier.isStatic(method.getModifiers())) {
                require(method.getName().equals("__neverdThrow") && Modifier.isPrivate(method.getModifiers())
                    && method.getReturnType() == RuntimeException.class
                    && java.util.Arrays.equals(method.getParameterTypes(), new Class<?>[] { Throwable.class }));
            }
        }
        for (int attempt = 0; attempt < 2; attempt++) {
            Child receiver = mode == 4 ? null : variant == 0 ? new Plain() : new Receiver();
            Object value = mode == 0 ? null : mode == 1 ? receiver : mode == 2 ? new Object[2] : new Object();
            Object result = mode == 3 ? null : mode == 10 ? value : new Object();
            Trace.events = Trace.overrides = 0;
            Trace.receiver = Trace.input = null;
            Trace.result = result;
            Trace.failAt = mode >= 5 && mode <= 7 ? mode - 4 : 0;
            Trace.failure = new RuntimeException("owned inherited failure");
            Object actual = null;
            Throwable caught = null;
            try { actual = call(route, receiver, value); } catch (Throwable failure) { caught = failure; }
            boolean virtual = receiver != null && route == 0 && variant == 1;
            int expected = receiver == null ? 0 : virtual ? 9 : Trace.failAt == 1 ? 1 : Trace.failAt == 2 ? 12 : 123;
            if (receiver == null) require(caught instanceof NullPointerException);
            else if (!virtual && Trace.failAt != 0) require(caught == Trace.failure);
            else require(caught == null && actual == (virtual ? value : result));
            require(Trace.events == expected && Trace.overrides == (virtual ? 1 : 0));
            if (receiver == null) require(Trace.receiver == null && Trace.input == null);
            else require(Trace.receiver == receiver && Trace.input == value);
            System.out.println("route=" + route + ",variant=" + variant + ",mode=" + mode + ",attempt=" + attempt + ",events=" + expected);
        }
    }
}
