package fixture;
public class DirectReader extends Base {
    public DirectReader(Object payload) {
        super(payload, Holder.word, Holder.wide);
        Trace.hit(4);
    }
}
