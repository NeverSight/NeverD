package fixture;
public interface SourceView {
    Object getSource();
    Object echo(Callback<String, byte[]> callback);
}
