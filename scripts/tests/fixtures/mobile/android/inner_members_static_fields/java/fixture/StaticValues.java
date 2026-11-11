package fixture;

public class StaticValues {
    public static int initial;
    public static long wide;
    public static InnerHost token;
    static {
        InnerHost.trace = InnerHost.trace * 10 + 1;
        if (InnerHost.fail) throw InnerHost.failure;
        initial = 11;
        wide = 0x12345678abcdef01L;
        token = new InnerHost(91);
    }
}
