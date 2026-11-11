package fixture;

import java.io.IOException;

public final class Core {
    private Core() {}

    public static Object raw(Class<?> token, Object[] values) throws IOException {
        return Provider.read(token, values);
    }

    public static String narrow(Class<?> token, Object[] values) throws IOException {
        return (String) Provider.read(token, values);
    }

    public static void ignore(Class<?> token, Object[] values) throws IOException {
        Provider.read(token, values);
    }

    public static <R> R relay(Class<R> token, Object[] values) throws IOException {
        return Provider.read(token, values);
    }

    public static Object fixed(Class<?> token, Object[] values) throws IOException {
        return Provider.fixed(token, values);
    }

    public static Object retained(Class<?> token, Object[] values) throws IOException {
        Object before = values[0];
        Provider.read(token, values);
        return before;
    }
}
