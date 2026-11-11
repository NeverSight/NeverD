package fixture;

public interface Persistent<P> extends RootBase<P> {
    Persistent<P> persistentRemove(Object key, Object value);
    Persistent<P> persistentPut(P value);
    Persistent<P> persistentClear();
}
