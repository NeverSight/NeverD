package fixture;
@SuppressWarnings({"rawtypes", "unchecked"})
public final class View<T> extends Root {
    public View() {}
    public Void typedThrow(Entry<T> input) {
        Effects.cause();
        throw new Failure();
    }
    public boolean add(Object input) {
        Entry entry = (Entry) input;
        return ((Boolean) (Object) typedThrow(entry)).booleanValue();
    }
    public Void throwMany(java.util.Collection<? extends Entry<T>> entries) {
        Effects.cause();
        throw new Failure();
    }
    public boolean addAll(java.util.Collection entries) {
        return ((Boolean) (Object) throwMany(entries)).booleanValue();
    }
    public Void throwScalar(T input) {
        Effects.cause();
        throw new Failure();
    }
    public boolean push(Object input) {
        return ((Boolean) (Object) throwScalar((T) input)).booleanValue();
    }
}
