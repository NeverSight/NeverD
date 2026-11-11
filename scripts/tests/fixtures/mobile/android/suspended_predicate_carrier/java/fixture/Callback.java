package fixture;
public interface Callback<A,B,R> { R invoke(A value, B completion); }
