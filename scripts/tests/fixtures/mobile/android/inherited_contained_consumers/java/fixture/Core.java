package fixture;
public class Core<C> extends Base<C> implements Leaf<C>, Root<C> {
    public Middle<C> delegate;
    public Core() { super(7); }
    @Override public Object request(Consumer<? super C> input, Consumer<? super Value> output) {
        return delegate.request(input, output);
    }
}
