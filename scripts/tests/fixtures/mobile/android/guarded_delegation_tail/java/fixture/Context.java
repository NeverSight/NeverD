package fixture;

public interface Context {
    boolean choose(Object[] values);
    int offset(Object[] values);
    void primary(Core self, int seed, long stamp) throws Throwable;
    void step(Core self, int index, Object value) throws Throwable;
    void finish(Core self, Object[] values) throws Throwable;
}
