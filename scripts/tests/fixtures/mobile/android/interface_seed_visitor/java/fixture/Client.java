package fixture;
@SuppressWarnings({"unchecked", "rawtypes"})
public final class Client {
    public static void call(Node start, Fn operation) {
        Provider.visit(start, operation);
    }
}
