package fixture;
public interface Api {
    Api apply(Object marker, String name, Carrier<Number, String> input);
}
