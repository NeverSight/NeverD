package fixture;
import java.util.Iterator;
public abstract class Base<E,C> {
    public int visits;
    protected abstract Iterator<E> values(C input);
    public Iterator<E> dispatch(C input) { visits++; return values(input); }
}
