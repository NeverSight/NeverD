import fixture.Child;
public final class CheckedStarCaller {
    public static Child create(Object key, Object first, Object second) {
        return new Child(key, first, second);
    }
}
