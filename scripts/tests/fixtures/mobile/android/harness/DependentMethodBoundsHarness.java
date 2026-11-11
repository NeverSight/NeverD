import fixture.*;
import java.lang.invoke.*;
import java.lang.reflect.*;

@SuppressWarnings({"rawtypes", "unchecked"})
public final class DependentMethodBoundsHarness {
    static int identities, states, failures;
    static final Method concrete, parent;
    static final MethodHandle concreteHandle, parentHandle;
    static {
        try {
            concrete = Core.class.getMethod("copy", Object[].class, Object.class, Sink.class);
            parent = Api.class.getMethod("copy", Object[].class, Object.class, Sink.class);
            concreteHandle = MethodHandles.publicLookup().unreflect(concrete);
            parentHandle = MethodHandles.publicLookup().unreflect(parent);
        } catch (Exception error) { throw new AssertionError(error); }
    }
    static void same(Object actual, Object expected) {
        if (actual != expected) throw new AssertionError("original dependent value identity changed");
        identities++;
    }
    static void state(int actual, int expected) {
        if (actual != expected) throw new AssertionError("original callback effects changed");
        states++;
    }
    static Object[] reflected(Method method, Core self, Object[] input, Object tag, Sink sink) throws Throwable {
        try { return (Object[]) method.invoke(self, input, tag, sink); }
        catch (InvocationTargetException error) { throw error.getCause(); }
    }
    static Object[] call(int route, Core<Object, String> self, Object[] input, Object tag, Sink<Object> sink) throws Throwable {
        switch (route) {
        case 0: return self.copy(input, tag, sink);
        case 1: return ((Api<Object>) self).copy(input, tag, sink);
        case 2: return ((Api) self).copy(input, tag, sink);
        case 3: return reflected(concrete, self, input, tag, sink);
        case 4: return reflected(parent, self, input, tag, sink);
        case 5: return (Object[]) concreteHandle.invoke(self, input, tag, sink);
        case 6: return (Object[]) parentHandle.invoke(self, input, tag, sink);
        default: return DependentMethodBoundsCaller.call(self, input, tag, sink);
        }
    }
    interface Run { void call() throws Throwable; }
    static void failure(Run action, Class<?> type, Throwable original) throws Throwable {
        try { action.call(); throw new AssertionError("missing original failure"); }
        catch (Throwable error) {
            if (error.getClass() != type) throw new AssertionError("failure order changed", error);
            if (original != null) same(error, original);
            failures++;
        }
    }
    static void reflection(Class<?> owner, Method method) {
        TypeVariable<?> root = owner.getTypeParameters()[0];
        TypeVariable<?>[] formals = method.getTypeParameters();
        if (formals.length != 2 || formals[0].getBounds().length != 1 || formals[1].getBounds().length != 1
                || !formals[0].getBounds()[0].equals(root) || !formals[1].getBounds()[0].equals(formals[0])
                || !formals[0].getGenericDeclaration().equals(method) || !formals[1].getGenericDeclaration().equals(method)
                || !((GenericArrayType) method.getGenericParameterTypes()[0]).getGenericComponentType().equals(formals[0])
                || !method.getGenericParameterTypes()[1].equals(formals[1])
                || !((GenericArrayType) method.getGenericReturnType()).getGenericComponentType().equals(formals[0]))
            throw new AssertionError("resolved class and method bound scopes changed");
        WildcardType callback = (WildcardType) ((ParameterizedType) method.getGenericParameterTypes()[2]).getActualTypeArguments()[0];
        if (callback.getLowerBounds().length != 1 || !callback.getLowerBounds()[0].equals(formals[0]))
            throw new AssertionError("complete callback bound changed");
    }
    public static void main(String[] args) throws Throwable {
        reflection(Core.class, concrete); reflection(Api.class, parent);
        for (int route = 0; route < 8; route++) {
            Core<Object, String> self = new Core<>();
            for (Object[] input : new Object[][]{new String[]{"owned"}, new Integer[]{17}, new Object[]{new Object()}, null})
                for (boolean nil : new boolean[]{false, true}) {
                    Object tag = nil || input == null ? null : input[0];
                    Object[] observed = new Object[2]; int[] calls = {0}; int before = self.calls;
                    Sink<Object> sink = (value, array) -> {
                        if (self.calls != before + 1 || self.seen != value) throw new AssertionError("callback preceded original state");
                        calls[0]++; observed[0] = value; observed[1] = array;
                    };
                    same(call(route, self, input, tag, sink), input);
                    same(self.seen, tag); same(observed[0], tag); same(observed[1], input);
                    same(self.value(tag), tag); state(self.calls, before + 1); state(calls[0], 1);
                }
            final int selected = route; Object tag = new Object(); Object[] input = {tag};
            int[] callbacks = {0}; Sink<Object> sink = (value, array) -> callbacks[0]++;
            RuntimeException problem = new IllegalStateException("owned dependent bound failure");
            int before = self.calls; self.failure = problem;
            failure(() -> call(selected, self, input, tag, sink), problem.getClass(), problem);
            state(self.calls, before + 1); state(callbacks[0], 0); same(self.seen, tag);
            self.failure = null;
            Sink<Object> throwing = (value, array) -> { callbacks[0]++; throw problem; };
            failure(() -> call(selected, self, input, tag, throwing), problem.getClass(), problem);
            state(self.calls, before + 2); state(callbacks[0], 1); same(self.seen, tag);
            failure(() -> call(selected, null, input, tag, sink), NullPointerException.class, null);
            state(self.calls, before + 2); state(callbacks[0], 1);
        }
        System.out.println("identities=" + identities + ",states=" + states + ",failures=" + failures);
        System.out.println("dependent-class-and-method-bounds-callback-variance-and-original-effects=retained");
    }
}
