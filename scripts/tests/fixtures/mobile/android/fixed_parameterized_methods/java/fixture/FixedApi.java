package fixture;
public interface FixedApi {
    void put(FixedBox<? super FixedTag> box);
    FixedBox<? super FixedTag> get();
}
