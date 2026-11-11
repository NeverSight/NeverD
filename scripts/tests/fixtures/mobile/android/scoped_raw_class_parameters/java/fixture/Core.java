package fixture;
public class Core<T extends Button> extends Middle<T> {
    @Override public boolean read(Object marker, Button input, Object after) {
        return super.read(marker, (T) input, after);
    }
}
