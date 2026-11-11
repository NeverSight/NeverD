package fixture;

import java.util.List;

public final class Provider {
    private Provider() {}
    public static <T> void fill(Context context,List<? super T> target,String key,Class<T> token) {
        Entry.fill(context,target,key,token);
    }
    static final class Entry {
        private Entry() {}
        static <T> void fill(Context context,List<? super T> target,String key,Class<T> token) {
            context.fill(target,key,token);
        }
    }
}
