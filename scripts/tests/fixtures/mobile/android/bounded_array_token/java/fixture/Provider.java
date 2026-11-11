package fixture;
public final class Provider {
    private Provider() {}
    public static <T extends Mark> T[] read(Context context, String key, Token<T> token) {
        Trace.hit(1, context, key, token, null);
        context.before(key, token);
        T[] result = (T[])context.reply;
        Trace.hit(2, context, key, token, result);
        return result;
    }
}
