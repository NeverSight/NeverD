package fixture;

import java.util.List;

public final class Provider {
    private Provider() {}
    public static <T extends Bound> List<T> read(Context context,List<T> target,String key) {
        return Entry.read(context,target,key);
    }
    static final class Entry {
        private Entry() {}
        static <T extends Bound> List<T> read(Context context,List<T> target,String key) {
            return context.read(target,key);
        }
    }
}
