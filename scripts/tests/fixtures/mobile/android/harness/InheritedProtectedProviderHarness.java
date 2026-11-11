import fixture.*;
import java.lang.invoke.*;
import java.lang.reflect.*;
@SuppressWarnings({"rawtypes", "unchecked"})
public final class InheritedProtectedProviderHarness {
    public static class External extends Provider<Object, NarrowBox<Object>> {
        protected Box<Object> read(NarrowBox<Object> input) {
            calls += 10; seen = input;
            if (failure != null) throw failure;
            return storage;
        }
    }
    static int values, identities, states, failures;
    static final Method typed, erased;
    static final MethodHandle typedHandle, erasedHandle;
    static {
        try {
            typed = Provider.class.getDeclaredMethod("read", Box.class); typed.setAccessible(true);
            erased = Provider.class.getMethod("read", Object.class);
            typedHandle = MethodHandles.lookup().unreflect(typed);
            erasedHandle = MethodHandles.publicLookup().unreflect(erased);
        } catch (Exception error) { throw new AssertionError(error); }
    }
    static void value(boolean ok) { if (!ok) throw new AssertionError("value"); values++; }
    static void same(Object a, Object b) { if (a != b) throw new AssertionError("identity"); identities++; }
    static void state(boolean ok) { if (!ok) throw new AssertionError("state"); states++; }
    static Object call(int route, Provider self, Object input) throws Throwable {
        try {
            switch (route) {
            case 0: return InheritedProtectedProviderCaller.erased(self, input);
            case 1: return InheritedProtectedProviderCaller.typed(self, (Box)input);
            case 2: return erased.invoke(self, input);
            case 3: return erasedHandle.invoke(self, input);
            case 4: return typed.invoke(self, input);
            default: return typedHandle.invoke(self, input);
            }
        } catch (InvocationTargetException error) { throw error.getCause(); }
    }
    public static void main(String[] args) throws Throwable {
        boolean rebuilt = args.length != 0 && args[0].equals("rebuilt");
        value(java.util.Arrays.stream(Core.class.getDeclaredMethods()).allMatch(method ->
            method.getName().equals("__neverdThrow") && Modifier.isPrivate(method.getModifiers())
            && Modifier.isStatic(method.getModifiers())));
        value(typed.getModifiers() == (rebuilt ? Modifier.PUBLIC : Modifier.PROTECTED));
        value(erased.isBridge() && erased.isSynthetic() && Modifier.isPublic(erased.getModifiers()));
        TypeVariable<?>[] formals = Provider.class.getTypeParameters();
        value(typed.getGenericParameterTypes()[0].equals(formals[1]));
        value(((ParameterizedType)typed.getGenericReturnType()).getActualTypeArguments()[0].equals(formals[0]));
        Object marker = new Object(), item = new Object();
        NarrowBox<Object> narrow = new NarrowBox<Object>(); narrow.value = item;
        Box<Object> plain = new Box<Object>(); plain.value = marker;
        RuntimeException problem = new IllegalStateException("owned failure");
        for (int route = 0; route < 6; route++)
            for (int kind = 0; kind < 3; kind++)
                for (Box<Object> input : new Box[]{null, narrow, plain})
                    for (boolean fail : new boolean[]{false, true}) {
                        Provider self = kind == 0 ? new Core<Object>() : kind == 1 ? new External() : null;
                        if (self != null) { self.seen = marker; self.storage = input; self.failure = fail ? problem : null; }
                        Throwable error = null; Object result = null;
                        try { result = call(route, self, input); } catch (Throwable caught) { error = caught; }
                        if (self == null) { value(error instanceof NullPointerException); failures++; }
                        else {
                            boolean cast = kind == 1 && input == plain;
                            state(self.calls == (cast ? 0 : kind == 1 ? 10 : 1));
                            state(self.visits == (route == 0 ? 1 : 0));
                            same(self.seen, cast ? marker : input);
                            if (cast) { value(error instanceof ClassCastException); failures++; }
                            else if (fail) { same(error, problem); failures++; }
                            else { same(error, null); same(result, input); }
                        }
                        same(narrow.value, item); same(plain.value, marker);
                    }
        for (int route : new int[]{0, 2, 3}) {
            Provider self = new Core<Object>(); self.seen = marker;
            Throwable error = null;
            try { call(route, self, new Object()); } catch (Throwable caught) { error = caught; }
            value(error instanceof ClassCastException); failures++;
            state(self.calls == 0 && self.visits == (route == 0 ? 1 : 0)); same(self.seen, marker);
        }
        System.out.println("values="+values+",identities="+identities+",states="+states+",failures="+failures);
        System.out.println("inherited-bounded-provider-and-external-protected-override=retained,no-child-virtual-methods-added");
    }
}
