package fixture;
abstract class Provider {
    abstract <T extends Bound> int read(T value);
}
