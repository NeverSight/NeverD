package fixture;
public interface Root {
    default int measure(float value) {
        Trace.hit(1);Trace.seen=this;Trace.input=value;
        return Trace.finish(value,31);
    }
    default float measure(int value) {
        Trace.hit(1);Trace.seen=this;Trace.inputInt=value;
        return Trace.finishInt(value);
    }
    default double measure(long value) {
        Trace.hit(1);Trace.seen=this;Trace.inputLong=value;
        return Trace.finishLong(value);
    }
}
