package fixture;

public interface Contract<T> {
    <A> A[] toArray(A[] values);
    <A, B> A[] copy(A[] values, B tag);
    T value(T next);
    boolean equals(Object other);
    int hashCode();
    String toString();
}
