package fixture;
public class Later {
    public static int value;
    static {
        Provider.word = Trace.nextWord;
        Provider.wide = Trace.nextWide;
        Trace.hit(2);
    }
}
