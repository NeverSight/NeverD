package fixture;
public class Client<T> {
    public final T[] values;
    public final boolean tail;
    public Client(T[] values,boolean tail) {this.values=values;this.tail=tail;}
    private int index(T value) {int result=Provider.score(values,value);if(result<0)return tail?values.length:-1;return result;}
    public int lookup(T value) {return index(value);}
}
