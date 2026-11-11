package fixture;
public abstract class AbstractCounter<P> extends ReferenceRoot<P> implements CountView<P> {
    public AbstractCounter() {}
    public abstract P removeAt(int index);
    public final P remove(int index) { return removeAt(index); }
}
