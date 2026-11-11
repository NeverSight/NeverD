package fixture;
public interface Root {
    default int measure(float value) {
        Trace.hit(1);Trace.seen=this;Trace.input=value;
        return Trace.finish(value,31);
    }
}
