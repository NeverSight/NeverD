package fixture;
public interface Factory {
    Object make(Object[] values);
    void stage(int phase, Object value);
}
