package fixture;
public class Provider {
    public String id;
    public Provider(String id) { this.id = id; }
    public String id() {
        Trace.hit(5, this, id, null);
        return id;
    }
}
