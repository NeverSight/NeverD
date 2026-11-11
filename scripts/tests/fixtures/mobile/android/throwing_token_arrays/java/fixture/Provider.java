package fixture;

import java.io.IOException;

public final class Provider {
    private Provider() {}

    @SuppressWarnings("unchecked")
    public static <T> T read(Class<T> token, Object... values) throws IOException, IllegalStateException {
        Trace.calls++;
        Trace.stage = 1;
        Trace.token = token;
        Trace.array = values;
        Trace.name = token.getName();
        Trace.stage = 2;
        Trace.length = values.length;
        Trace.first = values[0];
        Trace.stage = 3;
        Trace.action.run();
        if (Trace.failure != null) throw Trace.failure;
        Trace.stage = 4;
        return (T) Trace.value;
    }

    public static <U> U fixed(Class<U> token, Object[] values) throws IOException, IllegalStateException {
        return read(token, values);
    }
}
