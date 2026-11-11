package fixture;
import java.util.Iterator;
public abstract class NestedRoot<R,S> implements NestedBranch<R,S> {
    public NestedRoot() {}
    public abstract Iterator<Pair<? extends R,? extends S>> values();
}
