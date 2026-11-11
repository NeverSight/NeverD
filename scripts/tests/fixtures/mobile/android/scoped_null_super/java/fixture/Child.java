package fixture;
import java.util.function.Function;
public class Child<T> extends Parent<T> {
    public final boolean secondary;
    public final Function<T,Comparable<?>> fn;
    public Child(boolean flag,Function<T,Comparable<?>> fn) {this(flag,false,fn);}
    public Child(boolean flag,boolean secondary,Function<T,Comparable<?>> fn) {
        super(flag,null);this.secondary=secondary;this.fn=fn;Trace.hit(4);
    }
}
