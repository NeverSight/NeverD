package fixture;
public class Provider {
    static {
        State.hit(1);
        if (Api.Z_FIRST != null || Api.A_SECOND != null || Api.COPY != null)
            throw new IllegalStateException("premature interface assignment");
    }
    public static String first(String value) {
        State.hit(2);
        if (Api.Z_FIRST != null || Api.A_SECOND != null)
            throw new IllegalStateException("first assignment order");
        return value;
    }
    public static String second(String value) {
        State.hit(3);
        if (!Api.Z_FIRST.equals("owned.first") || Api.A_SECOND != null)
            throw new IllegalStateException("second assignment order");
        return value;
    }
}
