package fixture;

public interface Order<T> {
    int compare(T left, T right);
}
