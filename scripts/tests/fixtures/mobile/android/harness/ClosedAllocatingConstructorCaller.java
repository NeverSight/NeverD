import fixture.*;
public final class ClosedAllocatingConstructorCaller {
    public static Core call(long bits,Callback<? super Value,? super Label,Value> callback) { return Core.create(bits,callback); }
}
