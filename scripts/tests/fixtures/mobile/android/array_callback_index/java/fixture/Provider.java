package fixture;
public class Provider {
    public static <T> int index(Filter<T> filter,T... values){return from(filter,0,values);}
    public static <T> int from(Filter<T> filter,int start,T... values){Trace.calls++;Trace.filter=filter;Trace.values=values;Trace.start=start;Trace.hit(1);Trace.action.run();Trace.hit(2);return Trace.result;}
}
