package fixture;
public class Caller<V extends Bound> extends Base<V> {
    public Object read(Context context, V input, long bits) {
        return new Result(super.read(context,input,bits),this);
    }
}
