package fixture;

public final class Caller {
    public static Persistent<Object> remove(Leaf<Object> who, Object key, Object value) {
        return who.persistentRemove(key, value);
    }
    public static Persistent<Object> put(Actor<Object> who, Object value) {
        return who.persistentPut(value);
    }
    public static Persistent<Object> clear(Actor<Object> who) {
        return who.persistentClear();
    }
}
