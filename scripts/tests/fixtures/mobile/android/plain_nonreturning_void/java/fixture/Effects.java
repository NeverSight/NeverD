package fixture;
public final class Effects {
    public static int calls;
    public static Object receiver;
    public static RuntimeException failure;
    public static Void prefix(Core value) {
        calls++;
        receiver = value;
        if (failure != null) throw failure;
        return null;
    }
}
