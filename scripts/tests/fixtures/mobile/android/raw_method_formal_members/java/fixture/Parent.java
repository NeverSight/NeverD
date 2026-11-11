package fixture;
public interface Parent<E> {
    <R, S> R[] array(R[] input, S tag);
    Object[] observe(Object[] input, Object tag);
    default <R, S> R[] call(R[] input, S tag) { return (R[]) observe(input, tag); }
}
