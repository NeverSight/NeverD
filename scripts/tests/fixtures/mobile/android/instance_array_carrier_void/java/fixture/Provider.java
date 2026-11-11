package fixture;
public class Provider {
    public <T> void fill(T[] values, Contract.Creator<T> creator) {
        Trace.hit(2); Trace.receiver = this; Trace.array = values; Trace.usedCreator = creator;
        for (int index = 0; index < values.length; ++index) {
            Trace.hit(3); values[index] = creator.next(); Trace.hit(4); ++Trace.stores;
        }
        Trace.hit(5);
    }
}
