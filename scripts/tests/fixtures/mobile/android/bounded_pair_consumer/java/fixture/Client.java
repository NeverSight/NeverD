package fixture;
public final class Client {
    @SuppressWarnings("unchecked")
    public static void call(Source source,State state) { Provider.consume(source,state); }
}
