package fixture;

import java.util.Map;

public final class Provider {
    private Provider() {}
    public static <A,B> void fill(Context context,Map<? super A,? super B> target,String key,Class<A> first,Class<B> second) {
        Entry.fill(context,target,key,first,second);
    }
    static final class Entry {
        private Entry() {}
        static <B,A> void fill(Context context,Map<? super A,? super B> target,String key,Class<A> first,Class<B> second) {
            context.fill(target,key,first,second);
        }
    }
}
