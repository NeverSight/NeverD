package fixture;
public interface Api {
    void accept(RawBox<?> box, Object tag);
    <M extends Mark> M select(RawBox<M> box);
    default <M extends Mark> M inherited(RawBox<M> box) {
        Trace.events = Trace.events * 10 + 6;
        return select(box);
    }
}
