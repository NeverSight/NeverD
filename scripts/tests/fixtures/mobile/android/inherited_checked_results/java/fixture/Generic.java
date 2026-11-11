package fixture;
public interface Generic<T> {
    T fetch();
    T fetch(int index);
    T fetch(long stamp, double fraction, Object token);
}
