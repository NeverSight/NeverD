import fixture.*;
public final class UnaryNestedCarrierCaller {
    public static Callback call(Carrier state) { return Provider.access(state); }
}
