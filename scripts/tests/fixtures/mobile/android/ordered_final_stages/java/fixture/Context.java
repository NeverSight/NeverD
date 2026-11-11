package fixture;
public interface Context {
    int enter(Core self, long seed, Object token) throws Throwable;
    boolean choose(Core self, int selector) throws Throwable;
    Object left(Core self, long seed, Object token) throws Throwable;
    Object right(Core self, long seed, Object token) throws Throwable;
    void cleanup(Core self, Object token) throws Throwable;
    long finish(Core self, long seed) throws Throwable;
    Object marker(Core self, Object token) throws Throwable;
}
