package fixture;
public final class Core<T> implements View<T> {
    public final Config<T> config;
    public float bias;
    public Core(Config<T> config) {
        Trace.partial=this;Trace.hit(1);this.config=config;bias=Float.intBitsToFloat(0x7fc00000);Trace.hit(2);
    }
    public Core(Config<T> config,float bias) { this(config);Trace.hit(3);this.bias=bias;Trace.hit(4); }
    public Config<T> config() { return config; }
}
