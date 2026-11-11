package fixture;
public abstract class Base<T> {
    public int visits;
    protected abstract void write(Token token, T array, int index);
    public void dispatch(Token token, T array, int index) { visits++; write(token, array, index); }
}
