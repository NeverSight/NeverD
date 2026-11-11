package fixture;
public interface FixedChild extends FixedApi {
    <R> R echo(R input);
}
