package fixture;
@SuppressWarnings({"rawtypes","unchecked"})
public final class Core {
    public static int call(Comparable first,Comparable second) {
        Trace.hit(5);int result=Comparisons.compareValues(first,second);Trace.result=result;Trace.hit(6);return result;
    }
}
