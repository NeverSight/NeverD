package fixture;
public interface BoxedGetterApi {
    Object getFloat();
    Object getDouble();
    <R, S> R echo(R input, S tag);
}
