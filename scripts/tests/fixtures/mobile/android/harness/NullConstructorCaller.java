import fixture.Child;
import fixture.Owner;
public final class NullConstructorCaller {
    public static Owner create(int variant, int value) {
        return variant == 0 ? new Owner(value) : new Child(value);
    }
}
