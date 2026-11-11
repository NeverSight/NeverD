package fixture;

public interface Nested<T> {
    Box<? super T> copy(Box<? super T> value);
}
