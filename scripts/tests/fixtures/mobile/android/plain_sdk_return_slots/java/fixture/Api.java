package fixture;
public interface Api extends Marker {
    State sourceState();
    <M> M identity(M value);
    static int __neverdReturnSlot() { return 123; }
}
