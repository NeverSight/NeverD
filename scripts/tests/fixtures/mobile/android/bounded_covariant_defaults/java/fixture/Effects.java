package fixture;
public class Effects {
    public static Object receiver;
    public static Object input;
    public static RuntimeException failure;
    public static int calls;
    public static void enter(Object receiver, Object input) {
        calls++;
        Effects.receiver = receiver;
        Effects.input = input;
    }
}
