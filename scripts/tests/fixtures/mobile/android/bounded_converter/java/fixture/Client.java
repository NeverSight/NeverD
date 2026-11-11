package fixture;
public final class Client {
    public static Bound call(Converter converter,Object value) { return Provider.access(converter,value); }
}
