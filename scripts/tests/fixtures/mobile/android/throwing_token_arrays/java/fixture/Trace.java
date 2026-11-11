package fixture;

import java.io.IOException;

public final class Trace {
    public static Object value, first;
    public static Class<?> token;
    public static Object[] array;
    public static Runnable action;
    public static IOException failure;
    public static String name;
    public static int calls, callbacks, stage, length;
    private Trace() {}
}
