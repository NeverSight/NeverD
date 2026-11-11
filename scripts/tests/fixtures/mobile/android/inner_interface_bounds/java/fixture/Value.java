package fixture;

public class Value implements Item {
    public Object value;
    public int calls;
    public RuntimeException failure;
    public Value(Object value) { this.value = value; }
    public Object get() {
        ++calls;
        if (failure != null) throw failure;
        return value;
    }
}
