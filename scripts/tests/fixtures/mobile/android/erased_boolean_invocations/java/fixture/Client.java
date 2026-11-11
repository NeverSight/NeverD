package fixture;
public final class Client {
    public static boolean direct(Root receiver, int key, Object value) {
        return receiver.direct(key, value);
    }
    public static boolean checked(Creator creator, int key, long flags, double fraction) {
        return Provider.checked(creator, key, flags, fraction);
    }
}
