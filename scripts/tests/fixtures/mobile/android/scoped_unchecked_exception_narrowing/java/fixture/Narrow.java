package fixture;
public interface Narrow<I> extends Api<I> {
    I read(I input) throws First, Second;
    <Q> Q unwrap(Class<Q> kind) throws First;
}
