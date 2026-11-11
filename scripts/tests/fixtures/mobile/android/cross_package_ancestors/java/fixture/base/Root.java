package fixture.base;

public abstract class Root<T> {
    public int seed;
    public int parentCalls;
    public RuntimeException failure;
    public Root(int seed) { this.seed = seed; }
    public abstract T echo(T value);
    final T hidden(T value) { ++parentCalls; return echo(value); }
    public T rootEntry(T value) { return hidden(value); }
    static int mode(int position) { return position + 10; }
    public int rootMode(int position) { return seed + mode(position); }
}
