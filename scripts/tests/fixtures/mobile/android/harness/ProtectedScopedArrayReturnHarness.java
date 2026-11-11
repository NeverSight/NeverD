import fixture.*;
import java.lang.invoke.*;
import java.lang.reflect.*;
public final class ProtectedScopedArrayReturnHarness {
    static int values, identities, states, failures;
    static final Method typed, erased;
    static final MethodHandle typedHandle, erasedHandle;
    static {
        try {
            typed = Core.class.getDeclaredMethod("convert", Result.class); typed.setAccessible(true);
            erased = Core.class.getMethod("convert", Object.class);
            typedHandle = MethodHandles.lookup().unreflect(typed); erasedHandle = MethodHandles.publicLookup().unreflect(erased);
        } catch (Exception error) { throw new AssertionError(error); }
    }
    static void value(boolean ok) { if (!ok) throw new AssertionError("value"); values++; }
    static void same(Object actual, Object expected) { if (actual != expected) throw new AssertionError("identity"); identities++; }
    static void state(boolean ok) { if (!ok) throw new AssertionError("state"); states++; }
    static Object call(int route, Core self, Object input) throws Throwable {
        try {
            switch (route) {
            case 0: return ProtectedScopedArrayReturnCaller.erased(self, input);
            case 1: return ProtectedScopedArrayReturnCaller.typed(self, (Result)input);
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
        TypeVariable<?> variable = Core.class.getTypeParameters()[0];
        value(((ParameterizedType)parent[0]).getActualTypeArguments()[0].equals(variable)
                && ((GenericArrayType)parent[1]).getGenericComponentType().equals(variable)
                && typed.getGenericParameterTypes()[0].equals(parent[0]) && typed.getGenericReturnType().equals(parent[1]));
        RuntimeException problem = new IllegalStateException("owned target failure");
        RuntimeException providerProblem = new IllegalArgumentException("owned provider failure");
        Object marker = new Object(); Object[] sentinel = new Object[]{marker};
        for (int route=0; route<6; route++) for (boolean nullSelf : new boolean[]{false,true})
            for (boolean nullBox : new boolean[]{false,true})
                for (Object[] output : new Object[][]{null, new Object[0], new Object[]{marker}, new String[]{"owned"}})
                    for (boolean fail : new boolean[]{false,true}) for (boolean providerFail : new boolean[]{false,true}) {
                        Core self = nullSelf ? null : new Core();
                        Result box = nullBox ? null : new Result();
                        if (self != null) { self.seen = marker; self.last = sentinel; self.failure = fail ? problem : null; }
                        if (box != null) { box.data = output; box.failure = providerFail ? providerProblem : null; }
                        Object result = null; Throwable error = null;
                        try { result = call(route, self, box); } catch (Throwable caught) { error = caught; }
                        state(box == null || box.reads == (!nullSelf && !fail ? 1 : 0));
                        if (self == null) { value(error instanceof NullPointerException); failures++; }
                        else {
                            state(self.calls == 1 && self.visits == (route == 0 ? 1 : 0)); same(self.seen, box);
                            if (fail) { same(error, problem); same(self.last, sentinel); failures++; }
                            else if (box == null) { value(error instanceof NullPointerException); same(self.last, sentinel); failures++; }
                            else if (providerFail) { same(error, providerProblem); same(self.last, sentinel); failures++; }
                            else {
                                same(error, null); same(result, output); same(self.last, output);
                                if (output != null && output.length != 0) {
                                    ((Object[])result)[0] = "changed"; same(box.data[0], "changed");
                                }
                            }
                        }
                    }
        for (int route : new int[]{0,2,3}) for (Object wrong : new Object[]{new Object(), new Object[0], new int[0]}) {
            Core self = new Core(); self.seen = marker; Throwable error = null;
            try { call(route, self, wrong); } catch (Throwable caught) { error = caught; }
            value(error instanceof ClassCastException); failures++;
            state(self.calls == 0 && self.visits == (route == 0 ? 1 : 0)); same(self.seen, marker);
        }
        System.out.println("values="+values+",identities="+identities+",states="+states+",failures="+failures);
        System.out.println("scoped-array-return-alias-provider-call-and-failure-order=retained,typed-access-change=reported");
    }
}
