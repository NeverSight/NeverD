package fixture;
public class Leaf extends Bound {
    protected static class Child<T extends Leaf> extends Parent<T> {
        public boolean run(Context context, T value, int bits) {
            boolean result = super.run(context, value, bits);
            Trace.hit(5); Trace.after = value; Trace.hit(6);
            return result;
        }
    }
    public static Base<Leaf> create() { return new Child<Leaf>(); }
}
