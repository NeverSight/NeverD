package fixture;
public class Base<T> {
    public Callback<T> saved;
    public T value;
    public Object marker;
    public Base(Callback<T> callback) {
        Owner.events = Owner.events * 5 + 1;
        Owner.escaped = this;
        saved = callback;
        T result = callback.call(this);
        value = result;
        marker = Owner.current;
        if (Owner.failure != null) throw Owner.failure;
        Owner.events = Owner.events * 5 + 2;
    }
    public Base(Object value, int marker) { this.value = (T) value; this.marker = new Integer(marker); }
    public T get() { return value; }
}
