import fixture.Owner;
import fixture.View;
import java.lang.invoke.MethodHandles;
import java.lang.invoke.MethodType;
import java.lang.reflect.Constructor;
import java.lang.reflect.Field;
import java.lang.reflect.Modifier;

public final class UnwrittenTypedFinalsHarness {
    private static final String[] SUFFIX = {"Z", "B", "C", "S", "I", "J", "F", "D", "O", "A"};
    private static final Class<?>[] TYPES = {boolean.class, byte.class, char.class, short.class,
        int.class, long.class, float.class, double.class, Object.class, int[].class};

    private static void check(boolean value) {
        if (!value) throw new AssertionError("owned unwritten typed final");
    }

    private static View prototype(int route) throws Throwable {
        if (route == 0) return Owner.get();
        if (route == 1) return UnwrittenTypedFinalsCaller.get();
        if (route == 2) return (View) Owner.class.getMethod("get").invoke(null);
        return (View) MethodHandles.lookup().findStatic(Owner.class, "get",
            MethodType.methodType(View.class)).invokeExact();
    }

    private static Object read(View value, int route, int kind) throws Throwable {
        if (route == 1) return UnwrittenTypedFinalsCaller.read(value, kind);
        if (route == 2) return View.class.getMethod("read" + SUFFIX[kind]).invoke(value);
        if (route == 3) return MethodHandles.lookup().findVirtual(View.class,
            "read" + SUFFIX[kind], MethodType.methodType(TYPES[kind])).invoke(value);
        switch (kind) {
            case 0: return value.readZ();
            case 1: return value.readB();
            case 2: return value.readC();
            case 3: return value.readS();
            case 4: return value.readI();
            case 5: return value.readJ();
            case 6: return value.readF();
            case 7: return value.readD();
            case 8: return value.readO();
            case 9: return value.readA();
            default: throw new AssertionError();
        }
    }

    private static void equal(Object actual, Object expected, int kind) {
        if (kind >= 8) check(actual == expected);
        else if (kind == 6) check(Float.floatToRawIntBits((Float) actual) == Float.floatToRawIntBits((Float) expected));
        else if (kind == 7) check(Double.doubleToRawLongBits((Double) actual) == Double.doubleToRawLongBits((Double) expected));
        else check(actual.equals(expected));
    }

    public static void main(String[] args) throws Throwable {
        int route = Integer.parseInt(args[0]), mode = Integer.parseInt(args[1]);
        boolean finals = Boolean.parseBoolean(args[2]);
        Object[] zeros = {false, (byte) 0, (char) 0, (short) 0, 0, 0L, 0.0f, 0.0d, null, null};
        int[] floatBits = {0, 0x80000000, 0x7fc12345, 0xff812345, 0x7f800000, 1, 0x3f800000, 0xff7fffff};
        long[] doubleBits = {0L, 0x8000000000000000L, 0x7ff8123456789abcL, 0xfff0123456789abcL,
            0x7ff0000000000000L, 1L, 0x3ff0000000000000L, 0xffefffffffffffffL};
        for (int attempt = 0; attempt < 2; attempt++) {
            View shared = prototype(route);
            check(shared == Owner.PROTOTYPE);
            Constructor<?> constructor = shared.getClass().getDeclaredConstructor();
            constructor.setAccessible(true);
            View fresh = (View) constructor.newInstance();
            check(fresh != shared);
            check(Modifier.isFinal(fresh.getClass().getModifiers()) == finals);
            Field[] fields = new Field[10];
            for (int kind = 0; kind < 10; kind++) {
                fields[kind] = fresh.getClass().getDeclaredField("zero" + SUFFIX[kind]);
                fields[kind].setAccessible(true);
                check(fields[kind].getType() == TYPES[kind]);
                check(Modifier.isFinal(fields[kind].getModifiers()) == finals);
                equal(read(fresh, route, kind), zeros[kind], kind);
            }
            Object token = new Object();
            int[] array = {mode, attempt, Integer.MIN_VALUE};
            Object[] changed = {true, (byte) (mode * 31 + 1), (char) (0xffff - mode),
                (short) (Short.MIN_VALUE + mode), Integer.MIN_VALUE + mode,
                0x1234567800000000L + mode, Float.intBitsToFloat(floatBits[mode]),
                Double.longBitsToDouble(doubleBits[mode]), token, array};
            for (int kind = 0; kind < 10; kind++) fields[kind].set(fresh, changed[kind]);
            for (int repeat = 0; repeat < 3; repeat++)
                for (int kind = 0; kind < 10; kind++) equal(read(fresh, route, kind), changed[kind], kind);
            for (int kind = 0; kind < 10; kind++) fields[kind].set(fresh, zeros[kind]);
            for (int kind = 0; kind < 10; kind++) equal(read(fresh, route, kind), zeros[kind], kind);
            System.out.println("route=" + route + ",mode=" + mode + ",attempt=" + attempt + ",checks=60");
        }
    }
}
