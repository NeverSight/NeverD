package fixture;
public interface Api<C> {
    <R extends C, S extends R> R[] copy(R[] input, S tag, Sink<? super R> sink);
    <R extends C> void resume(R tag, Sink<? super R> sink);
    C value(C input);
}
