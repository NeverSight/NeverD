package fixture;

public final class State {
    public static int constructors;
    public static int reads;
    public static int events;
    public static Token<?> lastToken;
    public static Object lastTag;
    public static Action<?> lastAction;
    public static RuntimeException constructorFailure;
    public static RuntimeException readFailure;
    public static void construct(Token<?> token, Object tag) {
        ++constructors;
        events = events * 10 + 1;
        lastToken = token;
        lastTag = tag;
        if (constructorFailure != null) throw constructorFailure;
    }
    public static void read() {
        ++reads;
        events = events * 10 + 2;
        if (readFailure != null) throw readFailure;
    }
}
