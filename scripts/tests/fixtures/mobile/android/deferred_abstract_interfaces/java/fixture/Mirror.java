package fixture;

public interface Mirror<P> {
    P current();
    P tail();
}
