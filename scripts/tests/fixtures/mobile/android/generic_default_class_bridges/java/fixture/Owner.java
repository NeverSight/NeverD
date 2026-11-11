package fixture;
public final class Owner {
    public int removals;
    public String key;
    public byte[] value;
    public RuntimeException failure;
    public Owner() {}
    public Leaf<String, byte[]> create() {
        return new Leaf<String, byte[]>() {
            protected void removed(String nextKey, byte[] nextValue) {
                removals++;
                key = nextKey;
                value = nextValue;
                Trace.events = Trace.events * 10 + 3;
                if (Owner.this.failure != null) throw Owner.this.failure;
            }
        };
    }
}
