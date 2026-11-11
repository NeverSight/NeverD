package fixture;
import java.util.List;
public final class Direct<T> extends Base<T> {
    public final int divisor,remainder;
    public Direct(List<T> list,int divisor) {
        super(list,list.size()/divisor);
        this.divisor=divisor;
        this.remainder=list.size()%divisor;
    }
}
