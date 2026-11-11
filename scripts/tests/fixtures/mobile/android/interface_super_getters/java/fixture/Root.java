package fixture;
public interface Root {
    default Payload getStyle() { Trace.rootCalls++; return Trace.alternate; }
}
