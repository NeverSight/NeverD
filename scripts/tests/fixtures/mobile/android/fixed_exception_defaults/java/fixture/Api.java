package fixture;
public interface Api<T> {
    default T apply(Object marker, String name, Carrier<Number, String> input) throws First, Second {
        if (input == null) throw new First();
        return null;
    }
}
