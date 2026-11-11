package fixture;

import java.util.HashMap;

public final class Provider {
    private Provider() {}
    public static <A,B> HashMap<A,B> read(Context context,String key,Class<? extends A> first,Class<? extends B> second) {
        return Entry.read(context,key,first,second);
    }
    static final class Entry {
        private Entry() {}
        static <B,A> HashMap<A,B> read(Context context,String key,Class<? extends A> first,Class<? extends B> second) {
            return context.read(key,first,second);
        }
    }
}
