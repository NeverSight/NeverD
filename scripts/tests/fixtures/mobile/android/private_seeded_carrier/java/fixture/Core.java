package fixture;

public final class Core {
    public Object saved;
    public Object key;
    public Object seed;
    public boolean flag;
    public int events;
    public RuntimeException failure;
    public Carrier replacement;

    public Core() {}

    private <T> Carrier<T> make(String key, boolean flag, T seed) {
        events = events * 5 + 1;
        this.key = key;
        this.seed = seed;
        this.flag = flag;
        if (failure != null) throw failure;
        saved = replacement;
        events = events * 5 + 2;
        return replacement;
    }

    public Carrier call(String key, boolean flag, Object seed) {
        events = events * 5 + 3;
        Carrier result = make(key, flag, seed);
        events = events * 5 + 4;
        return result;
    }

    public <T> Carrier<T> empty(String key) { return make(key, false, null); }

    public void discard(String key, boolean flag, Object seed) { make(key, flag, seed); }
}
