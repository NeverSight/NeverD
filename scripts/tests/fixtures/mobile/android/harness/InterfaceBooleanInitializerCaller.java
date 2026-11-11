package fixture;
public class InterfaceBooleanInitializerCaller {
    public static boolean read() {
        boolean result=Policy.enabled;
        Trace.hit(4);
        return result;
    }
}
