package fixture;
public interface Right extends Root {
    @Override default int measure(float value) {
        Trace.hit(2);Trace.seen=this;Trace.input=value;
        return Trace.finish(value,53);
    }
}
