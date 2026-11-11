import fixture.*;
import java.lang.invoke.*;
import java.lang.reflect.*;
import java.util.Arrays;
@SuppressWarnings({"rawtypes", "unchecked"})
public final class ProtectedScopedResultHarness {
    static int values, identities, states, failures;
    static final Method[] typed = new Method[4], erased = new Method[4];
    static final MethodHandle[] typedHandle = new MethodHandle[4], erasedHandle = new MethodHandle[4];
    static {
        try {
            String[] names = {"make", "size", "copy", "narrow"};
            for (int i = 0; i < 4; i++) {
                Class<?>[] tp = i == 0 ? new Class<?>[0] : new Class<?>[]{Box.class};
                Class<?>[] ep = i == 0 ? tp : new Class<?>[]{i == 3 ? Box.class : Object.class};
                typed[i] = find(names[i], tp, i == 1 ? int.class : Box.class);
                erased[i] = find(names[i], ep, i == 1 ? int.class : Object.class);
                typed[i].setAccessible(true);
                typedHandle[i] = MethodHandles.lookup().unreflect(typed[i]);
                erasedHandle[i] = MethodHandles.publicLookup().unreflect(erased[i]);
            }
        } catch (Exception error) { throw new AssertionError(error); }
    }
    static Method find(String name, Class<?>[] args, Class<?> result) throws Exception {
        for (Method method : Core.class.getDeclaredMethods())
            if (method.getName().equals(name) && Arrays.equals(method.getParameterTypes(), args) && method.getReturnType() == result) return method;
        throw new NoSuchMethodException(name);
    }
    static void value(boolean ok) { if (!ok) throw new AssertionError("value"); values++; }
    static void same(Object a, Object b) { if (a != b) throw new AssertionError("identity"); identities++; }
    static void state(boolean ok) { if (!ok) throw new AssertionError("state"); states++; }
    static Object call(int kind, int route, Core self, Object input) throws Throwable {
        try {
            if (route == 0) {
                switch (kind) {
                case 0: return self.callMake();
                case 1: return ((Base)self).callSize(input);
                case 2: return ((Base)self).callCopy(input);
                default: return self.callNarrow((Box)input);
                }
            }
            if (route == 1) return ProtectedScopedResultCaller.call(kind, self, (Box)input);
            if (route == 2 || route == 4) return (route == 2 ? erased[kind] : typed[kind]).invoke(self, kind == 0 ? new Object[0] : new Object[]{input});
            MethodHandle handle = route == 3 ? erasedHandle[kind] : typedHandle[kind];
            return kind == 0 ? handle.invokeWithArguments(self) : handle.invokeWithArguments(self, input);
        } catch (InvocationTargetException error) { throw error.getCause(); }
    }
    interface Action { void run() throws Throwable; }
    static void failure(Action action, Class<?> kind) throws Throwable {
        try { action.run(); throw new AssertionError("missing failure"); }
        catch (Throwable problem) { if (problem.getClass() != kind) throw new AssertionError("failure", problem); failures++; }
    }
    public static void main(String[] args) throws Throwable {
        boolean rebuilt = args.length != 0 && args[0].equals("rebuilt");
        for (int kind = 0; kind < 4; kind++) {
            value(typed[kind].getModifiers() == (rebuilt ? Modifier.PUBLIC : Modifier.PROTECTED));
            value(erased[kind].isBridge() && erased[kind].isSynthetic() && Modifier.isPublic(erased[kind].getModifiers()));
        }
        Object a = new Object(), b = new Object();
        Box<Object> first = new Box<Object>(), second = new Box<Object>(), sentinel = new Box<Object>();
        first.value = a; first.count = Integer.MIN_VALUE;
        second.value = b; second.count = Integer.MAX_VALUE;
        RuntimeException original = new IllegalStateException("owned failure");
        for (int kind = 0; kind < 4; kind++)
            for (int route = 0; route < 6; route++)
                for (boolean nil : new boolean[]{false, true})
                    for (Box<Object> input : new Box[]{null, first, second})
                        for (boolean fail : new boolean[]{false, true}) {
                            Core<Object> self = nil ? null : new Core<Object>();
                            if (self != null) { self.seen = sentinel; self.storage = input; self.failure = fail ? original : null; }
                            Throwable error = null; Object result = null;
                            try { result = call(kind, route, self, input); } catch (Throwable caught) { error = caught; }
                            if (nil) { state(error instanceof NullPointerException); failures++; }
                            else {
                                state(self.calls == 1 && self.operation == kind); same(self.seen, kind == 0 ? sentinel : input);
                                if (fail) { same(error, original); failures++; }
                                else if (kind == 1 && input == null) { state(error instanceof NullPointerException); failures++; }
                                else {
                                    state(error == null);
                                    if (kind == 1) value(((Integer)result).intValue() == input.count);
                                    else same(result, input);
                                }
                            }
                            same(first.value, a); same(second.value, b);
                            value(first.count == Integer.MIN_VALUE); value(second.count == Integer.MAX_VALUE);
                        }
        for (int kind : new int[]{1, 2})
            for (int route : new int[]{2, 3}) {
                final int selectedKind = kind, selectedRoute = route;
                Core<Object> self = new Core<Object>(); self.seen = sentinel;
                failure(() -> call(selectedKind, selectedRoute, self, new Object()), ClassCastException.class);
                state(self.calls == 0); same(self.seen, sentinel);
            }
        System.out.println("values="+values+",identities="+identities+",states="+states+",failures="+failures);
        System.out.println("scoped-protected-results-parameters-and-original-aliases=retained,typed-access-changes=reported");
    }
}
