import fixture.*;
public final class SeededVoidCaller {
    public static void call(Object seed, Fn<?,?> callback, Factory<?> task, long flags) {
        Client.call(seed, callback, task, flags);
    }
}
