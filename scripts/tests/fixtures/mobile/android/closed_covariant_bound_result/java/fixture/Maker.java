package fixture;
public interface Maker {
 <R extends Carrier<? extends Element>> R make(String label, java.io.OutputStream sink) throws java.io.IOException;
}
