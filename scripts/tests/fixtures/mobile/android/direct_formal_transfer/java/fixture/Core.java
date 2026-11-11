package fixture;

import java.io.IOException;

public final class Core {
    public final Class<?> token;
    public Core(Class<?> token) {
        Trace.constructing = this;
        Provider.read(token);
        this.token = token;
    }
    public static Object raw(Object seed) { return Provider.read(seed); }
    public static String narrow(Object seed) { return (String)Provider.read(seed); }
    public static Object ignored(Object seed) {
        Provider.read(seed);
        return Trace.marker;
    }
    public static Class<?> token(Class<?> token) {
        Provider.read(token);
        return token;
    }
    public static Object checked(Object seed) throws IOException { return Provider.checked(seed); }
    public static String checkedNarrow(Object seed) throws IOException { return (String)Provider.checked(seed); }
}
