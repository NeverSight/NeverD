package fixture;
public class Leaf extends Parent {
    @Override public Result read(String key) {
        Trace.leaves++;Trace.hit(8);return super.read(key);
    }
}
