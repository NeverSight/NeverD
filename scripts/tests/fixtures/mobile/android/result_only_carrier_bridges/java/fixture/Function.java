package fixture;
public interface Function<P, R> {
    R invoke(P value);
}
