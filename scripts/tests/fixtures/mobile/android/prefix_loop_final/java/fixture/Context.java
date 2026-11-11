package fixture;

public interface Context {
    int enter(Core self, long seed, Object token) throws Throwable;
    void step(Core self, int index, Object token) throws Throwable;
    void spin(Core self, Object token) throws Throwable;
    boolean choose(Core self, boolean branch) throws Throwable;
    Object left(Core self, long seed, Object token) throws Throwable;
    Object right(Core self, long seed, Object token) throws Throwable;
    long finish(Core self, long seed) throws Throwable;
}
