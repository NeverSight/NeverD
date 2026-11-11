package fixture;

public final class Pair<K, V> {
    public Reader reader;
    public int accesses;
    public RuntimeException failure;
    public Pair(Reader reader) { this.reader = reader; }
    public Reader get() {
        ++accesses;
        if (failure != null) throw failure;
        return reader;
    }
}
