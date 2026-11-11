package fixture;
public final class Caller {
    private Caller() {}
    public static Object call(Core value, int index) { return value.get(index); }
}
