package fixture;

public interface Mapper<A, B> {
    B map(A value);
}
