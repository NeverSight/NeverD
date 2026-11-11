package fixture;
public class Client<T> {
    public final T[] values;
    public final boolean tail;
    public Client(T[] values,boolean tail) {this.values=values;this.tail=tail;}
    @SuppressWarnings({"rawtypes","unchecked"})
    public static int search(Object[] values,Object key,java.util.Comparator comparator,boolean range) {
        return range ? java.util.Arrays.binarySearch(values,0,values==null?0:values.length,key,comparator)
                     : java.util.Arrays.binarySearch(values,key,comparator);
    }
    private int index(T value) {int result=Provider.score(values,value,Trace.expectedComparator);if(result<0)return tail?values.length:-1;return result;}
    public int lookup(T value) {return index(value);}
}
