package fixture;
public class ResultLeaf<T extends Bound> implements ResultBase<T> {
    public final Bound item;
    public ResultLeaf(Bound item) { this.item = item; }
    @SuppressWarnings("unchecked") public T value() { return (T) item; }
}
