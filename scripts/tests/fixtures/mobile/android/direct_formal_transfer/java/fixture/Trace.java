package fixture;

import java.io.IOException;

public final class Trace {
    private Trace() {}
    public static Object seed, callbackSeed, value, marker;
    public static Core constructing;
    public static Runnable action;
    public static IOException failure;
    public static int calls, callbacks, stage, entry;
}
