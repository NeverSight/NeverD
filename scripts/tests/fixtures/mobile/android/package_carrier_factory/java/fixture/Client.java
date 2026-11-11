package fixture;
@SuppressWarnings({"rawtypes", "unchecked"})
public class Client {
    public static Factory call(Seed seed, int code, Context first, Context second) {
        return Factory.select(seed, code, first, second);
    }
    public static java.io.Serializable checked(Seed seed, int code, Context first, Context second) {
        return (java.io.Serializable)Factory.select(seed, code, first);
    }
}
