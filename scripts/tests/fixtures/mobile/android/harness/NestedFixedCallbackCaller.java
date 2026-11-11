import fixture.*;
public final class NestedFixedCallbackCaller {
    public static Callback call(Carrier state) { return Provider.access(state); }
}
