import fixture.*;
import java.lang.invoke.*;
import java.lang.reflect.*;
public final class ProtectedMixedArrayHarness {
    static int values, identities, states, failures;
    static final Class<?>[] classes = { BooleanCore.class, ByteCore.class, ShortCore.class,
        CharacterCore.class, IntegerCore.class, LongCore.class, FloatCore.class, DoubleCore.class };
    static final Object[] replacements = { Boolean.TRUE, Byte.valueOf((byte)-3), Short.valueOf((short)-257),
        Character.valueOf('Q'), Integer.valueOf(0x12345678), Long.valueOf(0x1020304050607080L),
        Float.valueOf(-0.0f), Double.valueOf(-0.0d) };
    static void value(boolean ok) { if (!ok) throw new AssertionError("value"); values++; }
    static void same(Object a, Object b) { if (a != b) throw new AssertionError("identity"); identities++; }
    static void state(boolean ok) { if (!ok) throw new AssertionError("state"); states++; }
    static void call(int route, int kind, Base<?> self, Token token, Object array, int index,
                     Method typed, Method erased, MethodHandle typedHandle, MethodHandle erasedHandle) throws Throwable {
        try {
            switch (route) {
            case 0: ProtectedMixedArrayCaller.erased(self, token, array, index); return;
            case 1: ProtectedMixedArrayCaller.typed(self, token, array, index, kind); return;
            case 2: erased.invoke(self, token, array, index); return;
            case 3: erasedHandle.invoke(self, token, array, index); return;
            case 4: typed.invoke(self, token, array, index); return;
            default: typedHandle.invoke(self, token, array, index);
            }
        } catch (InvocationTargetException error) { throw error.getCause(); }
    }
    public static void main(String[] args) throws Throwable {
        boolean rebuilt = args.length != 0 && args[0].equals("rebuilt");
        RuntimeException problem = new IllegalStateException("owned failure"); Object marker = new Object();
        for (int kind=0; kind<classes.length; kind++) {
            Class<?> cls = classes[kind];
            Class<?> arrayType = (Class<?>)((ParameterizedType)cls.getGenericSuperclass()).getActualTypeArguments()[0];
            Class<?> element = arrayType.getComponentType();
            Method typed = cls.getDeclaredMethod("write", Token.class, arrayType, int.class); typed.setAccessible(true);
            Method erased = cls.getMethod("write", Token.class, Object.class, int.class);
            MethodHandle typedHandle = MethodHandles.lookup().unreflect(typed), erasedHandle = MethodHandles.publicLookup().unreflect(erased);
            value(typed.getModifiers() == (rebuilt ? Modifier.PUBLIC : Modifier.PROTECTED));
            value(erased.isBridge() && erased.isSynthetic() && Modifier.isPublic(erased.getModifiers()));
            same(typed.getParameterTypes()[1], arrayType);
            Object zero = Array.get(Array.newInstance(element, 1), 0);
            for (int route=0; route<6; route++)
                for (boolean nullSelf : new boolean[]{false, true})
                    for (boolean nullToken : new boolean[]{false, true})
                        for (int size : new int[]{-1, 0, 3})
                            for (int index : new int[]{-1, 0, 2, 3})
                                for (boolean fail : new boolean[]{false, true}) {
                                    Base<?> self = nullSelf ? null : (Base<?>)cls.getConstructor().newInstance();
                                    Token token = nullToken ? null : new Token();
                                    Object input = size < 0 ? null : Array.newInstance(element, size);
                                    if (self != null) { cls.getField("seen").set(self, marker); cls.getField("failure").set(self, fail ? problem : null); }
                                    Throwable error = null;
                                    try { call(route, kind, self, token, input, index, typed, erased, typedHandle, erasedHandle); }
                                    catch (Throwable caught) { error = caught; }
                                    boolean wrote = self != null && !fail && token != null && input != null && index >= 0 && index < size;
                                    if (self == null) { value(error instanceof NullPointerException); failures++; }
                                    else {
                                        state(cls.getField("calls").getInt(self) == 1 && self.visits == (route == 0 ? 1 : 0));
                                        same(cls.getField("seen").get(self), input); value(cls.getField("index").getInt(self) == index);
                                        if (fail) { same(error, problem); failures++; }
                                        else if (token == null || input == null) { value(error instanceof NullPointerException); failures++; }
                                        else if (!wrote) { value(error instanceof ArrayIndexOutOfBoundsException); failures++; }
                                        else same(error, null);
                                    }
                                    if (token != null) state(token.calls == (!nullSelf && !fail ? 1 : 0));
                                    if (input != null)
                                        for (int at=0; at<size; at++) value(Array.get(input, at).equals(wrote && at == index ? replacements[kind] : zero));
                                }
            for (int route : new int[]{0, 2, 3})
                for (Object wrong : new Object[]{new Object(), new Object[0], Array.newInstance(element, 0, 0),
                        Array.newInstance(element == boolean.class ? byte.class : boolean.class, 0)}) {
                    Base<?> self = (Base<?>)cls.getConstructor().newInstance(); cls.getField("seen").set(self, marker); Token token = new Token();
                    Throwable error = null;
                    try { call(route, kind, self, token, wrong, 0, typed, erased, typedHandle, erasedHandle); }
                    catch (Throwable caught) { error = caught; }
                    value(error instanceof ClassCastException); failures++;
                    state(cls.getField("calls").getInt(self) == 0 && self.visits == (route == 0 ? 1 : 0) && token.calls == 0);
                    same(cls.getField("seen").get(self), marker);
                }
        }
        System.out.println("values="+values+",identities="+identities+",states="+states+",failures="+failures);
        System.out.println("all-eight-primitive-array-inputs-aliases-and-ordered-stores=retained,typed-access-changes=reported");
    }
}
