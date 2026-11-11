package fixture;

public interface Duo<A, B> {
    A first();
    B second();
    B replace(B value);
}
