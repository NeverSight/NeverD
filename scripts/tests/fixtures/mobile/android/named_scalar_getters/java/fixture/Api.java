package fixture;
public interface Api<T> {
    int size(); long span(); float ratio(); double weight();
    <R, S> R echo(R input, S tag);
}
