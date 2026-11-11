package fixture;

public final class MutableReader implements Reader {
    public String value;
    public int reads;
    public RuntimeException failure;
    public MutableReader(String value) { this.value = value; }
    public String read() {
        ++reads;
        if (failure != null) throw failure;
        return value;
    }
}
