package fixture;
public final class Client {
    private Client() {}
    public static Object produce() { return Holder.build(); }
}
