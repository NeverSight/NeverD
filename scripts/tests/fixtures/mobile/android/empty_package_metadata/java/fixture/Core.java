package fixture;

public class Core {
    public Core() {}
    public static int run(int value) {
        return fixture.beta.Anchor.apply(fixture.alpha.Anchor.apply(value));
    }
    public static void ignore(int value) {
        fixture.beta.Anchor.apply(fixture.alpha.Anchor.apply(value));
    }
}
