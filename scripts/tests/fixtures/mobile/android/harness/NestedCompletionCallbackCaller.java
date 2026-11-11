import fixture.*;
public final class NestedCompletionCallbackCaller {
    public static Object call(Policy policy,Callback callback,Completion completion) { return policy.work(callback,completion); }
}
