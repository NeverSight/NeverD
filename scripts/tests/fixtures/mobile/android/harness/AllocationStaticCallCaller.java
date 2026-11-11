import fixture.*;
public final class AllocationStaticCallCaller {
    public static Target<Value> checked(Object input, long word, double bits) {
        Target<Value> result = new Target<Value>((Value) Provider.check(input, word, bits), input, word, bits);
        Trace.after = input;
        return result;
    }
    public static Target<Object> raw(Object input, long word, double bits) {
        Target<Object> result = new Target<Object>(Provider.check(input, word, bits), input, word, bits);
        Trace.after = input;
        return result;
    }
}
