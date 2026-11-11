package fixture;
public class Host {
    public int[] marker;
    public int[] observed;
    public int calls;
    public Object seen;
    public RuntimeException failure;
    public Api<String> make() {
        return new Api<String>() {
            public final boolean contains(Object value) {
                if (!(value instanceof String)) return false;
                return contains((String) value);
            }
            public boolean contains(String value) { return super.contains(value); }
            public final int index(Object value) {
                if (!(value instanceof String)) return -1;
                return index((String) value);
            }
            public int index(String value) { return super.index(value); }
            public Object observe(Object value) {
                Host.this.calls++; Host.this.observed = Host.this.marker;
                Host.this.marker[0] += 3; Host.this.seen = value;
                if (Host.this.failure != null) throw Host.this.failure;
                return value;
            }
        };
    }
}
