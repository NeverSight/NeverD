package fixture;
public final class Factory {
    public static int events;
    public Token savedToken;
    public Context savedContext;
    public Entry<?>[] savedEntries;
    public Object tokenMark;
    public int length;
    public Bound created, saved, replacement;
    public RuntimeException failure;
    public <T extends Bound> T create(Token<T> token, Context context, Entry<?>... entries) {
        events = events * 5 + 1;
        savedToken = token; savedContext = context; savedEntries = entries;
        tokenMark = token.inspect(); context.touch();
        length = entries.length;
        created = entries[0].build();
        if (failure != null) throw failure;
        saved = replacement;
        events = events * 5 + 2;
        return (T) replacement;
    }
}
