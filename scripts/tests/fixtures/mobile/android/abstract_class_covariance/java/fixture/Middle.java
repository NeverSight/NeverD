package fixture;
public abstract class Middle extends Root implements Api {
    public abstract Middle get(Object marker);
    protected abstract Middle protectedGet();
    public abstract <U> U identity(U value);
}
