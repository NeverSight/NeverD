package fixture;
public class Client {
    public static Object call(Context context, Callback block) throws InterruptedException {
        Object result = Provider.select(Trace.contextArg(context), Trace.callbackArg(block));
        Trace.result = result;
        Trace.hit(9);
        return result;
    }
}
