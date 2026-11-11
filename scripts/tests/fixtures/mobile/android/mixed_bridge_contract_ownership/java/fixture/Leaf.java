package fixture;
public class Leaf extends Parent {
    @Override public Result read(String key) {
        Trace.leaves++;Trace.hit(8);return super.read(key);
    }
    @Override public <U> Leaf accept(U value) {
        Trace.seedLeaves++;Trace.hit(6);return (Leaf)super.accept(value);
    }
}
