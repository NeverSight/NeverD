package fixture;
public interface Api<T> {
    Box visit(T value, int[] marker) throws First, Second;
}
