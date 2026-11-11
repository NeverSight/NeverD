import fixture.*;
public final class NestedCompletionResultCaller {
    public static Object call(Context context, Callback block) throws InterruptedException { return Client.call(context,block); }
    public static Object initialized() { return Initializer.value; }
}
