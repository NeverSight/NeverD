package fixture;
public interface Api {
    <T, U> T echo(Class<T> token, T input);
    Carrier<Number, String> inspect(Carrier<Number, String> input);
}
