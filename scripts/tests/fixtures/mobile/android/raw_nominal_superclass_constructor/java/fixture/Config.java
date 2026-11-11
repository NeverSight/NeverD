package fixture;
public final class Config {
    public Object tag;
    public Runnable action;
    public Config(Object tag, Runnable action) { this.tag = tag; this.action = action; }
}
