package fixture;
public final class Provider {
    public static <T extends Comparable<? super T>> T clamp(T seed,Range<T> range) {
        Trace.seenSeed=seed;Trace.seenRange=range;Trace.hit(1);
        T low=range.lower();Trace.low=low;Trace.hit(3);
        int first=seed.compareTo(low);Trace.first=first;Trace.hit(5);
        if(first<0)return low;
        T high=range.upper();Trace.high=high;Trace.hit(7);
        int second=seed.compareTo(high);Trace.second=second;Trace.hit(9);
        return second>0?high:seed;
    }
}
