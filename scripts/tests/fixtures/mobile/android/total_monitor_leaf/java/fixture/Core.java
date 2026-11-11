package fixture;
public final class Core extends Base {
    public Core(Object value) { super(value); }
    public synchronized Object own() { return this; }
    public synchronized Object input(Object value) { return value; }
    public synchronized int total(int left, int right) { return left + right; }
    public static Object read(Object lock, Object value) {
        return ((Core)lock).input(value);
    }
    public static Base typed(Object lock, Object value) {
        return (Base)read(lock, value);
    }
    public static int sum(Object lock, int left, int right) {
        return ((Core)lock).total(left, right);
    }
}
