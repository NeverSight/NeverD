package fixture;
public interface Callback<A,B,R> { R invoke(A scope, B completion); }
