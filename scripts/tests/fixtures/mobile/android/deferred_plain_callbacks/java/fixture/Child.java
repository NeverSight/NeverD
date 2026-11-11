package fixture;

public final class Child<S, V> extends Parent<S, V> {
    public int calls;
    public int lastKind;
    public int lastFirst;
    public int lastSecond;
    public S lastLeft;
    public S lastRight;
    public RuntimeException failure;

    public Child() {}

    private void observeEvent(int kind, int first, int second) {
        calls++;
        lastKind = kind;
        lastFirst = first;
        lastSecond = second;
        events = events * 31 + kind;
        if (failure != null) throw failure;
    }

    public void added(int first, int second) { observeEvent(1, first, second); }
    public void moved(int first, int second) { observeEvent(2, first, second); }
    public void removed(int first, int second) { observeEvent(3, first, second); }
    public void changed(int first, int second) { observeEvent(4, first, second); }

    public int compare(S left, S right) {
        lastLeft = left;
        lastRight = right;
        observeEvent(5, 0, 0);
        return left == right ? 0 : 1;
    }
}
