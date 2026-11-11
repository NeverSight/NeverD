import fixture.*;
import java.lang.invoke.*;
import java.lang.reflect.*;
public final class ProtectedArrayGetterHarness {
    static int values, identities, states, failures;
    static final Class<?>[] classes = { BooleanCore.class, ByteCore.class, ShortCore.class,
        CharacterCore.class, IntegerCore.class, LongCore.class, FloatCore.class, DoubleCore.class };
    static void value(boolean ok) { if (!ok) throw new AssertionError("value"); values++; }
    static void same(Object a, Object b) { if (a != b) throw new AssertionError("identity"); identities++; }
    static void state(boolean ok) { if (!ok) throw new AssertionError("state"); states++; }
    static Object call(int route, int kind, Base<?> self, Method typed, Method erased,
                       MethodHandle typedHandle, MethodHandle erasedHandle) throws Throwable {
        try {
            switch (route) {
            case 0: return ProtectedArrayGetterCaller.erased(self);
            case 1: return ProtectedArrayGetterCaller.typed(self, kind);
            case 2: return erased.invoke(self);
            case 3: return erasedHandle.invoke(self);
            case 4: return typed.invoke(self);
            default: return typedHandle.invoke(self);
            }
        } catch (InvocationTargetException error) { throw error.getCause(); }
    }
    public static void main(String[] args) throws Throwable {
        boolean rebuilt = args.length != 0 && args[0].equals("rebuilt");
        RuntimeException problem = new IllegalStateException("owned failure");
        for (int kind = 0; kind < classes.length; kind++) {
            Class<?> cls = classes[kind]; Method typed = null, erased = null;
            for (Method method : cls.getDeclaredMethods())
                if (method.getName().equals("get")) {
                    if (method.isBridge()) erased = method; else typed = method;
                }
            typed.setAccessible(true);
            MethodHandle typedHandle = MethodHandles.lookup().unreflect(typed);
            MethodHandle erasedHandle = MethodHandles.publicLookup().unreflect(erased);
            value(typed.getModifiers() == (rebuilt ? Modifier.PUBLIC : Modifier.PROTECTED));
            value(erased.isBridge() && erased.isSynthetic() && Modifier.isPublic(erased.getModifiers()));
            Class<?> arrayType = typed.getReturnType(), element = arrayType.getComponentType();
            same(((ParameterizedType)cls.getGenericSuperclass()).getActualTypeArguments()[0], arrayType);
            Field stored = cls.getField("value"), last = cls.getField("last");
            Object empty = Array.newInstance(element, 0), filled = Array.newInstance(element, 3);
            Object marker = Array.newInstance(element, 2);
            Object item;
            if (kind == 0) item = Boolean.TRUE;
            else if (kind == 3) item = Character.valueOf('q');
            else item = Byte.valueOf((byte)7);
            Array.set(filled, 0, item);
            Object initial = Array.get(filled, 0);
            for (int route = 0; route < 6; route++)
                for (boolean nullSelf : new boolean[]{false, true})
                    for (Object input : new Object[]{null, empty, filled})
                        for (int length : new int[]{-1, 0, 3})
                            for (boolean fresh : new boolean[]{false, true})
                                for (boolean fail : new boolean[]{false, true}) {
                                    Base<?> self = nullSelf ? null : (Base<?>)cls.getConstructor().newInstance();
                                    if (self != null) {
                                        stored.set(self, input); last.set(self, marker);
                                        cls.getField("length").setInt(self, length);
                                        cls.getField("fresh").setBoolean(self, fresh);
                                        cls.getField("failure").set(self, fail ? problem : null);
                                    }
                                    Throwable error = null; Object result = null;
                                    try { result = call(route, kind, self, typed, erased, typedHandle, erasedHandle); }
                                    catch (Throwable caught) { error = caught; }
                                    if (nullSelf) { value(error instanceof NullPointerException); failures++; }
                                    else {
                                        state(cls.getField("calls").getInt(self) == 1);
                                        state(self.visits == (route == 0 ? 1 : 0));
                                        if (fail || (fresh && length < 0)) {
                                            if (fail) same(error, problem);
                                            else value(error instanceof NegativeArraySizeException);
                                            same(stored.get(self), input); same(last.get(self), marker); failures++;
                                        } else {
                                            same(error, null); same(result, stored.get(self)); same(result, last.get(self));
                                            if (!fresh) same(result, input);
                                            else { value(result != input && result.getClass() == arrayType);
                                                   value(Array.getLength(result) == length); }
                                        }
                                    }
                                    value(Array.getLength(empty) == 0 && Array.getLength(filled) == 3);
                                    value(Array.get(filled, 0).equals(initial));
                                }
        }
        System.out.println("values="+values+",identities="+identities+",states="+states+",failures="+failures);
        System.out.println("all-eight-primitive-array-results-and-original-aliases=retained,typed-access-changes=reported");
    }
}
