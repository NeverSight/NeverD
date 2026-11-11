package fixture;
public final class Shared {
    public static final Object VALUE = create();
    private static Object create() {
        State.hit(4);
        if (!Api.Z_FIRST.equals("owned.first") || !Api.A_SECOND.equals("owned.second") || Api.COPY != null)
            throw new IllegalStateException("field copy order");
        return new Object();
    }
}
