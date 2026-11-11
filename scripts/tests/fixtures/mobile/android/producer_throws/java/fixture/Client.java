package fixture;
public class Client {
    public static <T> T call(Provider receiver,Handler<T> handler) throws Failure {return receiver.read(handler);}
    public static <T> Object ignore(Provider receiver,Handler<T> handler) throws Failure {receiver.read(handler);return Trace.marker;}
}
