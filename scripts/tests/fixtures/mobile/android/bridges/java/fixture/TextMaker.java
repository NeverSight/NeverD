package fixture;

public class TextMaker implements Maker<String> {
    public TextMaker() {}
    public String make(int value) { return "made"; }
    public String[] many(int size) { return new String[size]; }
    public String echo(String value) { return value; }
}
