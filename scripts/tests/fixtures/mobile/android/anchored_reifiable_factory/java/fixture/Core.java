package fixture;

import java.lang.reflect.Constructor;
import java.util.List;

public final class Core {
    private Core() {}

    public static Constructor call(Class type, List context) {
        Factory.events = Factory.events * 5 + 3;
        Constructor result = Factory.create(type, context);
        Factory.events = Factory.events * 5 + 4;
        return result;
    }

    public static void discard(Class type, List context) { call(type, context); }

    public static <R> Constructor<R> typed(Class<R> type, List<? extends Class<?>> context) {
        Factory.events = Factory.events * 5 + 3;
        Constructor<R> result = Factory.create(type, context);
        Factory.events = Factory.events * 5 + 4;
        return result;
    }
}
