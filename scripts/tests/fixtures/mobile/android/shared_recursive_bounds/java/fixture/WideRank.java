package fixture;

public final class WideRank implements Comparable<Object> {
    public Object expected;
    public int value;
    public int calls;
    public RuntimeException failure;
    public WideRank(int value) { this.value = value; }
    public int compareTo(Object other) {
        ++calls;
        if (failure != null) throw failure;
        if (other != expected) throw new IllegalArgumentException();
        return value;
    }
}
