package fixture;
public interface Function<A, B, C, R> {
    R invoke(A first, B second, C third);
}
