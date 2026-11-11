package fixture;

public class RootBase implements StateView {
    public int rootCalls;

    public RootBase() {
        rootCalls = 3;
    }

    public int rootState() {
        return rootCalls;
    }

    public <R> R identity(R value) {
        rootCalls++;
        return value;
    }

    public <R> R pair(Pair<R> box, R fallback) {
        rootCalls++;
        return fallback;
    }
}
