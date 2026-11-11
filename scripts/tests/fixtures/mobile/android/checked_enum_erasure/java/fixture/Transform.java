package fixture;

public interface Transform<P, R> {
    R apply(P value);
}
