package fixture;
public interface Callback<I,C,O> { O apply(I input,C completion); }
