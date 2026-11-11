package fixture;
public class Child<I, J> extends Root<I, J> implements Contract<I, J> {
    public java.util.Collection<?> lastCollection;
    public int collectionCalls;
    public Child() {}
    @Override public boolean contains(Object key) {
        overrideCalls++;
        lastKey = key;
        if (failure != null) throw failure;
        return stored == key;
    }
    @Override @SuppressWarnings("unchecked") public J get(Object key) {
        overrideCalls++;
        lastKey = key;
        if (failure != null) throw failure;
        return (J) stored;
    }
    @Override public boolean containsAll(java.util.Collection<? extends Object> items) {
        collectionCalls++;
        lastCollection = items;
        if (failure != null) throw failure;
        return items == null;
    }
}
