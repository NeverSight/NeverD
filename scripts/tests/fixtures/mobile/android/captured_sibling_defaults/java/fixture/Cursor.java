package fixture;

public interface Cursor<E> {
    boolean hasNext();
    E next();
    <T> T[] echo(T[] values);
    default void visit(java.util.function.Consumer<? super E> consumer) {
        if (hasNext()) consumer.accept(next());
    }
}
