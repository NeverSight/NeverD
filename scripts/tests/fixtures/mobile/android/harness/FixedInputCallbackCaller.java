import fixture.*;
public final class FixedInputCallbackCaller {
    public static Object call(Callback callback,Completion completion) { return Provider.work(callback,completion); }
}
