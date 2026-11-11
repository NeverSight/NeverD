package fixture;
public final class Client {
    public static FixedResult call(Object seed, Carrier<?, ?> callback, float[] values) {
        return Facade.make(seed, (Carrier)callback, values);
    }
}
