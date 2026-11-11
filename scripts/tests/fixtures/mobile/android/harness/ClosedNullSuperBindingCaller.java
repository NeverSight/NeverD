import fixture.Child;
import fixture.Context;
public final class ClosedNullSuperBindingCaller {
    public static Child create(Context input) { return new Child(input); }
}
