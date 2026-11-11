package fixture;
public interface Sink<T> {
    int compare(T left, T right);
}
