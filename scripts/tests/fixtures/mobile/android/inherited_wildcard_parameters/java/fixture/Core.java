package fixture;
public final class Core<K,V> extends Parent<Pair<K,V>,K,V> {
    public int calls, marker;
    public Object observed, key, value;
    public RuntimeException failure;
    public boolean specificRemove(Pair<K,V> entry) {
        calls++; marker = marker * 3 + 1; observed = entry;
        key = entry.getKey(); value = entry.getValue();
        if (key instanceof int[]) ((int[]) key)[0] += 5;
        if (value instanceof int[]) ((int[]) value)[0] += 7;
        if (failure != null) throw failure;
        return key != null && value != null;
    }
    public boolean entry(Pair<K,V> entry) { return specificRemove(entry); }
}
