package fixture;

public interface Root<T> {
    T value(T next);

    Root<T> reverse();

    default T defaultValue(Object input, T fallback) {
        Trace.defaultCalls++;
        Trace.lastArgument = input;
        if (Trace.failure != null) throw Trace.failure;
        return fallback;
    }

    default int fallback(int input) {
        Trace.rootFallback++;
        Trace.lastOwner = 10;
        if (Trace.failure != null) throw Trace.failure;
        return input + 10;
    }

    default int inheritedOnly(int input) {
        Trace.rootOnly++;
        Trace.lastOwner = 30;
        if (Trace.failure != null) throw Trace.failure;
        return input + 30;
    }
}
