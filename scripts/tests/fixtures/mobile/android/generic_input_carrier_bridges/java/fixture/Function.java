package fixture;
public interface Function<P, Q, R, O> {
    O invoke(P first, Q second, R number);
}
