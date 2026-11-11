import fixture.Core;
import java.lang.invoke.*;
import java.lang.reflect.*;
public final class FinalArrayPayloadHarness {
    static void check(boolean value) {
        if (!value) throw new AssertionError("original final array payload");
    }
    static int call(int route, int index, int[] alias) throws Throwable {
        if (route == 0) return Core.read(index, alias);
        if (route == 1) {
            try { return (Integer) Core.class.getDeclaredMethod("read", int.class, int[].class)
                                        .invoke(null, index, alias); }
            catch (InvocationTargetException failure) { throw failure.getCause(); }
        }
        return (int) MethodHandles.lookup().findStatic(Core.class, "read",
                MethodType.methodType(int.class, int.class, int[].class)).invokeExact(index, alias);
    }
    public static void main(String[] args) throws Throwable {
        Field field = Core.class.getDeclaredField("VALUE");
        check(Modifier.isStatic(field.getModifiers()) && Modifier.isFinal(field.getModifiers()));
        int[] alias = Core.VALUE;
        check(field.get(null) == alias && alias.length == 6);
        check(alias[0] == 19 && alias[1] == -3 && alias[2] == 0 && alias[3] == 42 && alias[4] == 7 && alias[5] == 2);
        int calls = 0, failures = 0;
        for (int route = 0; route < 3; route++) {
            for (int mode = 0; mode < 8; mode++) {
                alias[2] = 0x7362 + route;
                int index = mode < 6 ? mode : mode == 6 ? -1 : 6;
                try {
                    int result = call(route, index, alias);
                    check(mode < 6 && result == alias[index]);
                } catch (Throwable failure) {
                    check(mode >= 6 && failure.getClass() == ArrayIndexOutOfBoundsException.class);
                    failures++;
                }
                check(Core.VALUE == alias && field.get(null) == alias && alias[2] == 0x7362 + route);
                calls++;
            }
            for (int[] other : new int[][]{null, new int[6]}) {
                try { call(route, 0, other); throw new AssertionError("alias failure missing"); }
                catch (IllegalArgumentException failure) { failures++; }
                check(Core.VALUE == alias);
                calls++;
            }
        }
        System.out.println("calls=" + calls + ",failures=" + failures + ",final-array-identity-and-payload=matched");
    }
}
