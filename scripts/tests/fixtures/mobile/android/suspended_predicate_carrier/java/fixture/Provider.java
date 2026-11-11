package fixture;
@SuppressWarnings({"rawtypes", "unchecked"})
public class Provider {
    static { Trace.hit(6); }
    public static <T> Carrier<T> choose(Carrier<? extends T> source,
            Callback<? super T, ? super Completion<? super Boolean>, ? extends Object> callback) {
        Carrier<? extends T> captured = source;
        Trace.seenInput = captured; Trace.mask |= 1; Trace.hit(1);
        T value = captured.value();
        Completion<? super Boolean> channel = Trace.completion;
        Boolean done = Trace.done;
        Trace.seenValue = value; Trace.seenCompletion = channel; Trace.seenDone = done;
        Trace.mask |= 2; Trace.hit(2);
        Object returned = callback.invoke(value, channel);
        Trace.seenReturn = returned; Trace.mask |= 4; Trace.hit(3);
        if (channel != null) channel.resume(done);
        Trace.mask |= 8; Trace.hit(4);
        Carrier<T> result = (Carrier<T>) Trace.output;
        Trace.seenOutput = result; Trace.mask |= 16; Trace.hit(5);
        return result;
    }
}
