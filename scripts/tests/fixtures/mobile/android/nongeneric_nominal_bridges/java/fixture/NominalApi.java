package fixture;
public interface NominalApi {
    <M, N> M echo(M input, N tag);
    NominalBase get();
}
