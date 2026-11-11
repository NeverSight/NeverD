package fixture;
public final class Client {
    public static Object call(Context a, Context b, boolean flag, Context c,
            Producer producer, Completion completion) {
        return Provider.first(a, b, flag, c, producer, completion);
    }
}
