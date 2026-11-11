package fixture;

public abstract class Parent<T, V> implements Order<T>, Updates {
    public int events;
    public int forwardingCalls;
    public Object lastPayload;

    public Parent() {}

    public abstract int compare(T left, T right);
    public abstract void changed(int first, int second);

    public void changed(int first, int second, Object payload) {
        forwardingCalls++;
        lastPayload = payload;
        events = events * 31 + 6;
        changed(first, second);
    }
}
