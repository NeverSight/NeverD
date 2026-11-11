package fixture;
@SuppressWarnings({"rawtypes", "unchecked"})
public final class Client {
    public static Result pick(Producer source, Object seed, Context context, long flags, int bits) {
        return Provider.select(source, seed, context, flags, bits);
    }
}
