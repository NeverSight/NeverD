package fixture;
public interface Api<T> {
    String Z_FIRST = Provider.first("owned$first".replace('$', '.'));
    String A_SECOND = Provider.second("owned$second".replace('$', '.'));
    Object COPY = Shared.VALUE;
}
