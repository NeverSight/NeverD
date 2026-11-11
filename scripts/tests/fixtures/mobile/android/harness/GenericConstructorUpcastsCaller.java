package fixture;
public class GenericConstructorUpcastsCaller {
    public static Core<Object,Object> make(Derived<Object,Object> value) { return new Core<>(value); }
}
