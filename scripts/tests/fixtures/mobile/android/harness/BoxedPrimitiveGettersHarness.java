import fixture.*;
import java.lang.reflect.*;

public class BoxedPrimitiveGettersHarness {
    static int values, identities, failures;
    static void check(boolean value) { values++; if (!value) throw new AssertionError("value " + values); }
    static void same(Object a, Object b) { identities++; if (a != b) throw new AssertionError("identity " + identities); }
    static Object get(int entry, boolean wide, BoxedGetterOwner owner) {
        if (wide) return entry == 0 ? owner.getDouble() : ((BoxedGetterApi) owner).getDouble();
        return entry == 0 ? owner.getFloat() : ((BoxedGetterApi) owner).getFloat();
    }
    public static void main(String[] args) throws Exception {
        boolean rebuilt = args.length == 1 && args[0].equals("rebuilt");
        int[] floatBits = {0, 0x80000000, 0x3f800000, 0xbf800000, 0x7f800000, 0xff800000, 0x7fc01234, 0xffc04321};
        long[] doubleBits = {0L, 0x8000000000000000L, 0x3ff0000000000000L, 0xbff0000000000000L,
                             0x7ff0000000000000L, 0xfff0000000000000L, 0x7ff8000000001234L, 0xfff8000000004321L};
        for (int i = 0; i < floatBits.length; i++) {
            BoxedGetterOwner owner = new BoxedGetterOwner(Float.intBitsToFloat(floatBits[i]), Double.longBitsToDouble(doubleBits[i]));
            check(Float.floatToRawIntBits(owner.readFloat()) == floatBits[i]);
            check(Double.doubleToRawLongBits(owner.readDouble()) == doubleBits[i]);
            for (int entry = 0; entry < 2; entry++) {
                GetterFloatBox.calls = 0; GetterDoubleBox.calls = 0;
                GetterFloatBox narrow = (GetterFloatBox) get(entry, false, owner);
                GetterFloatBox again = (GetterFloatBox) get(entry, false, owner);
                check(narrow != again); check(Float.floatToRawIntBits(narrow.value) == floatBits[i]);
                check(Float.floatToRawIntBits(again.value) == floatBits[i]);
                check(Float.floatToRawIntBits(GetterFloatBox.last) == floatBits[i]);
                check(GetterFloatBox.calls == 2); check(GetterDoubleBox.calls == 0);
                GetterDoubleBox wide = (GetterDoubleBox) get(entry, true, owner);
                GetterDoubleBox another = (GetterDoubleBox) get(entry, true, owner);
                check(wide != another); check(Double.doubleToRawLongBits(wide.value) == doubleBits[i]);
                check(Double.doubleToRawLongBits(another.value) == doubleBits[i]);
                check(Double.doubleToRawLongBits(GetterDoubleBox.last) == doubleBits[i]);
                check(GetterDoubleBox.calls == 2); check(GetterFloatBox.calls == 2);
                for (Object input : new Object[] {owner, narrow, wide, "owned", null}) {
                    same(owner.echo(input, narrow), input); same(((BoxedGetterApi) owner).echo(input, wide), input);
                }
            }
        }
        BoxedGetterOwner owner = new BoxedGetterOwner(-3.0f, 19.0);
        for (boolean wide : new boolean[] {false, true}) for (int entry = 0; entry < 2; entry++) {
            RuntimeException sentinel = new RuntimeException("owned primitive boxing failure");
            GetterFloatBox.calls = 0; GetterDoubleBox.calls = 0;
            if (wide) GetterDoubleBox.failure = sentinel; else GetterFloatBox.failure = sentinel;
            try { get(entry, wide, owner); throw new AssertionError("missing sentinel"); }
            catch (RuntimeException failure) { failures++; same(failure, sentinel); }
            check(GetterFloatBox.calls == (wide ? 0 : 1)); check(GetterDoubleBox.calls == (wide ? 1 : 0));
            check(wide ? GetterDoubleBox.last == 19.0 : GetterFloatBox.last == -3.0f);
            GetterFloatBox.failure = null; GetterDoubleBox.failure = null;
            GetterFloatBox.calls = 0; GetterDoubleBox.calls = 0;
            try { get(entry, wide, null); throw new AssertionError("missing null receiver"); }
            catch (NullPointerException expected) { failures++; }
            check(GetterFloatBox.calls == 0); check(GetterDoubleBox.calls == 0);
        }
        for (String name : new String[] {"getFloat", "getDouble"}) {
            Method bridge = BoxedGetterOwner.class.getDeclaredMethod(name);
            check(bridge.isBridge() == !rebuilt); check(bridge.isSynthetic() == !rebuilt);
            same(bridge.getReturnType(), Object.class); same(bridge.getGenericReturnType(), Object.class);
        }
        for (Class<?> declaration : new Class<?>[] {BoxedGetterApi.class, BoxedGetterOwner.class}) {
            Method echo = declaration.getDeclaredMethod("echo", Object.class, Object.class);
            TypeVariable<?>[] formals = echo.getTypeParameters(); check(formals.length == 2);
            for (TypeVariable<?> formal : formals) { same(formal.getGenericDeclaration(), echo); same(formal.getBounds()[0], Object.class); }
            check(echo.getGenericParameterTypes()[0].equals(formals[0]));
            check(echo.getGenericParameterTypes()[1].equals(formals[1])); check(echo.getGenericReturnType().equals(formals[0]));
            check(declaration.getTypeParameters().length == 0);
        }
        System.out.println("values=" + values + ",identities=" + identities + ",failures=" + failures);
        System.out.println("primitive-width-bits-boxing-effects-and-failure-order=matched,original-bridge-flags=measured");
    }
}
