package fixture;
public final class MutablePair implements Pair<Object,Object> {
    public Object key, value;
    public int keyCalls, valueCalls, marker;
    public RuntimeException keyFailure, valueFailure;
    public Object getKey() { keyCalls++; marker = marker * 3 + 1; if (keyFailure != null) throw keyFailure; return key; }
    public Object getValue() { valueCalls++; marker = marker * 3 + 2; if (valueFailure != null) throw valueFailure; return value; }
}
