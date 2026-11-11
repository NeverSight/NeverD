package fixture;
public abstract class Base<B> {
    public int visits;
    protected abstract void read(Token token, int index, B builder, boolean flag, long cookie, double weight);
    public void dispatch(Token token, int index, B builder, boolean flag, long cookie, double weight) {
        visits++; read(token, index, builder, flag, cookie, weight);
    }
}
