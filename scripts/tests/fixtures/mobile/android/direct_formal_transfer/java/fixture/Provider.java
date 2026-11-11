package fixture;

import java.io.IOException;

public final class Provider {
    private Provider() {}
    @SuppressWarnings("unchecked")
    public static <T> T read(T seed) throws IllegalArgumentException {
        Trace.calls++;
        Trace.entry = 1;
        Trace.seed = seed;
        Trace.stage = 1;
        Trace.action.run();
        Trace.stage = 2;
        return (T)Trace.value;
    }
    @SuppressWarnings("unchecked")
    public static <T> T checked(T seed) throws IOException {
        Trace.calls++;
        Trace.entry = 2;
        Trace.seed = seed;
        Trace.stage = 1;
        Trace.action.run();
        Trace.stage = 2;
        if (Trace.failure != null) throw Trace.failure;
        return (T)Trace.value;
    }
}
