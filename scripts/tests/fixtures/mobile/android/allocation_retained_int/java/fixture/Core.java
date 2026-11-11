package fixture;
public final class Core {
    public static Outer make(Producer producer) {
        return new Outer(new Inner(producer.length(),producer.length()));
    }
}
