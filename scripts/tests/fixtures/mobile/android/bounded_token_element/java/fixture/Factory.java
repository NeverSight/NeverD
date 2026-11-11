package fixture;
public abstract class Factory {
    protected Factory() {}
    protected abstract <T extends Bound> T create(String name,Token<T> token,Context context);
}
