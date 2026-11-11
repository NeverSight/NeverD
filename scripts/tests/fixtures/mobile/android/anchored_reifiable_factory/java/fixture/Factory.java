package fixture;

import java.lang.reflect.Constructor;
import java.util.List;

public final class Factory {
    public static int events;
    public static Class<?> savedType;
    public static Object savedContext;
    public static String name;
    public static int fingerprint;
    public static Object saved;
    public static Constructor replacement;
    public static RuntimeException failure;

    private Factory() {}

    public static <T> Constructor<T> create(Class<T> type, List<? extends Class<?>> context) {
        events = events * 5 + 1;
        savedType = type;
        savedContext = context;
        name = type.getName();
        fingerprint = context.hashCode();
        if (failure != null) throw failure;
        saved = replacement;
        events = events * 5 + 2;
        return replacement;
    }
}
