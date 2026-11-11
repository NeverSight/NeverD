package fixture;
public class Names {
    public static String first = Trace.firstName;
    public static String second = Trace.secondName;
    static { Trace.hit(2); }
}
