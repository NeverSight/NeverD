package fixture.alpha;

public class Anchor {
    public static int calls;
    public Anchor() {}
    public static int apply(int value) {
        calls+=1;
        return value+3;
    }
}
