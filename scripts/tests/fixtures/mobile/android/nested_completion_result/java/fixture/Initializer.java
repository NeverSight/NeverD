package fixture;
public class Initializer {
    public static Object value;
    static {
        try {
            value = Provider.select(Trace.contextArg(Context.installed), Trace.callbackArg(Context.block));
            Trace.result = value;
            Trace.hit(9);
        } catch (InterruptedException cause) {
            throw new ExceptionInInitializerError(cause);
        }
    }
}
