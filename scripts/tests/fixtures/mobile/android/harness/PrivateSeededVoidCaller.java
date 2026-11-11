import fixture.*;
public final class PrivateSeededVoidCaller {
    public static void call(Object seed, Fn<?,?> callback, Factory<?> task, long flags) {
        Client.call(seed, callback, task, flags);
    }
}
