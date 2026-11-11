package fixture;
import java.util.Iterator;
@SuppressWarnings("rawtypes")
public class Core extends Parent implements Api {
    public NarrowIterator values() { return (NarrowIterator) super.values(); }
    public Iterator<?> entry() { return values(); }
    public Iterator call() { return values(); }
}
