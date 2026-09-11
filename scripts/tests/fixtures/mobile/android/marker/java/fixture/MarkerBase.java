package fixture;

@MarkerDefault
@MarkerClass
@MarkerRuntime
@MarkerInherited
public class MarkerBase {
    public int seed;

    public MarkerBase(int value) {
        seed = value + 3;
    }

    public int adjust(int value) {
        return seed * 2 - value;
    }
}
