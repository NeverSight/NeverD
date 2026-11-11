import fixture.*;
public final class TaggedCompletionCallbackCaller {
    public static Object call(Policy policy,Tag tag,Callback callback,Completion completion) { return policy.work(tag,callback,completion); }
}
