package fixture;
public abstract class Middle<B extends Builder> extends Base<B> {
    protected void read(Token token, int index, B builder, boolean flag, long cookie, double weight) {
        throw new AssertionError("owned base callback");
    }
}
