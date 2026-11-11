package fixture;
@SuppressWarnings({"unchecked", "rawtypes"})
public final class Client {
    public static void call(Node start, Fn operation) {
        Provider.forward(start, operation);
    }
}
