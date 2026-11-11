import fixture.*;
import java.lang.invoke.*;
import java.lang.reflect.*;
@SuppressWarnings({"rawtypes", "unchecked"})
public final class InheritedFinalProtectedHarness {
    static int values, identities, states, failures;
    static final Method typed, erased;
    static final MethodHandle typedHandle, erasedHandle;
    static {
        try {
            typed = Provider.class.getDeclaredMethod("insert", Box.class, int.class, Object.class);
            typed.setAccessible(true);
            erased = Provider.class.getMethod("insert", Object.class, int.class, Object.class);
            typedHandle = MethodHandles.lookup().unreflect(typed);
            erasedHandle = MethodHandles.publicLookup().unreflect(erased);
        } catch (Exception error) { throw new AssertionError(error); }
    }
    static void value(boolean ok) { if (!ok) throw new AssertionError("value or scope"); values++; }
    static void same(Object a, Object b) { if (a != b) throw new AssertionError("identity"); identities++; }
    static void state(boolean ok) { if (!ok) throw new AssertionError("state"); states++; }
    static void call(int route, Core self, Object box, int index, Object item) throws Throwable {
        try {
            switch (route) {
            case 0: self.typed((Box)box, index, item); break;
            case 1: ((Base)self).dispatch(box, index, item); break;
            case 2: InheritedFinalProtectedCaller.call(self, box, index, item); break;
            case 3: erased.invoke(self, box, index, item); break;
            case 4: erasedHandle.invokeExact((Provider)self, box, index, item); break;
            case 5: typed.invoke(self, box, index, item); break;
            case 6: typedHandle.invokeExact((Provider)self, (Box)box, index, item); break;
            default: InheritedFinalProtectedCaller.typed(self, (Box)box, index, item);
            }
        } catch (InvocationTargetException error) { throw error.getCause(); }
    }
    interface Action { void run() throws Throwable; }
    static void failure(Action action, Class<?> kind) throws Throwable {
        try { action.run(); throw new AssertionError("missing failure"); }
        catch (Throwable problem) { if (problem.getClass() != kind) throw new AssertionError("failure", problem); failures++; }
    }
    public static void main(String[] args) throws Throwable {
        boolean rebuilt = args.length != 0 && args[0].equals("rebuilt");
        TypeVariable<?> e = Provider.class.getTypeParameters()[0];
        value(typed.getTypeParameters().length == 0);
        value(typed.getGenericParameterTypes()[0].equals(Provider.class.getTypeParameters()[1]));
        value(typed.getGenericParameterTypes()[2].equals(e));
        value(typed.getModifiers() == ((rebuilt ? Modifier.PUBLIC : Modifier.PROTECTED) | Modifier.FINAL));
        value(erased.isBridge() && erased.isSynthetic() && Modifier.isPublic(erased.getModifiers()));
        value(java.util.Arrays.stream(Core.class.getDeclaredMethods()).allMatch(method ->
            method.getName().equals("__neverdThrow") && Modifier.isPrivate(method.getModifiers())
            && Modifier.isStatic(method.getModifiers())));
        Object first = new Object(), second = new Object(), sentinel = new Object();
        RuntimeException problem = new IllegalStateException("owned failure");
        for (int route = 0; route < 8; route++) {
            for (boolean nil : new boolean[]{false, true})
                for (Object item : new Object[]{null, first, second})
                    for (int index : new int[]{Integer.MIN_VALUE, -1, 0, Integer.MAX_VALUE})
                        for (boolean fail : new boolean[]{false, true}) {
                            Core<Object> self = new Core<Object>();
                            Box<Object> box = nil ? null : new Box<Object>();
                            if (box != null) { box.value = sentinel; box.index = 13; }
                            self.failure = fail ? problem : null;
                            Throwable error = null;
                            try { call(route, self, box, index, item); } catch (Throwable caught) { error = caught; }
                            state(self.calls == 1 && self.visits == (route == 1 || route == 2 ? 1 : 0));
                            same(self.last, item);
                            if (fail) { same(error, problem); failures++; }
                            else if (nil) { value(error instanceof NullPointerException); failures++; }
                            else same(error, null);
                            if (box != null) {
                                same(box.value, fail ? sentinel : item);
                                value(box.index == (fail ? 13 : index));
                            }
                        }
            final int selected = route;
            failure(() -> call(selected, null, null, 0, first), NullPointerException.class);
        }
        for (int route : new int[]{1, 2, 3, 4}) {
            final int selected = route; Core<Object> self = new Core<Object>();
            failure(() -> call(selected, self, new Object(), 7, first), ClassCastException.class);
            state(self.calls == 0 && self.visits == (route == 1 || route == 2 ? 1 : 0));
            same(self.last, null);
        }
        System.out.println("values="+values+",identities="+identities+",states="+states+",failures="+failures);
        System.out.println("inherited-final-generic-provider-and-original-erased-dispatch=retained,typed-access-change=reported");
    }
}
