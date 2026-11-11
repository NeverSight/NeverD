package fixture;
public interface RawApi {
    RawBox<RawTag> get(RawTag input);
    void accept(RawBox<RawTag> box);
}
