package fixture;

public interface Reader<P, R> {
    R read(P value);
}
