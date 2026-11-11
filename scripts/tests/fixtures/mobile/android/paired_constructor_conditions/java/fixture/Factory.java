package fixture;
public interface Factory {
    Object step(Object value, int index);
    void stage(int phase, Object value);
}
