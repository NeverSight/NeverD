package fixture;
public final class Client {
    public static Receiver make(Sink sink) { return new Receiver(sink); }
}
