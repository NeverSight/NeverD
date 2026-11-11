package fixture;
public class Caller {
    public static Outer.Member call(Outer outer, Converter converter, String label, int mode) {
        return Factory.create(outer, converter, label, mode);
    }
}
