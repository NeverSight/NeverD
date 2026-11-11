package fixture;
public final class Result<T> {
    public T[] data;
    public int reads;
    public RuntimeException failure;
    public T[] read() { reads++; if (failure != null) throw failure; return data; }
}
