package fixture;

import java.util.List;

public final class Provider {
    private Provider() {}
    public static <T> List<T> read(Context context,List<T> target,String key,Class<T> token) {
        return Entry.read(context,target,key,token);
    }
    static final class Entry {
        private Entry() {}
        static <T> List<T> read(Context context,List<T> target,String key,Class<T> token) {
            return context.read(target,key,token);
        }
    }
}
