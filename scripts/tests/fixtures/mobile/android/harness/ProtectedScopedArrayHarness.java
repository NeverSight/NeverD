import fixture.*;
import java.lang.invoke.*;
import java.lang.reflect.*;
public final class ProtectedScopedArrayHarness {
    static int values, identities, states, failures;
    static final Method typed, erased;
    static final MethodHandle typedHandle, erasedHandle;
    static {
        try {
            typed = Core.class.getDeclaredMethod("convert", Object[].class); typed.setAccessible(true);
            erased = Core.class.getMethod("convert", Object.class);
            typedHandle = MethodHandles.lookup().unreflect(typed); erasedHandle = MethodHandles.publicLookup().unreflect(erased);
        } catch (Exception error) { throw new AssertionError(error); }
    }
    static void value(boolean ok) { if (!ok) throw new AssertionError("value"); values++; }
    static void same(Object a, Object b) { if (a != b) throw new AssertionError("identity"); identities++; }
    static void state(boolean ok) { if (!ok) throw new AssertionError("state"); states++; }
    static Object call(int route, Core self, Object input) throws Throwable {
        try {
            switch (route) {
            case 0: return ProtectedScopedArrayCaller.erased(self, input);
            case 1: return ProtectedScopedArrayCaller.typed(self, (Object[])input);
            case 2: return erased.invoke(self, input);
            case 3: return erasedHandle.invoke(self, input);
            case 4: return typed.invoke(self, input);
            default: return typedHandle.invoke(self, input);
            }
        } catch (InvocationTargetException error) { throw error.getCause(); }
    }
    public static void main(String[] args) throws Throwable {
        boolean rebuilt = args.length != 0 && args[0].equals("rebuilt");
        value(typed.getModifiers() == (rebuilt ? Modifier.PUBLIC : Modifier.PROTECTED));
        value(erased.isBridge() && erased.isSynthetic() && Modifier.isPublic(erased.getModifiers()));
        Type[] parent = ((ParameterizedType)Core.class.getGenericSuperclass()).getActualTypeArguments();
        same(((GenericArrayType)parent[0]).getGenericComponentType(), Core.class.getTypeParameters()[0]); same(parent[1], Core.class.getTypeParameters()[0]);
        RuntimeException problem = new IllegalStateException("owned failure"); Object marker = new Object();
        Result sentinel = new Result(new Object[]{marker});
        for (int route=0; route<6; route++)
            for (boolean nullSelf : new boolean[]{false, true})
                for (Object[] input : new Object[][]{null, new Object[0], new Object[]{marker, "owned", null}})
                    for (boolean fail : new boolean[]{false, true}) {
                        Core self = nullSelf ? null : new Core();
                        if (self != null) { self.seen = marker; self.last = sentinel; self.failure = fail ? problem : null; }
                        int before = Result.created; Object result = null; Throwable error = null;
                        try { result = call(route, self, input); } catch (Throwable caught) { error = caught; }
                        state(Result.created == before + (!nullSelf && !fail ? 1 : 0));
                        if (self == null) { value(error instanceof NullPointerException); failures++; }
                        else {
                            state(self.calls == 1 && self.visits == (route == 0 ? 1 : 0)); same(self.seen, input);
                            if (fail) { same(error, problem); same(self.last, sentinel); failures++; }
                            else if (input == null) { value(error instanceof NullPointerException); same(self.last, sentinel); failures++; }
                            else {
                                same(error, null); same(result, self.last); same(((Result)result).data, input);
                                value(((Result)result).length == input.length);
                                if (input.length != 0) { ((Result)result).data[0] = problem; same(input[0], problem); }
                            }
                        }
                    }
        for (int route : new int[]{0, 2, 3})
            for (Object wrong : new Object[]{new Object(), new byte[0], new long[0]}) {
                Core self = new Core(); self.seen = marker; int before = Result.created; Throwable error = null;
                try { call(route, self, wrong); } catch (Throwable caught) { error = caught; }
                value(error instanceof ClassCastException); failures++;
                state(self.calls == 0 && self.visits == (route == 0 ? 1 : 0) && Result.created == before);
                same(self.seen, marker);
            }
        System.out.println("values="+values+",identities="+identities+",states="+states+",failures="+failures);
        System.out.println("scoped-array-input-return-alias-and-constructor-order=retained,typed-access-change=reported");
    }
}
