package fixture;
@SuppressWarnings({"rawtypes","unchecked"})
class ComparisonBase {
    static { Trace.hit(7); }
    public static <T extends Comparable<?>> int compareValues(T first,T second) {
        Trace.hit(1);
        if(first==second)return 0;
        if(first==null)return -1;
        if(second==null)return 1;
        Trace.hit(2);return ((Comparable)first).compareTo(second);
    }
}
