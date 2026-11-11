package fixture;
@SuppressWarnings({"rawtypes", "unchecked"})
public class Client {
    public static void call() {
        Trace.hit(9);
        Provider.apply(Trace.sink, Trace.pairs);
        Trace.hit(8);
    }
}
