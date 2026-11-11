package fixture;
public class Trace {
    public static long events;
    public static int fail;
    public static RuntimeException failure;
    public static Object contextArgument, callbackArgument, context, callback, completion, scope, returned, result;
    public static int callbackCalls;
    public static void hit(int event) {
        events = events * 19 + event;
        if (event == fail) throw failure;
    }
    public static Context contextArg(Context context) {
        contextArgument = context;
        hit(1);
        return context;
    }
    public static Callback callbackArg(Callback callback) {
        callbackArgument = callback;
        hit(2);
        return callback;
    }
}
