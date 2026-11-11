package fixture;
public final class Core<T> {
    private final T[] values;
    private final boolean reuse;
    public Core(T[] values, boolean reuse) { this.values = values; this.reuse = reuse; }
    public Object[] snapshot() {
        Object[] answer = Facade.adapt(values, reuse);
        Trace.hit(4);
        return answer;
    }
    public String[] narrowed() { return (String[]) snapshot(); }
    public void ignored() { snapshot(); Trace.hit(5); }
}
