import fixture.Child;
public final class ClosedArraySuperCaller {
    public static Child create(Object captured, Object context, int index, CharSequence[] input, long bits) {
        return new Child(captured, context, index, input, bits);
    }
}
