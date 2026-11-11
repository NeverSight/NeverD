package fixture;
public class Child<T> extends Parent<T> {
    public Child(Class<T> type,String... names) {this(false,type,names);}
    public Child(boolean flag,Class<T> type,String... names) {super(flag,new Proxy(names,type));}
}
