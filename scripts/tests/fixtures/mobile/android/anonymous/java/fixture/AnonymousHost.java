package fixture;

public class AnonymousHost {
    public static final Creator<String> CREATOR = new Creator<String>() {
        public String make(int value) { return "made"; }
        public String[] many(int size) { return new String[size]; }
        public String echo(String value) { return value; }
    };
    public AnonymousHost() {}
}
