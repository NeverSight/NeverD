package fixture;
public final class Client {
    @SuppressWarnings("rawtypes")
    public static Carrier call(Carrier parent, Object first, Object second, String label, int mode) {
        return Provider.create(parent, first, second, label, mode);
    }
}
