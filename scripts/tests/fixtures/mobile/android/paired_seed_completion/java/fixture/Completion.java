package fixture;
public interface Completion<T> {
    Object context();
    void accept(Object value);
}
