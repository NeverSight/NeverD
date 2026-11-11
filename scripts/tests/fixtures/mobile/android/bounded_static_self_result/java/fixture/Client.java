package fixture;
public class Client {
    public static Factory call(Bound seed) {
        return Factory.select(seed);
    }
    public static java.io.Serializable checked(Bound seed) {
        return (java.io.Serializable)Factory.select(seed);
    }
}
