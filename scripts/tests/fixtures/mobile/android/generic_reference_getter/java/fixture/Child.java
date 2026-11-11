package fixture;
import java.util.Set;
public class Child<T> extends Counter<T> implements CountView<T> {
    public Set<T> alternate;
    public int overrides;
    public Child() {}
    @Override public Set<T> getItems() {
        overrides++;
        super.getItems();
        return alternate;
    }
}
