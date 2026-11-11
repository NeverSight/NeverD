package fixture;

public interface Factory<A,B,R> extends Marker<R> {
    R get();
}
