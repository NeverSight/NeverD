package fixture;
public final class Creator {
    public static Bundle make(Box box, Object value) {
        return new Bundle(new Base<Object>(3) {
            public Object read() { return box.read(); }
        }, new Base<Object>(4) {
            public Object read() { return value; }
        }, value);
    }
}
