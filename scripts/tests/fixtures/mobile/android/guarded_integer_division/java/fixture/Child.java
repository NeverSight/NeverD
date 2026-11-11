package fixture;
import java.util.List;
public final class Child<T> extends Base<T> {
    public final int divisor,remainder;
    public Child(List<T> list,int divisor) {
        super(list,list.size()/(divisor>0?divisor:1));
        Trace.require(divisor>0);
        this.divisor=divisor;
        this.remainder=list.size()%divisor;
    }
}
