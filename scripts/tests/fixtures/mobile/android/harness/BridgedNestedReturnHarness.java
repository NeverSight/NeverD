import fixture.*;
import java.lang.invoke.*;
import java.lang.reflect.*;
import java.util.*;
@SuppressWarnings({"rawtypes", "unchecked"})
public final class BridgedNestedReturnHarness {
    static int values, identities, states, failures;
    static final Method typed, erased;
    static final MethodHandle typedHandle, erasedHandle;
    static {
        try {
            typed = Core.class.getDeclaredMethod("values", Token.class); typed.setAccessible(true);
            erased = Core.class.getMethod("values", Object.class);
            typedHandle = MethodHandles.lookup().unreflect(typed);
            erasedHandle = MethodHandles.publicLookup().unreflect(erased);
        } catch (Exception error) { throw new AssertionError(error); }
    }
    static void value(boolean ok) { if (!ok) throw new AssertionError("value"); values++; }
    static void same(Object a, Object b) { if (a != b) throw new AssertionError("identity"); identities++; }
    static void state(boolean ok) { if (!ok) throw new AssertionError("state"); states++; }
    static Iterator call(int route, Core self, Object input) throws Throwable {
        try {
            switch (route) {
            case 0: return self.typed((Token)input);
            case 1: return ((Base)self).dispatch(input);
            case 2: return BridgedNestedReturnCaller.call(self, input);
            case 3: return (Iterator)erased.invoke(self, input);
            case 4: return (Iterator)erasedHandle.invokeExact(self, input);
            case 5: return (Iterator)typed.invoke(self, input);
            case 6: return (Iterator)typedHandle.invokeExact(self, (Token)input);
            default: return BridgedNestedReturnCaller.typed(self, (Token)input);
            }
        } catch (InvocationTargetException error) { throw error.getCause(); }
    }
    interface Action { void run() throws Throwable; }
    static void failure(Action action, Class<?> kind) throws Throwable {
        try { action.run(); throw new AssertionError("missing failure"); }
        catch (Throwable problem) { if (problem.getClass() != kind) throw new AssertionError("failure", problem); failures++; }
    }
    public static void main(String[] args) throws Throwable {
        boolean rebuilt = args.length > 0 && args[0].equals("rebuilt");
        value(typed.getModifiers() == (rebuilt ? Modifier.PUBLIC : Modifier.PROTECTED));
        value(erased.isBridge() && erased.isSynthetic() && Modifier.isPublic(erased.getModifiers()));
        TypeVariable<?>[] formals = Core.class.getTypeParameters();
        ParameterizedType parameter = (ParameterizedType)typed.getGenericParameterTypes()[0];
        ParameterizedType result = (ParameterizedType)((ParameterizedType)typed.getGenericReturnType()).getActualTypeArguments()[0];
        for (int i = 0; i < 2; i++) {
            value(parameter.getActualTypeArguments()[i].equals(formals[i]));
            Type actual = result.getActualTypeArguments()[i];
            value(rebuilt ? actual instanceof WildcardType && ((WildcardType)actual).getUpperBounds()[0].equals(formals[i]) : actual.equals(formals[i]));
        }
        RuntimeException marker = new IllegalStateException("owned nested result failure");
        for (int route = 0; route < 8; route++) {
            for (boolean nilInput : new boolean[]{false, true})
                for (boolean nilResult : new boolean[]{false, true})
                    for (boolean fail : new boolean[]{false, true}) {
                        Core<Object,Object> self = new Core<Object,Object>();
                        Token<Object,Object> token = nilInput ? null : new Token<Object,Object>();
                        Pair<Object,Object> pair = new Pair<Object,Object>(); pair.first = self; pair.second = marker;
                        Iterator<Pair<Object,Object>> expected = nilResult ? null : Arrays.asList(pair).iterator();
                        if (token != null) token.value = expected;
                        self.failure = fail ? marker : null;
                        Iterator actual = null; Throwable error = null;
                        try { actual = call(route, self, token); } catch (Throwable caught) { error = caught; }
                        state(self.calls == 1 && self.visits == (route == 1 || route == 2 ? 1 : 0));
                        if (nilInput) { value(error instanceof NullPointerException); failures++; }
                        else {
                            state(token.reads == 1); same(token.receiver, self); same(token.value, expected);
                            if (fail) { same(error, marker); failures++; }
                            else {
                                same(error, null); same(actual, expected);
                                if (actual != null) { same(actual.next(), pair); same(pair.first, self); same(pair.second, marker); value(!actual.hasNext()); }
                            }
                        }
                    }
            final int selected = route;
            failure(() -> call(selected, null, null), NullPointerException.class);
        }
        for (int route : new int[]{1, 2, 3, 4}) {
            final int selected = route; Core self = new Core();
            failure(() -> call(selected, self, new Object()), ClassCastException.class);
            state(self.calls == 0 && self.visits == (route == 1 || route == 2 ? 1 : 0));
        }
        System.out.println("values="+values+",identities="+identities+",states="+states+",failures="+failures);
        System.out.println("nested-variance-and-access=reported,original-bridge-descriptor-and-effects=retained");
    }
}
