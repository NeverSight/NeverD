package fixture.beta;

public class Anchor {
    public static int calls;
    public Anchor() {}
    public static int apply(int value) {
        calls+=1;
        return value*5;
    }
}
