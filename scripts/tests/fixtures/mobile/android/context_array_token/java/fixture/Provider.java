package fixture;

public final class Provider {
    private Provider() {}
    @SuppressWarnings("unchecked")
    public static <T> T[] read(Context context, String key, Class<T> token) {
        Trace.hit(1, context, key, token, null);
        context.callback(key, token);
        T[] result = (T[])context.reply;
        Trace.hit(2, context, key, token, result);
        return result;
    }
}
