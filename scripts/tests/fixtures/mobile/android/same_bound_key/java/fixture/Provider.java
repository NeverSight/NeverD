package fixture;
public final class Provider {
    public long visits;
    public Key seen;
    public Element returned;
    public boolean skip;
    public <E extends Element> E get(Key<E> key) {
        visits++;seen=key;
        if(skip)return null;
        E value=key.read();returned=value;return value;
    }
}
