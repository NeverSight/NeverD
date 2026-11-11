package fixture;
public interface Callback<I,O> extends java.io.Serializable { O apply(I input); }
