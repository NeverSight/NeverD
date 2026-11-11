package fixture;
import java.util.*;

public abstract class Adapter<T,U> extends AbstractMap<T,U> {
    public Adapter() {}
    @SuppressWarnings({"unchecked", "rawtypes"})
    public Set<Object> getKeys() { return (Set) super.keySet(); }
    @Override @SuppressWarnings({"unchecked", "rawtypes"})
    public final Set<T> keySet() { return (Set) getKeys(); }
}
