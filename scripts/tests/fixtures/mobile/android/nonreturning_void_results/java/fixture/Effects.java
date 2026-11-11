package fixture;
public final class Effects {
    public static int calls;
    public static RuntimeException failure;
    public static Void cause() {
        calls++;
        if (failure != null) throw failure;
        return null;
    }
}
