package fixture;
public final class Factory {
    public static int events;
    public Token savedToken;
    public Object marker;
    public String replacement, savedResult;
    public RuntimeException failure;
    public <T extends Bound> String key(Token<T> token) {
        events = events * 5 + 1;
        savedToken = token;
        marker = token.inspect();
        if (failure != null) throw failure;
        savedResult = replacement;
        events = events * 5 + 2;
        return replacement;
    }
}
