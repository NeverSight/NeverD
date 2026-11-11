package fixture;
public interface Factory {
    Object make(Object[] values, Object seed);
    void stage(int phase, Object value);
}
